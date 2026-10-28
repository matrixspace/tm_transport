//Functional tests for the multicast transport component.
//
//These are correctness tests, not latency measurements. They drive the
//component directly (no data flow graph) through the same entry points the
//importer/exporter use: multicast_getPublisher and
//multicast_addSubscriptionClient.
//
//Two of the cases are regressions for teardown bugs:
//- a subscription joined with interface=lo could not leave the group: the
//  leave call omitted the interface and the kernel answered EADDRNOTAVAIL,
//  which escaped from multicast_removeSubscriptionClient;
//- the receive io_context thread was detached, so it could still be inside
//  handleReceive while the component freed the subscription (use-after-free
//  at shutdown).
//
//Everything runs on the loopback interface: the group is joined on lo and the
//publisher sends out of lo, so no traffic leaves the machine. UDP multicast is
//lossy in general; the helpers below republish until a message arrives rather
//than sleeping for a fixed time. The test leaves no files or sockets behind.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <tm_kit/transport/multicast/MulticastComponent.hpp>

namespace basic = dev::cd606::tm::basic;
namespace transport = dev::cd606::tm::transport;
namespace multicast = dev::cd606::tm::transport::multicast;

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
    uint32_t x = seq*2654435761u+1;
    for (size_t ii=0; ii<size; ++ii) {
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

//Each case gets its own group:port so cases never see each other's datagrams.
//The base is derived from the pid to make concurrent CI runs independent.
int nextPort() {
    static int counter = 0;
    ++counter;
    return 41000+(static_cast<int>(::getpid())%10000)+counter;
}

transport::ConnectionLocator makeLocator(std::string const &group, int port,
                                         std::string const &mode) {
    transport::ConnectionLocator l {group, port};
    l = l.addProperty("interface", "lo");
    if (!mode.empty()) {
        l = l.addProperty("envelop", mode);
    }
    return l;
}

using Publisher = std::function<void(basic::ByteDataWithTopic &&, int)>;

//UDP multicast has no connection notification, and datagrams published before
//the receiver has joined the group are lost. Republish the same message until
//it arrives, or the deadline passes.
bool publishUntil(Publisher const &pub, std::string const &topic,
                  std::string const &content, Collector &c,
                  size_t want, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now()
        +std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        pub({topic, content}, 1);
        if (c.size() >= want) {
            return true;
        }
        sleepMs(2);
    }
    return false;
}

//publishUntil can publish one message several times before the receiver's
//count catches up, so a case asserts that every delivered copy is the expected
//one rather than that exactly one copy arrived.
bool allMatch(std::vector<std::pair<std::string, std::string>> const &got,
              std::string const &topic, std::string const &content) {
    if (got.empty()) {
        return false;
    }
    for (auto const &item : got) {
        if (item.first != topic || item.second != content) {
            return false;
        }
    }
    return true;
}

void removeIgnoringError(multicast::MulticastComponent &comp, uint32_t id) {
    try {
        comp.multicast_removeSubscriptionClient(id);
    } catch (std::exception const &e) {
        std::cerr << "removeSubscriptionClient threw: " << e.what() << std::endl;
        ++g_fail;
    }
}

//1: interface=lo subscription, receive, remove (the remove used to throw
//EADDRNOTAVAIL), then subscribe again on the same group:port in the same
//process and receive again.
void testResubscribeWithInterface() {
    auto port = nextPort();
    auto locator = makeLocator("239.255.42.10", port, "");
    multicast::MulticastComponent pubComponent;
    multicast::MulticastComponent subComponent;
    auto pub = pubComponent.multicast_getPublisher(locator);

    Collector first;
    auto id1 = subComponent.multicast_addSubscriptionClient(
        locator
        , multicast::MulticastComponent::NoTopicSelection {}
        , [&first](basic::ByteDataWithTopic &&d) { first.onData(std::move(d)); }
    );
    auto c1 = makeContent(1, 64);
    bool received1 = publishUntil(pub, "t.resub", c1, first, 1, 3000);

    bool removed = true;
    std::string removeError;
    try {
        subComponent.multicast_removeSubscriptionClient(id1);
    } catch (std::exception const &e) {
        removed = false;
        removeError = e.what();
    }

    Collector second;
    auto id2 = subComponent.multicast_addSubscriptionClient(
        locator
        , multicast::MulticastComponent::NoTopicSelection {}
        , [&second](basic::ByteDataWithTopic &&d) { second.onData(std::move(d)); }
    );
    auto c2 = makeContent(2, 64);
    bool received2 = publishUntil(pub, "t.resub", c2, second, 1, 3000);

    auto got1 = first.snapshot();
    auto got2 = second.snapshot();
    bool contentOk = received1 && allMatch(got1, "t.resub", c1)
        && received2 && allMatch(got2, "t.resub", c2);
    check(received1 && removed && contentOk,
          "1 subscribe interface=lo, remove, re-subscribe same group:port",
          "first="+std::to_string(got1.size())
          +" removed="+(removed ? "yes" : "no")
          +(removeError.empty() ? "" : " ("+removeError+")")
          +" second="+std::to_string(got2.size()));

    removeIgnoringError(subComponent, id2);
}

//2: two clients on the same subscription; removing one must not disturb the
//other.
void testMultiClientRemoveOne() {
    auto port = nextPort();
    auto locator = makeLocator("239.255.42.11", port, "");
    multicast::MulticastComponent pubComponent;
    multicast::MulticastComponent subComponent;
    auto pub = pubComponent.multicast_getPublisher(locator);

    Collector a, b;
    auto idA = subComponent.multicast_addSubscriptionClient(
        locator
        , multicast::MulticastComponent::NoTopicSelection {}
        , [&a](basic::ByteDataWithTopic &&d) { a.onData(std::move(d)); }
    );
    auto idB = subComponent.multicast_addSubscriptionClient(
        locator
        , multicast::MulticastComponent::NoTopicSelection {}
        , [&b](basic::ByteDataWithTopic &&d) { b.onData(std::move(d)); }
    );

    //joining the group is synchronous, so there is no connection setup to wait
    //for: remove A before the measured batch and B must still get all of it
    removeIgnoringError(subComponent, idA);
    sleepMs(10);

    const size_t n = 50;
    std::vector<std::string> sent;
    sent.reserve(n);
    for (size_t ii=0; ii<n; ++ii) {
        sent.push_back(makeContent(static_cast<uint32_t>(ii), 64));
        pub({"t.multi", sent.back()}, 1);
        sleepMs(2);
    }
    bool bArrived = b.waitFor(n, 3000);
    sleepMs(100);
    auto gotA = a.snapshot();
    auto gotB = b.snapshot();

    bool ok = bArrived && gotA.empty() && gotB.size() == n;
    std::string detail = "remaining="+std::to_string(gotB.size())+"/"
        +std::to_string(n)+" removed="+std::to_string(gotA.size());
    if (ok) {
        for (size_t ii=0; ii<n; ++ii) {
            if (gotB[ii].second != sent[ii]) {
                ok = false;
                detail += " mismatch at "+std::to_string(ii);
                break;
            }
        }
    }
    check(ok, "2 two clients, remove one, the other keeps receiving", detail);

    removeIgnoringError(subComponent, idB);
}

//3: all three envelop modes still deliver the same bytes they did before the
//teardown fix (and the topic where the envelop carries one). The wire format is
//frozen, so this pins the existing representation rather than an idealized one:
//the sender writes a 4-byte topic length + topic + content for both binary
//modes, "binary-adhoc" strips that header, and "binary" deliberately hands the
//whole datagram to the client as content (topic is not carried). The test uses
//the same expectation for both, so a change in either direction fails.
void testEnvelopModes() {
    struct Mode {
        std::string name;
        bool hasTopic;
    };
    std::vector<Mode> modes {
        {"cbor", true}
        , {"binary-adhoc", true}
        , {"binary", false}
    };
    for (auto const &mode : modes) {
        auto port = nextPort();
        auto locator = makeLocator("239.255.42.12", port, mode.name);
        multicast::MulticastComponent pubComponent;
        multicast::MulticastComponent subComponent;
        auto pub = pubComponent.multicast_getPublisher(locator);
        Collector c;
        auto id = subComponent.multicast_addSubscriptionClient(
            locator
            , multicast::MulticastComponent::NoTopicSelection {}
            , [&c](basic::ByteDataWithTopic &&d) { c.onData(std::move(d)); }
        );

        //include zero bytes and a topic so a truncation or an encoding change
        //shows up as a content difference
        auto topic = std::string("t.envelop");
        auto content = std::string("\x00\x01raw\x00payload\xff", 14);
        //the expected content of the "binary" mode is the full sender datagram
        std::string expectedContent = content;
        if (!mode.hasTopic) {
            uint32_t topicLen = static_cast<uint32_t>(topic.size());
            expectedContent.assign(reinterpret_cast<char const *>(&topicLen), sizeof(topicLen));
            expectedContent += topic;
            expectedContent += content;
        }
        bool arrived = publishUntil(pub, topic, content, c, 1, 3000);
        auto got = c.snapshot();
        bool ok = arrived && !got.empty();
        for (auto const &item : got) {
            if (item.second != expectedContent) {
                ok = false;
            }
            if (mode.hasTopic ? item.first != topic : !item.first.empty()) {
                ok = false;
            }
        }
        std::string detail = "got="+std::to_string(got.size());
        if (!ok && !got.empty()) {
            std::string hex;
            for (unsigned char ch : got[0].second) {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%02x", ch);
                hex += buf;
            }
            detail = "topic='"+got[0].first+"' len="
                +std::to_string(got[0].second.size())+"/"
                +std::to_string(expectedContent.size())+" hex="+hex;
        }
        check(ok, "3 envelop="+mode.name+" delivers the same bytes as before",
              detail);
        removeIgnoringError(subComponent, id);
    }
}

//4: repeated create/subscribe/receive/destroy cycles. Before the teardown fix
//the detached receive thread outlived the subscription and touched freed
//memory; under ASan (or with enough iterations) this case reports it.
void testCreateDestroyCycles() {
    const int iterations = 50;
    //reuse one group:port for every iteration: each teardown has to leave the
    //group cleanly before the next one joins it
    auto locator = makeLocator("239.255.42.13",
        46000+(static_cast<int>(::getpid())%5000), "");
    int failedAt = -1;
    std::string detail;
    for (int ii=0; ii<iterations && failedAt<0; ++ii) {
        multicast::MulticastComponent pubComponent;
        multicast::MulticastComponent subComponent;
        auto pub = pubComponent.multicast_getPublisher(locator);
        Collector c;
        auto id = subComponent.multicast_addSubscriptionClient(
            locator
            , multicast::MulticastComponent::NoTopicSelection {}
            , [&c](basic::ByteDataWithTopic &&d) { c.onData(std::move(d)); }
        );
        auto content = makeContent(static_cast<uint32_t>(ii), 32);
        if (!publishUntil(pub, "t.cycle", content, c, 1, 1000)) {
            failedAt = ii;
            detail = "no message at iteration "+std::to_string(ii);
            break;
        }
        //destroy with datagrams still in flight: this is the teardown path
        //that used to race the receive thread
        for (int kk=0; kk<8; ++kk) {
            pub({"t.cycle", content}, 1);
        }
        removeIgnoringError(subComponent, id);
    }
    check(failedAt < 0, "4 50 create/subscribe/receive/destroy cycles",
          failedAt < 0 ? "" : detail);
}

} //namespace

int main() {
    testResubscribeWithInterface();
    testMultiClientRemoveOne();
    testEnvelopModes();
    testCreateDestroyCycles();

    std::cout << "SUMMARY pass=" << g_pass << " fail=" << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
