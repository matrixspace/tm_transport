//Functional tests for the ZeroMQ transport component.
//
//These are correctness tests, not latency measurements. They drive the
//component directly (no data flow graph) through the same entry points the
//importer/exporter use: zeroMQ_getPublisher and zeroMQ_addSubscriptionClient.
//
//Each case uses its own ipc:// path under /dev/shm, so the cases do not
//interfere with each other and repeated runs do not inherit a stale socket.
//Each case also removes its socket file when it finishes: zeroMQ does not
//unlink the ipc file on close, so without PathCleanup every run would leave
//files behind.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <tm_kit/transport/zeromq/ZeroMQComponent.hpp>

namespace basic = dev::cd606::tm::basic;
namespace transport = dev::cd606::tm::transport;
namespace zeromq = dev::cd606::tm::transport::zeromq;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, std::string const &name, std::string const &detail = "") {
    if (ok) {
        std::cout << "PASS " << name;
        ++g_pass;
    } else {
        std::cout << "FAIL " << name;
        ++g_fail;
    }
    if (!detail.empty()) {
        std::cout << ": " << detail;
    }
    std::cout << std::endl;
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string makeContent(uint32_t seq, size_t size) {
    std::string s(size, '\0');
    if (size >= 4) {
        s[0] = static_cast<char>(seq & 0xff);
        s[1] = static_cast<char>((seq >> 8) & 0xff);
        s[2] = static_cast<char>((seq >> 16) & 0xff);
        s[3] = static_cast<char>((seq >> 24) & 0xff);
    }
    uint32_t x = seq*2654435761u+1;
    for (size_t ii=4; ii<size; ++ii) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s[ii] = static_cast<char>(x & 0xff);
    }
    return s;
}

struct Collector {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::pair<std::string, std::string>> got;

    void onData(basic::ByteDataWithTopic &&d) {
        {
            std::lock_guard<std::mutex> _(mutex);
            got.emplace_back(std::move(d.topic), std::move(d.content));
        }
        cv.notify_all();
    }

    size_t size() {
        std::lock_guard<std::mutex> _(mutex);
        return got.size();
    }

    bool waitFor(size_t n, int timeoutMs) {
        std::unique_lock<std::mutex> lk(mutex);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this,n]() {
            return got.size() >= n;
        });
    }

    void clear() {
        std::lock_guard<std::mutex> _(mutex);
        got.clear();
    }

    std::vector<std::pair<std::string, std::string>> snapshot() {
        std::lock_guard<std::mutex> _(mutex);
        return got;
    }
};

std::string nextPath(std::string const &tag) {
    static int counter = 0;
    ++counter;
    std::string p = "/dev/shm/tm_kit_zmq_test_"+std::to_string(::getpid())+"_"+tag+"_"+std::to_string(counter);
    ::unlink(p.c_str());
    return p;
}

//Removes the ipc file when the case ends. Declare it before the components:
//destruction runs in reverse order, so the file goes away only after both
//ends of the socket have been closed.
struct PathCleanup {
    std::string path;

    explicit PathCleanup(std::string const &p) : path(p) {}

    ~PathCleanup() {
        ::unlink(path.c_str());
    }

    PathCleanup(PathCleanup const &) = delete;
    PathCleanup &operator=(PathCleanup const &) = delete;
};

transport::ConnectionLocator makeLocator(std::string const &path) {
    return transport::ConnectionLocator {"ipc", 0, "", "", path};
}

using Publisher = std::function<void(basic::ByteDataWithTopic &&)>;

//PUB/SUB has no connection notification, and messages published before the
//subscription is established are dropped. Send a small message on each
//target's topic until every collector has seen one, then drain the leftovers.
bool warmup(Publisher const &pub, std::vector<std::pair<Collector *, std::string>> const &targets) {
    if (targets.empty()) {
        return true;
    }
    auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto const &t : targets) {
            pub({t.second, std::string(16, 'w')});
        }
        bool all = true;
        for (auto const &t : targets) {
            if (t.first->size() == 0) {
                all = false;
            }
        }
        if (all) {
            sleepMs(20);
            for (auto const &t : targets) {
                t.first->clear();
            }
            return true;
        }
        sleepMs(2);
    }
    return false;
}

void publishPaced(Publisher const &pub, std::string const &topic,
                  std::vector<std::string> const &contents, int intervalUs) {
    for (auto const &c : contents) {
        pub({topic, c});
        if (intervalUs > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(intervalUs));
        }
    }
}

//1: one NoTopicSelection client, 10000 messages, order and content
void testSingleClient() {
    std::string path = nextPath("single");
    PathCleanup cleanup {path};
    zeromq::ZeroMQComponent pubComponent;
    zeromq::ZeroMQComponent subComponent;
    auto locator = makeLocator(path);
    auto pub = pubComponent.zeroMQ_getPublisher(locator);

    Collector c;
    auto id = subComponent.zeroMQ_addSubscriptionClient(
        locator
        , zeromq::ZeroMQComponent::NoTopicSelection {}
        , [&c](basic::ByteDataWithTopic &&d) { c.onData(std::move(d)); }
    );
    bool connected = warmup(pub, {{&c, "t.single"}});

    const size_t n = 10000;
    std::vector<std::string> sent(n);
    for (size_t ii=0; ii<n; ++ii) {
        sent[ii] = makeContent(static_cast<uint32_t>(ii), 64);
    }
    publishPaced(pub, "t.single", sent, 50);
    bool arrived = c.waitFor(n, 10000);
    auto got = c.snapshot();

    bool ok = connected && arrived && got.size() == n;
    std::string detail = "connected="+std::to_string(connected)+" received="+std::to_string(got.size());
    if (ok) {
        for (size_t ii=0; ii<n; ++ii) {
            if (got[ii].first != "t.single" || got[ii].second != sent[ii]) {
                ok = false;
                detail += " first mismatch at "+std::to_string(ii);
                break;
            }
        }
    }
    check(ok, "1 single NoTopicSelection client, 10000 messages in order", detail);

    subComponent.zeroMQ_removeSubscriptionClient(id);
}

//2: string topic filtering, match and no-match
void testStringTopics() {
    std::string path = nextPath("string");
    PathCleanup cleanup {path};
    zeromq::ZeroMQComponent pubComponent;
    zeromq::ZeroMQComponent subComponent;
    auto locator = makeLocator(path);
    auto pub = pubComponent.zeroMQ_getPublisher(locator);

    Collector ax, ay, none;
    auto idAX = subComponent.zeroMQ_addSubscriptionClient(
        locator, std::string("a.x")
        , [&ax](basic::ByteDataWithTopic &&d) { ax.onData(std::move(d)); });
    auto idAY = subComponent.zeroMQ_addSubscriptionClient(
        locator, std::string("a.y")
        , [&ay](basic::ByteDataWithTopic &&d) { ay.onData(std::move(d)); });
    auto idNone = subComponent.zeroMQ_addSubscriptionClient(
        locator, std::string("none")
        , [&none](basic::ByteDataWithTopic &&d) { none.onData(std::move(d)); });

    bool connected = warmup(pub, {{&ax, "a.x"}, {&ay, "a.y"}});

    const size_t n = 500;
    std::vector<std::string> sentX(n), sentY(n);
    for (size_t ii=0; ii<n; ++ii) {
        sentX[ii] = makeContent(static_cast<uint32_t>(ii), 64);
        sentY[ii] = makeContent(static_cast<uint32_t>(ii+n), 64);
    }
    for (size_t ii=0; ii<n; ++ii) {
        pub({"a.x", sentX[ii]});
        pub({"a.y", sentY[ii]});
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    bool arrived = ax.waitFor(n, 10000) && ay.waitFor(n, 10000);
    sleepMs(100);
    auto gotX = ax.snapshot();
    auto gotY = ay.snapshot();
    auto gotNone = none.snapshot();

    bool ok = connected && arrived && gotX.size() == n && gotY.size() == n && gotNone.empty();
    std::string detail = "a.x="+std::to_string(gotX.size())+" a.y="+std::to_string(gotY.size())
        +" none="+std::to_string(gotNone.size());
    if (ok) {
        for (size_t ii=0; ii<n; ++ii) {
            if (gotX[ii].first != "a.x" || gotX[ii].second != sentX[ii]
                || gotY[ii].first != "a.y" || gotY[ii].second != sentY[ii]) {
                ok = false;
                detail += " mismatch at "+std::to_string(ii);
                break;
            }
        }
    }
    check(ok, "2 string topic match and no-match", detail);

    subComponent.zeroMQ_removeSubscriptionClient(idAX);
    subComponent.zeroMQ_removeSubscriptionClient(idAY);
    subComponent.zeroMQ_removeSubscriptionClient(idNone);
}

//3: two NoTopicSelection clients on one locator (the copy path, not the move path)
void testTwoNoFilterClients() {
    std::string path = nextPath("two");
    PathCleanup cleanup {path};
    zeromq::ZeroMQComponent pubComponent;
    zeromq::ZeroMQComponent subComponent;
    auto locator = makeLocator(path);
    auto pub = pubComponent.zeroMQ_getPublisher(locator);

    Collector a, b;
    auto idA = subComponent.zeroMQ_addSubscriptionClient(
        locator, zeromq::ZeroMQComponent::NoTopicSelection {}
        , [&a](basic::ByteDataWithTopic &&d) { a.onData(std::move(d)); });
    auto idB = subComponent.zeroMQ_addSubscriptionClient(
        locator, zeromq::ZeroMQComponent::NoTopicSelection {}
        , [&b](basic::ByteDataWithTopic &&d) { b.onData(std::move(d)); });

    bool connected = warmup(pub, {{&a, "t.two"}, {&b, "t.two"}});

    const size_t n = 1000;
    std::vector<std::string> sent(n);
    for (size_t ii=0; ii<n; ++ii) {
        sent[ii] = makeContent(static_cast<uint32_t>(ii), 256);
    }
    publishPaced(pub, "t.two", sent, 50);
    bool arrived = a.waitFor(n, 10000) && b.waitFor(n, 10000);
    auto gotA = a.snapshot();
    auto gotB = b.snapshot();

    bool ok = connected && arrived && gotA.size() == n && gotB.size() == n;
    std::string detail = "A="+std::to_string(gotA.size())+" B="+std::to_string(gotB.size());
    if (ok) {
        for (size_t ii=0; ii<n; ++ii) {
            if (gotA[ii].second != sent[ii] || gotB[ii].second != sent[ii]) {
                ok = false;
                detail += " mismatch at "+std::to_string(ii);
                break;
            }
        }
    }
    check(ok, "3 two NoTopicSelection clients both receive independent copies", detail);

    subComponent.zeroMQ_removeSubscriptionClient(idA);
    subComponent.zeroMQ_removeSubscriptionClient(idB);
}

//4: one no-filter client and one string-filter client sharing the subscription
void testNoFilterPlusFilter() {
    std::string path = nextPath("mixed");
    PathCleanup cleanup {path};
    zeromq::ZeroMQComponent pubComponent;
    zeromq::ZeroMQComponent subComponent;
    auto locator = makeLocator(path);
    auto pub = pubComponent.zeroMQ_getPublisher(locator);

    Collector all, onlyX;
    auto idAll = subComponent.zeroMQ_addSubscriptionClient(
        locator, zeromq::ZeroMQComponent::NoTopicSelection {}
        , [&all](basic::ByteDataWithTopic &&d) { all.onData(std::move(d)); });
    auto idX = subComponent.zeroMQ_addSubscriptionClient(
        locator, std::string("a.x")
        , [&onlyX](basic::ByteDataWithTopic &&d) { onlyX.onData(std::move(d)); });

    bool connected = warmup(pub, {{&all, "a.x"}, {&onlyX, "a.x"}});

    const size_t perTopic = 500;
    std::vector<std::string> sentAll, sentX;
    for (size_t ii=0; ii<perTopic; ++ii) {
        auto cx = makeContent(static_cast<uint32_t>(ii), 64);
        auto cy = makeContent(static_cast<uint32_t>(ii+perTopic), 64);
        pub({"a.x", cx});
        pub({"a.y", cy});
        sentAll.push_back(cx);
        sentAll.push_back(cy);
        sentX.push_back(cx);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    bool arrived = all.waitFor(sentAll.size(), 10000) && onlyX.waitFor(sentX.size(), 10000);
    sleepMs(100);
    auto gotAll = all.snapshot();
    auto gotX = onlyX.snapshot();

    bool ok = connected && arrived && gotAll.size() == sentAll.size() && gotX.size() == sentX.size();
    std::string detail = "all="+std::to_string(gotAll.size())+"/"+std::to_string(sentAll.size())
        +" a.x="+std::to_string(gotX.size())+"/"+std::to_string(sentX.size());
    if (ok) {
        for (size_t ii=0; ii<sentAll.size(); ++ii) {
            if (gotAll[ii].second != sentAll[ii]) {
                ok = false;
                detail += " all mismatch at "+std::to_string(ii);
                break;
            }
        }
    }
    if (ok) {
        for (size_t ii=0; ii<sentX.size(); ++ii) {
            if (gotX[ii].first != "a.x" || gotX[ii].second != sentX[ii]) {
                ok = false;
                detail += " a.x mismatch at "+std::to_string(ii);
                break;
            }
        }
    }
    check(ok, "4 no-filter client plus string-filter client", detail);

    subComponent.zeroMQ_removeSubscriptionClient(idAll);
    subComponent.zeroMQ_removeSubscriptionClient(idX);
}

//5: wire hooks on both sides
void testHooks() {
    std::string path = nextPath("hooks");
    PathCleanup cleanup {path};
    zeromq::ZeroMQComponent pubComponent;
    zeromq::ZeroMQComponent subComponent;
    auto locator = makeLocator(path);

    transport::UserToWireHook userToWire {
        [](basic::ByteData &&d) -> basic::ByteData {
            std::string out = std::move(d.content);
            out.push_back(static_cast<char>(0xab));
            return basic::ByteData {std::move(out)};
        }
    };
    transport::WireToUserHook wireToUser {
        [](basic::ByteDataView const &d) -> std::optional<basic::ByteData> {
            std::string s {d.content};
            if (s.empty() || static_cast<uint8_t>(s.back()) != 0xab) {
                return std::nullopt;
            }
            s.pop_back();
            return basic::ByteData {std::move(s)};
        }
    };

    auto pub = pubComponent.zeroMQ_getPublisher(locator, userToWire);

    Collector c;
    auto id = subComponent.zeroMQ_addSubscriptionClient(
        locator
        , zeromq::ZeroMQComponent::NoTopicSelection {}
        , [&c](basic::ByteDataWithTopic &&d) { c.onData(std::move(d)); }
        , wireToUser
    );
    //warmup messages go through the same hook, so they arrive as 16 bytes too
    bool connected = warmup(pub, {{&c, "t.hooks"}});

    const size_t n = 1000;
    std::vector<std::string> sent(n);
    for (size_t ii=0; ii<n; ++ii) {
        sent[ii] = makeContent(static_cast<uint32_t>(ii), 128);
    }
    publishPaced(pub, "t.hooks", sent, 50);
    bool arrived = c.waitFor(n, 10000);
    auto got = c.snapshot();

    bool ok = connected && arrived && got.size() == n;
    std::string detail = "connected="+std::to_string(connected)+" received="+std::to_string(got.size());
    if (ok) {
        for (size_t ii=0; ii<n; ++ii) {
            if (got[ii].first != "t.hooks" || got[ii].second != sent[ii]) {
                ok = false;
                detail += " mismatch at "+std::to_string(ii);
                break;
            }
        }
    }
    check(ok, "5 user-to-wire and wire-to-user hooks", detail);

    subComponent.zeroMQ_removeSubscriptionClient(id);
}

//6: payload sizes that cross the CBOR length encodings, plus long topics
void testPayloadSizes() {
    std::string path = nextPath("sizes");
    PathCleanup cleanup {path};
    zeromq::ZeroMQComponent pubComponent;
    zeromq::ZeroMQComponent subComponent;
    auto locator = makeLocator(path);
    auto pub = pubComponent.zeroMQ_getPublisher(locator);

    Collector c;
    auto id = subComponent.zeroMQ_addSubscriptionClient(
        locator
        , zeromq::ZeroMQComponent::NoTopicSelection {}
        , [&c](basic::ByteDataWithTopic &&d) { c.onData(std::move(d)); });
    bool connected = warmup(pub, {{&c, "t.sizes"}});

    std::vector<size_t> sizes {0, 1, 23, 24, 255, 256, 65535, 65536};
    std::vector<std::string> sent, sentTopics;
    for (size_t size : sizes) {
        for (int ii=0; ii<20; ++ii) {
            sent.push_back(makeContent(static_cast<uint32_t>(sent.size()), size));
            sentTopics.push_back("t.sizes");
        }
    }
    //topic lengths that cross the same encoding boundaries
    std::vector<size_t> topicSizes {0, 11, 23, 24, 255, 256, 300};
    for (size_t tl : topicSizes) {
        std::string topic(tl, 'p');
        sent.push_back(makeContent(static_cast<uint32_t>(sent.size()), 32));
        sentTopics.push_back(topic);
    }

    for (size_t ii=0; ii<sent.size(); ++ii) {
        pub({sentTopics[ii], sent[ii]});
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    bool arrived = c.waitFor(sent.size(), 15000);
    auto got = c.snapshot();

    bool ok = connected && arrived && got.size() == sent.size();
    std::string detail = "connected="+std::to_string(connected)+" received="+std::to_string(got.size())
        +"/"+std::to_string(sent.size());
    if (ok) {
        for (size_t ii=0; ii<sent.size(); ++ii) {
            if (got[ii].first != sentTopics[ii] || got[ii].second != sent[ii]) {
                ok = false;
                detail += " mismatch at "+std::to_string(ii)
                    +" (topicLen "+std::to_string(got[ii].first.size())+"/"+std::to_string(sentTopics[ii].size())
                    +", contentLen "+std::to_string(got[ii].second.size())+"/"+std::to_string(sent[ii].size())+")";
                break;
            }
        }
    }
    check(ok, "6 payload sizes 0..65536 and topic length boundaries", detail);

    subComponent.zeroMQ_removeSubscriptionClient(id);
}

//7: start/stop at the component level: the same operations behind the
//importer's stop and restart control commands
void testStartStop() {
    std::string path = nextPath("stop");
    PathCleanup cleanup {path};
    zeromq::ZeroMQComponent pubComponent;
    zeromq::ZeroMQComponent subComponent;
    auto locator = makeLocator(path);
    auto pub = pubComponent.zeroMQ_getPublisher(locator);

    Collector first;
    auto id1 = subComponent.zeroMQ_addSubscriptionClient(
        locator, zeromq::ZeroMQComponent::NoTopicSelection {}
        , [&first](basic::ByteDataWithTopic &&d) { first.onData(std::move(d)); });
    bool connected1 = warmup(pub, {{&first, "t.stop"}});

    const size_t n = 200;
    std::vector<std::string> sent1(n), sent2(n);
    for (size_t ii=0; ii<n; ++ii) {
        sent1[ii] = makeContent(static_cast<uint32_t>(ii), 64);
        sent2[ii] = makeContent(static_cast<uint32_t>(ii+n), 64);
    }
    publishPaced(pub, "t.stop", sent1, 100);
    bool arrived1 = first.waitFor(n, 10000);
    auto got1 = first.snapshot();

    subComponent.zeroMQ_removeSubscriptionClient(id1);
    //publishing with no subscriber must not crash and must not be delivered later
    publishPaced(pub, "t.stop", sent2, 100);
    sleepMs(200);

    Collector second;
    auto id2 = subComponent.zeroMQ_addSubscriptionClient(
        locator, zeromq::ZeroMQComponent::NoTopicSelection {}
        , [&second](basic::ByteDataWithTopic &&d) { second.onData(std::move(d)); });
    bool connected2 = warmup(pub, {{&second, "t.stop"}});
    auto sent3 = sent1;
    publishPaced(pub, "t.stop", sent3, 100);
    bool arrived2 = second.waitFor(n, 10000);
    auto got2 = second.snapshot();

    bool ok = connected1 && connected2 && arrived1 && arrived2
        && got1.size() == n && got2.size() == n;
    std::string detail = "before-stop="+std::to_string(got1.size())
        +" after-restart="+std::to_string(got2.size())+"/"+std::to_string(n);
    if (ok) {
        for (size_t ii=0; ii<n; ++ii) {
            if (got1[ii].second != sent1[ii] || got2[ii].second != sent3[ii]) {
                ok = false;
                detail += " mismatch at "+std::to_string(ii);
                break;
            }
        }
    }
    check(ok, "7 stop and restart subscription (add/remove client)", detail);

    subComponent.zeroMQ_removeSubscriptionClient(id2);
}

//8: shutdown does not block with queued/slow data (linger=0 on both sockets)
void testShutdown() {
    auto start = std::chrono::steady_clock::now();

    //publisher with no subscriber at all: 5000 messages are queued nowhere
    {
        std::string path = nextPath("shutdown_pub");
        PathCleanup cleanup {path};
        zeromq::ZeroMQComponent pubComponent;
        auto locator = makeLocator(path);
        auto pub = pubComponent.zeroMQ_getPublisher(locator);
        for (int ii=0; ii<5000; ++ii) {
            pub({"t.shutdown", makeContent(static_cast<uint32_t>(ii), 256)});
        }
    }

    //publisher and subscriber destroyed while messages are still queued
    {
        std::string path = nextPath("shutdown_pair");
        PathCleanup cleanup {path};
        zeromq::ZeroMQComponent pubComponent;
        zeromq::ZeroMQComponent subComponent;
        auto locator = makeLocator(path);
        auto pub = pubComponent.zeroMQ_getPublisher(locator);
        Collector c;
        auto id = subComponent.zeroMQ_addSubscriptionClient(
            locator, zeromq::ZeroMQComponent::NoTopicSelection {}
            , [&c](basic::ByteDataWithTopic &&d) { c.onData(std::move(d)); });
        warmup(pub, {{&c, "t.shutdown"}});
        //do not wait for these: destroy with data in flight
        for (int ii=0; ii<5000; ++ii) {
            pub({"t.shutdown", makeContent(static_cast<uint32_t>(ii), 256)});
        }
        subComponent.zeroMQ_removeSubscriptionClient(id);
    }

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-start).count();
    check(elapsedMs < 3000, "8 shutdown with queued data returns promptly",
          "elapsed="+std::to_string(elapsedMs)+"ms");
}

//9: the optimized in-place CBOR path produces exactly the bytes and values of
//the allocating path
void testCborEquivalence() {
    namespace bu = basic::bytedata_utils;
    std::vector<std::string> topics {"", "a", "ipc.profile", std::string(23, 't'), std::string(24, 't'), std::string(300, 't')};
    std::vector<size_t> sizes {0, 1, 23, 24, 255, 256, 65535, 65536};

    bool ok = true;
    std::string detail;
    basic::ByteDataWithTopic reused;
    for (auto const &topic : topics) {
        for (size_t size : sizes) {
            basic::ByteDataWithTopic in {topic, makeContent(static_cast<uint32_t>(size), size)};

            std::string allocating = bu::RunCBORSerializer<basic::ByteDataWithTopic>::apply(in);
            std::string reusedBytes;
            auto n = bu::RunCBORSerializer<basic::ByteDataWithTopic>::calculateSize(in);
            reusedBytes.resize(n);
            auto written = bu::RunCBORSerializer<basic::ByteDataWithTopic>::apply(in, reusedBytes.data());
            if (allocating != reusedBytes || written != n) {
                ok = false;
                detail = "encode mismatch topicLen="+std::to_string(topic.size())
                    +" size="+std::to_string(size);
                break;
            }

            auto decoded = bu::RunCBORDeserializer<basic::ByteDataWithTopic>::apply(reusedBytes, 0);
            auto decodedLen = bu::RunCBORDeserializer<basic::ByteDataWithTopic>::applyInPlace(reused, reusedBytes, 0);
            if (!decoded || !decodedLen || *decodedLen != reusedBytes.size()
                || std::get<1>(*decoded) != reusedBytes.size()
                || std::get<0>(*decoded).topic != in.topic
                || std::get<0>(*decoded).content != in.content
                || reused.topic != in.topic || reused.content != in.content) {
                ok = false;
                detail = "decode mismatch topicLen="+std::to_string(topic.size())
                    +" size="+std::to_string(size);
                break;
            }
        }
        if (!ok) {
            break;
        }
    }
    check(ok, "9 CBOR allocating and in-place paths are equivalent", detail);
}

//10: hwm/sndhwm/rcvhwm locator properties are accepted and data still flows
void testHwmProperties() {
    bool ok = true;
    std::string detail;
    std::vector<std::vector<std::pair<std::string, std::string>>> cases {
        {{"hwm", "50000"}}
        , {{"sndhwm", "0"}, {"rcvhwm", "0"}}
        , {{"hwm", "10"}, {"sndhwm", "20000"}, {"rcvhwm", "20000"}}
    };
    for (std::size_t ci=0; ci<cases.size() && ok; ++ci) {
        std::string path = nextPath("hwm");
        PathCleanup cleanup {path};
        zeromq::ZeroMQComponent pubComponent;
        zeromq::ZeroMQComponent subComponent;
        auto locator = makeLocator(path);
        for (auto const &kv : cases[ci]) {
            locator = locator.addProperty(kv.first, kv.second);
        }
        auto pub = pubComponent.zeroMQ_getPublisher(locator);
        Collector c;
        auto id = subComponent.zeroMQ_addSubscriptionClient(
            locator, zeromq::ZeroMQComponent::NoTopicSelection {}
            , [&c](basic::ByteDataWithTopic &&d) { c.onData(std::move(d)); });
        if (!warmup(pub, {{&c, "t.hwm"}})) {
            ok = false;
            detail = "warmup failed case="+std::to_string(ci);
        } else {
            //a burst larger than the libzmq default hwm (1000), unpaced
            const std::size_t n = 5000;
            for (std::size_t ii=0; ii<n; ++ii) {
                pub({"t.hwm", makeContent(static_cast<uint32_t>(ii), 64)});
            }
            if (!c.waitFor(n, 5000)) {
                ok = false;
                detail = "case="+std::to_string(ci)+" got="+std::to_string(c.size())+"/"+std::to_string(n);
            }
        }
        subComponent.zeroMQ_removeSubscriptionClient(id);
    }
    check(ok, "10 hwm locator properties", detail);
}

//11: invalid hwm values are rejected with std::invalid_argument
void testHwmInvalid() {
    bool ok = true;
    std::string detail;
    std::vector<std::pair<std::string, std::string>> bad {
        {"hwm", "abc"}, {"hwm", "-1"}, {"sndhwm", "12x"}, {"rcvhwm", "99999999999"}
    };
    for (auto const &kv : bad) {
        std::string path = nextPath("hwm_bad");
        PathCleanup cleanup {path};
        zeromq::ZeroMQComponent comp;
        auto locator = makeLocator(path).addProperty(kv.first, kv.second);
        bool threw = false;
        try {
            if (kv.first == "rcvhwm") {
                comp.zeroMQ_addSubscriptionClient(locator, zeromq::ZeroMQComponent::NoTopicSelection {}
                    , [](basic::ByteDataWithTopic &&) {});
            } else {
                comp.zeroMQ_getPublisher(locator);
            }
        } catch (std::invalid_argument const &) {
            threw = true;
        }
        if (!threw) {
            ok = false;
            detail = kv.first+"="+kv.second+" did not throw";
            break;
        }
    }
    check(ok, "11 invalid hwm values throw", detail);
}

} //namespace

int main() {
    testCborEquivalence();
    testSingleClient();
    testStringTopics();
    testTwoNoFilterClients();
    testNoFilterPlusFilter();
    testHooks();
    testPayloadSizes();
    testStartStop();
    testShutdown();
    testHwmProperties();
    testHwmInvalid();

    std::cout << "SUMMARY pass=" << g_pass << " fail=" << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
