// Functional test program for the shm_cursor transport.
//
// Modes (all options are --key=value):
//   --mode=pub     publisher process, goes through MultiTransportBroadcastPublisherManagingUtils
//   --mode=sub     subscriber process, goes through MultiTransportBroadcastListenerManagingUtils
//                  (or the SynchronousRunner variant with --sync, or
//                  fetchFirstUpdateAndDisconnect with --fetch)
//   --mode=stress  in-process torn-read stress test under heavy overwrite (correctness only,
//                  not a latency measurement)
// Every mode prints exactly one line starting with "RESULT " that run_tests.sh parses.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/TerminationController.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/infra/SynchronousRunner.hpp>

#include <tm_kit/basic/ByteData.hpp>
#include <tm_kit/basic/TrivialBoostLoggingComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>

#include <tm_kit/transport/BoostUUIDComponent.hpp>
#include <tm_kit/transport/HeartbeatAndAlertComponent.hpp>
#include <tm_kit/transport/MultiTransportBroadcastListenerManagingUtils.hpp>
#include <tm_kit/transport/MultiTransportBroadcastListenerManagingUtils_SynchronousRunner.hpp>
#include <tm_kit/transport/MultiTransportBroadcastPublisherManagingUtils.hpp>
#include <tm_kit/transport/shm_cursor/ShmCursorComponent.hpp>

using namespace dev::cd606::tm;

using TheEnvironment = infra::Environment<
    infra::CheckTimeComponent<false>,
    infra::TrivialExitControlComponent,
    basic::TrivialBoostLoggingComponent,
    basic::real_time_clock::ClockComponent,
    transport::BoostUUIDComponent,
    transport::AllNetworkTransportComponents,
    transport::HeartbeatAndAlertComponent>;
using M = infra::RealTimeApp<TheEnvironment>;
using R = infra::AppRunner<M>;

// MultiTransportBroadcastFirstUpdateQueryManagingUtils cannot be instantiated with
// AllNetworkTransportComponents: WebSocketImporterExporter::fetchFirstUpdateAndDisconnect
// has an ambiguous websocket_addSubscriptionClient call (pre-existing, unrelated to
// shm_cursor), so the fetch path uses an environment with only the shm_cursor transport
using FetchEnvironment = infra::Environment<
    infra::CheckTimeComponent<false>,
    infra::TrivialExitControlComponent,
    basic::TrivialBoostLoggingComponent,
    basic::real_time_clock::ClockComponent,
    transport::BoostUUIDComponent,
    transport::shm_cursor::ShmCursorComponent>;

namespace {
    class Args {
    private:
        std::unordered_map<std::string, std::string> kv_;

    public:
        Args(int argc, char **argv)
            : kv_() {
            for (int ii = 1; ii < argc; ++ii) {
                std::string s = argv[ii];
                if (s.rfind("--", 0) != 0) {
                    throw std::runtime_error("bad argument '" + s + "', expected --key=value");
                }
                auto pos = s.find('=');
                if (pos == std::string::npos) {
                    kv_[s.substr(2)] = "true";
                } else {
                    kv_[s.substr(2, pos - 2)] = s.substr(pos + 1);
                }
            }
        }
        bool has(std::string const &k) const {
            return kv_.find(k) != kv_.end();
        }
        std::string get(std::string const &k, std::string const &d = "") const {
            auto iter = kv_.find(k);
            return (iter == kv_.end()) ? d : iter->second;
        }
        int64_t num(std::string const &k, int64_t d) const {
            auto iter = kv_.find(k);
            return (iter == kv_.end()) ? d : std::stoll(iter->second);
        }
        bool flag(std::string const &k) const {
            return get(k, "false") == "true";
        }
    };

    std::vector<std::string> splitComma(std::string const &s) {
        std::vector<std::string> ret;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) {
                ret.push_back(item);
            }
        }
        return ret;
    }

    int64_t nowNs() {
        struct timespec ts;
        ::clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
    }

    [[noreturn]] void finish(std::string const &line) {
        std::cout << "RESULT " << line << std::endl;
        std::fflush(stdout);
        std::fflush(stderr);
        // skip destructors: reader threads and graph threads are still running
        std::_Exit(0);
    }

    // message content: header followed by a pattern derived from (seq, pubId)
    struct MsgHeader {
        uint64_t seq;
        uint64_t pubId;
        int64_t sendNs;
        uint32_t len;
        uint32_t topicTag;
    };
    static_assert(sizeof(MsgHeader) == 32, "unexpected test message header size");

    inline uint8_t patternByte(uint64_t seq, uint64_t pubId, std::size_t k) {
        return static_cast<uint8_t>(seq * 131u + k * 7u + pubId);
    }
    void fillMessage(std::string &out, uint64_t seq, uint64_t pubId, std::size_t len, uint32_t topicTag) {
        out.resize(len);
        MsgHeader h{seq, pubId, nowNs(), static_cast<uint32_t>(len), topicTag};
        std::memcpy(out.data(), &h, sizeof(h));
        for (std::size_t k = sizeof(h); k < len; ++k) {
            out[k] = static_cast<char>(patternByte(seq, pubId, k));
        }
    }
    bool checkMessage(std::string_view c, MsgHeader &h) {
        if (c.size() < sizeof(MsgHeader)) {
            return false;
        }
        std::memcpy(&h, c.data(), sizeof(h));
        if (h.len != c.size()) {
            return false;
        }
        for (std::size_t k = sizeof(h); k < c.size(); ++k) {
            if (static_cast<uint8_t>(c[k]) != patternByte(h.seq, h.pubId, k)) {
                return false;
            }
        }
        return true;
    }

    uint64_t randomId() {
        std::random_device rd;
        return (static_cast<uint64_t>(rd()) << 32) ^ rd() ^ static_cast<uint64_t>(::getpid()) ^ static_cast<uint64_t>(nowNs());
    }

    transport::ConnectionLocator locatorOf(std::string const &channel) {
        auto parsed = transport::parseMultiTransportBroadcastChannel(channel);
        if (!parsed || std::get<0>(*parsed) != transport::MultiTransportBroadcastListenerConnectionType::ShmCursor) {
            throw std::runtime_error("'" + channel + "' is not a shm_cursor channel");
        }
        return std::get<1>(*parsed);
    }

    // ---------------------------------------------------------------- publisher

    template <bool Typed>
    void runPublisher(Args const &a) {
        using D = std::conditional_t<Typed, basic::TypedDataWithTopic<std::string>, basic::ByteDataWithTopic>;

        std::string const channel = a.get("channel");
        auto const locator = locatorOf(channel);
        uint64_t const count = static_cast<uint64_t>(a.num("count", 100000));
        int64_t const rate = a.num("rate", 0);
        std::size_t const size = static_cast<std::size_t>(a.num("size", 256));
        auto const topics = splitComma(a.get("topics", "t.a"));
        uint64_t const oversizeEvery = static_cast<uint64_t>(a.num("oversize-every", 0));
        int64_t const lingerMs = a.num("linger-ms", 500);
        // lets readers that are polling for the segment attach before the first message
        int64_t const startDelayMs = a.num("start-delay-ms", 300);
        std::size_t const capacity = static_cast<std::size_t>(std::stoul(locator.query("slotSize", "1024"))) - 16;
        std::string const heartbeatChannel = a.get("heartbeat-channel");

        TheEnvironment env;
        R r(&env);
        if (!heartbeatChannel.empty()) {
            transport::initializeHeartbeatAndAlertComponent(&env, "shm_cursor_test_pub", heartbeatChannel);
        }

        auto importer = M::simpleImporter<D>(
            [&env, locator, count, rate, size, topics, oversizeEvery, lingerMs, startDelayMs, capacity](M::PublisherCall<D> &pub) {
                uint64_t const pubId = randomId();
                std::this_thread::sleep_for(std::chrono::milliseconds(startDelayMs));
                uint64_t expectedRejected = 0;
                auto const t0 = std::chrono::steady_clock::now();
                for (uint64_t i = 1; count == 0 || i <= count; ++i) {
                    if (rate > 0) {
                        auto target = t0 + std::chrono::nanoseconds(static_cast<int64_t>((i - 1) * (1000000000.0 / rate)));
                        auto now = std::chrono::steady_clock::now();
                        if (target - now > std::chrono::microseconds(200)) {
                            std::this_thread::sleep_for(target - now - std::chrono::microseconds(100));
                        }
                        while (std::chrono::steady_clock::now() < target) {
                        }
                    }
                    uint32_t const tag = static_cast<uint32_t>(i % topics.size());
                    std::string const &topic = topics[tag];
                    std::size_t len = size;
                    if (oversizeEvery > 0 && i % oversizeEvery == 0) {
                        if ((i / oversizeEvery) % 2 == 1) {
                            // exactly fits: topic+content == capacity, must be delivered
                            len = capacity - topic.size();
                        } else {
                            // one byte too large, must be rejected
                            len = capacity - topic.size() + 1;
                            ++expectedRejected;
                        }
                    }
                    D d;
                    d.topic = topic;
                    fillMessage(d.content, i, pubId, len, tag);
                    pub(std::move(d));
                }
                auto const t1 = std::chrono::steady_clock::now();
                double const elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                std::this_thread::sleep_for(std::chrono::milliseconds(lingerMs));
                std::ostringstream oss;
                oss << "role=pub pubId=" << pubId
                    << " sent=" << count
                    << " rejected=" << env.shm_cursor_publisherRejectedCount(locator)
                    << " expectedRejected=" << expectedRejected
                    << " elapsedMs=" << elapsedMs
                    << " msgPerSec=" << (elapsedMs > 0 ? (count * 1000.0 / elapsedMs) : 0.0);
                finish(oss.str());
            },
            infra::LiftParameters<M::TimePoint>().SuggestThreaded(true));
        if constexpr (Typed) {
            auto sink = transport::MultiTransportBroadcastPublisherManagingUtils<R>::oneBroadcastPublisher<std::string>(r, "pub", channel);
            r.connect(r.importItem("source", importer), sink);
        } else {
            auto sink = transport::MultiTransportBroadcastPublisherManagingUtils<R>::oneByteDataBroadcastPublisher(r, "pub", channel);
            r.connect(r.importItem("source", importer), sink);
        }
        if (!heartbeatChannel.empty()) {
            transport::attachHeartbeatAndAlertComponent(r, &env, "shm_cursor_test.heartbeat", std::chrono::milliseconds(100));
        }
        r.finalize();
        infra::terminationController(infra::TerminateAfterDuration{std::chrono::hours(24)});
    }

    // ---------------------------------------------------------------- subscriber

    struct SubStats {
        std::vector<std::string> topics;
        std::unordered_set<std::string> expectTopics;
        bool raw;
        int64_t sleepUs;

        std::atomic<uint64_t> received{0};
        std::atomic<uint64_t> corrupt{0};
        std::atomic<uint64_t> wrongTopic{0};
        std::atomic<uint64_t> outOfOrder{0};
        std::atomic<uint64_t> gaps{0};
        std::atomic<uint64_t> pubSwitches{0};
        std::atomic<uint64_t> lastPubReceived{0};
        std::atomic<uint64_t> firstSeq{0};
        std::atomic<uint64_t> lastSeq{0};
        std::atomic<int64_t> firstAgeUs{-1};
        std::atomic<int64_t> lastRecvNs{0};

        // touched by the delivering thread only
        bool any = false;
        uint64_t curPub = 0;
        uint64_t last = 0;

        void onMessage(std::string const &topic, std::string const &content) {
            int64_t const now = nowNs();
            if (raw) {
                received.fetch_add(1, std::memory_order_relaxed);
                lastRecvNs.store(now, std::memory_order_relaxed);
                return;
            }
            MsgHeader h;
            if (!checkMessage(content, h)) {
                corrupt.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (!topics.empty() && (h.topicTag >= topics.size() || topics[h.topicTag] != topic)) {
                    wrongTopic.fetch_add(1, std::memory_order_relaxed);
                }
                if (!expectTopics.empty() && expectTopics.find(topic) == expectTopics.end()) {
                    wrongTopic.fetch_add(1, std::memory_order_relaxed);
                }
                if (!any || h.pubId != curPub) {
                    if (any) {
                        pubSwitches.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        firstSeq.store(h.seq, std::memory_order_relaxed);
                        firstAgeUs.store((now - h.sendNs) / 1000, std::memory_order_relaxed);
                    }
                    any = true;
                    curPub = h.pubId;
                    lastPubReceived.store(0, std::memory_order_relaxed);
                } else if (h.seq <= last) {
                    outOfOrder.fetch_add(1, std::memory_order_relaxed);
                } else {
                    gaps.fetch_add(h.seq - last - 1, std::memory_order_relaxed);
                }
                last = h.seq;
                lastSeq.store(h.seq, std::memory_order_relaxed);
                lastPubReceived.fetch_add(1, std::memory_order_relaxed);
            }
            received.fetch_add(1, std::memory_order_relaxed);
            lastRecvNs.store(now, std::memory_order_relaxed);
            if (sleepUs > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
            }
        }
    };

    template <bool Typed>
    void runSubscriber(Args const &a) {
        using D = std::conditional_t<Typed, basic::TypedDataWithTopic<std::string>, basic::ByteDataWithTopic>;

        std::string const channel = a.get("channel");
        auto const locator = locatorOf(channel);
        std::optional<std::string> topic = std::nullopt;
        if (a.has("topic")) {
            topic = a.get("topic");
        }
        std::string const label = a.get("label", "sub");
        uint64_t const count = static_cast<uint64_t>(a.num("count", 0));
        int64_t const idleMs = a.num("idle-ms", 2000);
        int64_t const maxWaitMs = a.num("max-wait-ms", 10000);

        auto st = std::make_shared<SubStats>();
        st->topics = splitComma(a.get("topics", ""));
        for (auto const &t : splitComma(a.get("expect-topics", ""))) {
            st->expectTopics.insert(t);
        }
        st->raw = a.flag("raw");
        st->sleepUs = a.num("sleep-us", 0);

        TheEnvironment env;
        R r(&env);
        infra::SynchronousRunner<M> sr(&env);
        std::thread syncThread;

        if (a.flag("fetch")) {
            // one-shot query path, prints the first message and exits
            FetchEnvironment fenv;
            auto f = transport::MultiTransportBroadcastFirstUpdateQueryManagingUtils<FetchEnvironment>::fetchFirstUpdateAndDisconnect(&fenv, channel, topic);
            if (f.wait_for(std::chrono::milliseconds(maxWaitMs)) != std::future_status::ready) {
                finish("role=fetch label=" + label + " ok=0");
            }
            auto d = f.get();
            MsgHeader h;
            bool ok = checkMessage(d.content, h);
            auto ft = transport::MultiTransportBroadcastFirstUpdateQueryManagingUtils<FetchEnvironment>::fetchTypedFirstUpdateAndDisconnect<std::string>(&fenv, channel, topic);
            bool typedOk = false;
            if (ft.wait_for(std::chrono::milliseconds(maxWaitMs)) == std::future_status::ready) {
                MsgHeader h2;
                typedOk = checkMessage(ft.get().content, h2);
            }
            std::ostringstream oss;
            oss << "role=fetch label=" << label << " ok=" << (ok ? 1 : 0) << " typedOk=" << (typedOk ? 1 : 0)
                << " topic=" << d.topic << " seq=" << (ok ? h.seq : 0);
            finish(oss.str());
        } else if (a.flag("sync")) {
            auto imp = transport::MultiTransportBroadcastListenerManagingUtils<infra::SynchronousRunner<M>>::oneByteDataBroadcastListener(sr, channel, topic);
            syncThread = std::thread([&sr, imp, st]() {
                auto res = sr.importItem<basic::ByteDataWithTopic>(imp);
                for (auto iter = res->begin(); iter != res->end(); ++iter) {
                    auto d = *iter;
                    st->onMessage(d.timedData.value.topic, d.timedData.value.content);
                }
            });
            syncThread.detach();
        } else {
            auto exporter = M::simpleExporter<D>([st](M::InnerData<D> &&d) {
                st->onMessage(d.timedData.value.topic, d.timedData.value.content);
            });
            if constexpr (Typed) {
                auto src = transport::MultiTransportBroadcastListenerManagingUtils<R>::oneBroadcastListenerWithTopic<std::string>(r, "sub", channel, topic);
                r.exportItem("sink", exporter, std::move(src));
            } else {
                auto src = transport::MultiTransportBroadcastListenerManagingUtils<R>::oneByteDataBroadcastListener(r, "sub", channel, topic);
                r.exportItem("sink", exporter, std::move(src));
            }
            r.preservePointer(st);
            r.finalize();
        }

        auto const t0 = std::chrono::steady_clock::now();
        std::cerr << "[shm_cursor_test] " << label << " ready" << std::endl;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            uint64_t const n = st->received.load();
            if (count > 0 && n >= count) {
                break;
            }
            int64_t const last = st->lastRecvNs.load();
            if (n > 0 && (nowNs() - last) / 1000000 > idleMs) {
                break;
            }
            if (n == 0 && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(maxWaitMs)) {
                break;
            }
        }
        std::ostringstream oss;
        oss << "role=sub label=" << label
            << " received=" << st->received.load()
            << " corrupt=" << st->corrupt.load()
            << " wrongTopic=" << st->wrongTopic.load()
            << " outOfOrder=" << st->outOfOrder.load()
            << " gaps=" << st->gaps.load()
            << " pubSwitches=" << st->pubSwitches.load()
            << " lastPubReceived=" << st->lastPubReceived.load()
            << " firstSeq=" << st->firstSeq.load()
            << " lastSeq=" << st->lastSeq.load()
            << " firstAgeUs=" << st->firstAgeUs.load()
            << " transportLost=" << env.shm_cursor_subscriptionLossCount(locator)
            << " overwrites=" << env.shm_cursor_subscriptionOverwriteCount(locator);
        finish(oss.str());
    }

    // ---------------------------------------------------------------- stress

    std::string stressTopic(uint64_t seq) {
        // variable topic length so that lengths and offsets keep changing
        return "s" + std::string(seq % 11, 'x') + std::to_string(seq % 3);
    }

    struct StressReader {
        std::unique_ptr<transport::shm_cursor::ShmCursorComponent> comp;
        std::atomic<uint64_t> delivered{0};
        std::atomic<uint64_t> torn{0};
        std::atomic<uint64_t> outOfOrder{0};
        uint64_t last = 0;
    };

    // Deliberately broken reader used as a negative control: it copies the latest
    // slot without the claim re-check, so the validator must see torn messages.
    // Layout constants mirror src/shm_cursor/ShmCursorComponent.cpp.
    void naiveReader(std::string const &id, std::atomic<bool> &stop, std::atomic<uint64_t> &checked, std::atomic<uint64_t> &torn) {
        int fd = -1;
        while (fd < 0 && !stop) {
            fd = ::open(("/dev/shm/tm_shm_cursor." + id).c_str(), O_RDONLY);
        }
        if (fd < 0) {
            return;
        }
        struct stat st;
        ::fstat(fd, &st);
        char const *m = static_cast<char const *>(::mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0));
        uint32_t slotSize;
        uint64_t slotCount;
        std::memcpy(&slotSize, m + 12, 4);
        std::memcpy(&slotCount, m + 16, 8);
        auto const *commit = reinterpret_cast<uint64_t const *>(m + 136);
        char const *slots = m + 640;
        std::vector<char> buf(slotSize);
        uint64_t lastC = 0;
        while (!stop) {
            uint64_t c = __atomic_load_n(commit, __ATOMIC_ACQUIRE);
            if (c == lastC || c == 0) {
                continue;
            }
            lastC = c;
            std::memcpy(buf.data(), slots + (c & (slotCount - 1)) * slotSize, slotSize);
            uint32_t cl, tl;
            std::memcpy(&cl, buf.data(), 4);
            std::memcpy(&tl, buf.data() + 4, 4);
            checked.fetch_add(1, std::memory_order_relaxed);
            MsgHeader h;
            if (tl + cl > slotSize - 16 || !checkMessage(std::string_view(buf.data() + 16 + tl, cl), h) || std::string_view(buf.data() + 16, tl) != stressTopic(h.seq)) {
                torn.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void runStress(Args const &a) {
        std::string const id = a.get("segment", "shm_cursor_stress_" + std::to_string(::getpid()));
        int64_t const slotCount = a.num("slot-count", 4);
        int64_t const slotSize = a.num("slot-size", 512);
        int const readers = static_cast<int>(a.num("readers", 3));
        int64_t const seconds = a.num("seconds", 5);
        bool const naive = !a.flag("no-naive");
        std::string const spec = "::::" + id + "[slotSize=" + std::to_string(slotSize) + ",slotCount=" + std::to_string(slotCount) + ",join=next]";
        auto const locator = transport::ConnectionLocator::parse(spec);
        std::size_t const capacity = static_cast<std::size_t>(slotSize) - 16;

        transport::shm_cursor::ShmCursorComponent pubComp;
        auto publisher = pubComp.shm_cursor_getPublisher(locator);

        std::vector<std::unique_ptr<StressReader>> rs;
        for (int ii = 0; ii < readers; ++ii) {
            auto rd = std::make_unique<StressReader>();
            rd->comp = std::make_unique<transport::shm_cursor::ShmCursorComponent>();
            auto *p = rd.get();
            rd->comp->shm_cursor_addSubscriptionClient(
                locator, transport::shm_cursor::ShmCursorComponent::NoTopicSelection{}, [p](basic::ByteDataWithTopic &&d) {
                    MsgHeader h;
                    if (!checkMessage(d.content, h) || d.topic != stressTopic(h.seq)) {
                        p->torn.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        if (h.seq <= p->last) {
                            p->outOfOrder.fetch_add(1, std::memory_order_relaxed);
                        }
                        p->last = h.seq;
                    }
                    p->delivered.fetch_add(1, std::memory_order_relaxed);
                });
            rs.push_back(std::move(rd));
        }
        // give the reader threads time to attach before the flood starts
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::atomic<bool> stop{false};
        std::atomic<uint64_t> naiveChecked{0}, naiveTorn{0};
        std::thread naiveTh;
        if (naive) {
            naiveTh = std::thread([&]() {
                naiveReader(id, stop, naiveChecked, naiveTorn);
            });
        }
        uint64_t sent = 0;
        std::thread pubTh([&]() {
            uint64_t const pubId = randomId();
            std::mt19937_64 rng(pubId);
            basic::ByteDataWithTopic d;
            for (uint64_t i = 1; !stop.load(std::memory_order_relaxed); ++i) {
                d.topic = stressTopic(i);
                std::size_t const maxLen = capacity - d.topic.size();
                std::size_t const len = sizeof(MsgHeader) + rng() % (maxLen - sizeof(MsgHeader) + 1);
                fillMessage(d.content, i, pubId, len, 0);
                publisher(std::move(d));
                sent = i;
            }
        });
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        stop = true;
        pubTh.join();
        if (naiveTh.joinable()) {
            naiveTh.join();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::ostringstream oss;
        oss << "role=stress slotCount=" << slotCount << " slotSize=" << slotSize << " sent=" << sent;
        uint64_t totalTorn = 0, totalOoo = 0;
        for (int ii = 0; ii < readers; ++ii) {
            auto &rd = *rs[ii];
            totalTorn += rd.torn.load();
            totalOoo += rd.outOfOrder.load();
            oss << " r" << ii << "_delivered=" << rd.delivered.load()
                << " r" << ii << "_lost=" << rd.comp->shm_cursor_subscriptionLossCount(locator)
                << " r" << ii << "_overwrites=" << rd.comp->shm_cursor_subscriptionOverwriteCount(locator)
                << " r" << ii << "_torn=" << rd.torn.load();
        }
        oss << " torn=" << totalTorn << " outOfOrder=" << totalOoo
            << " naiveChecked=" << naiveChecked.load() << " naiveTorn=" << naiveTorn.load();
        ::shm_unlink(("/tm_shm_cursor." + id).c_str());
        ::unlink(("/dev/shm/tm_shm_cursor." + id + ".lock").c_str());
        finish(oss.str());
    }
}

int main(int argc, char **argv) {
    try {
        Args a(argc, argv);
        std::string const mode = a.get("mode");
        bool const typed = a.flag("typed");
        if (mode == "pub") {
            if (typed) {
                runPublisher<true>(a);
            } else {
                runPublisher<false>(a);
            }
        } else if (mode == "sub") {
            if (typed) {
                runSubscriber<true>(a);
            } else {
                runSubscriber<false>(a);
            }
        } else if (mode == "stress") {
            runStress(a);
        } else {
            std::cerr << "usage: shm_cursor_test --mode=pub|sub|stress [--key=value ...]" << std::endl;
            return 2;
        }
    } catch (std::exception const &ex) {
        std::cerr << "[shm_cursor_test] error: " << ex.what() << std::endl;
        std::cout << "RESULT role=error what=\"" << ex.what() << "\"" << std::endl;
        return 1;
    }
    return 0;
}
