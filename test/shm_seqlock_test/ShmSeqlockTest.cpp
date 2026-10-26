//Functional test driver for the shm_seqlock transport.
//
//  shm_seqlock_test stress --segment=<name> [--readers=2] [--read=classic|spec] [--wait=spin|futex]
//                          [--slotCount=64] [--slotSize=512] [--count=3000000] [--readerSlowEvery=0]
//      single process, one publisher thread and N reader threads on the component
//      directly; checks integrity and accounting under lapping. Not a latency tool.
//
//  shm_seqlock_test pub --channel=<spec> [--count=0(forever)] [--intervalNs=0] [--payload=256]
//                       [--topics=a.x,a.y,b.x] [--run=1] [--startDelayMs=0] [--logEvery=0]
//                       [--oversizeEvery=0] [--oversizeLen=0] [--holdSec=0]
//      publishes through MultiTransportBroadcastPublisherManagingUtils::oneByteDataBroadcastPublisher
//
//  shm_seqlock_test sub --channel=<spec> [--topic=<desc>] [--count=0] [--timeoutSec=30]
//                       [--idleExitSec=0] [--sleepUsPerMsg=0] [--topics=a.x,a.y,b.x]
//                       [--expectTopicRe=<regex>] [--payload=0]
//      subscribes through MultiTransportBroadcastListenerManagingUtils::oneByteDataBroadcastListener
//
//  shm_seqlock_test fetch --channel=<spec> [--topic=<desc>] [--timeoutSec=10]
//      MultiTransportBroadcastFirstUpdateQueryManagingUtils::fetchFirstUpdateAndDisconnect
//
//Every payload carries {run, seq, len, topicIdx} followed by bytes derived from
//seq, so any torn or mixed read is detected.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <boost/algorithm/string.hpp>

#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>

#include <tm_kit/basic/ByteData.hpp>
#include <tm_kit/basic/TrivialBoostLoggingComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>

#include <tm_kit/transport/MultiTransportBroadcastPublisherManagingUtils.hpp>
#include <tm_kit/transport/MultiTransportBroadcastListenerManagingUtils.hpp>
#include <tm_kit/transport/shm_seqlock/ShmSeqlockComponent.hpp>

using namespace dev::cd606::tm;

namespace {
    using Options = std::map<std::string, std::string>;

    std::string opt(Options const &o, std::string const &k, std::string const &d) {
        auto iter = o.find(k);
        return (iter == o.end() ? d : iter->second);
    }
    uint64_t optNum(Options const &o, std::string const &k, uint64_t d) {
        auto iter = o.find(k);
        return (iter == o.end() ? d : std::stoull(iter->second));
    }
    std::vector<std::string> optList(Options const &o, std::string const &k, std::string const &d) {
        std::vector<std::string> v;
        auto s = opt(o, k, d);
        boost::split(v, s, boost::is_any_of(","));
        return v;
    }
    int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    struct PayloadHeader {
        uint64_t run;
        uint64_t seq;
        uint32_t len;
        uint32_t topicIdx;
    };
    inline char fillByte(uint64_t seq, std::size_t k) {
        return static_cast<char>((seq*131u+k*7u+13u) & 0xffu);
    }
    void makePayload(std::string &s, uint64_t run, uint64_t seq, std::size_t len, uint32_t topicIdx) {
        len = std::max(len, sizeof(PayloadHeader));
        s.resize(len);
        PayloadHeader h {run, seq, static_cast<uint32_t>(len), topicIdx};
        std::memcpy(s.data(), &h, sizeof(h));
        for (std::size_t k=sizeof(h); k<len; ++k) {
            s[k] = fillByte(seq, k);
        }
    }
    bool checkPayload(std::string const &content, PayloadHeader &h) {
        if (content.size() < sizeof(PayloadHeader)) {
            return false;
        }
        std::memcpy(&h, content.data(), sizeof(h));
        if (h.len != content.size()) {
            return false;
        }
        for (std::size_t k=sizeof(h); k<content.size(); ++k) {
            if (content[k] != fillByte(h.seq, k)) {
                return false;
            }
        }
        return true;
    }

    //Shared by stress readers and the sub mode; only called from one reader thread
    struct Checker {
        std::vector<std::string> topics;
        std::optional<std::regex> expectRe;
        std::size_t payload = 0;
        uint64_t sleepUs = 0;
        uint64_t slowEvery = 0;

        std::mutex mutex;
        std::atomic<uint64_t> received {0};
        std::atomic<int64_t> lastRecvNs {0};
        uint64_t bad = 0, ooo = 0, gaps = 0, topicBad = 0, sizeMismatch = 0;
        bool haveFirst = false;
        uint64_t firstRun = 0, firstSeq = 0;
        bool printFirst = false;
        struct RunInfo {
            uint64_t count;
            uint64_t lastSeq;
        };
        std::map<uint64_t, RunInfo> runs;

        void on(basic::ByteDataWithTopic const &d) {
            {
                std::lock_guard<std::mutex> _(mutex);
                PayloadHeader h;
                if (!checkPayload(d.content, h)) {
                    ++bad;
                } else {
                    if (payload != 0 && d.content.size() != payload) {
                        ++sizeMismatch;
                    }
                    if (h.topicIdx >= topics.size() || d.topic != topics[h.topicIdx]) {
                        ++topicBad;
                    }
                    if (expectRe && !std::regex_match(d.topic, *expectRe)) {
                        ++topicBad;
                    }
                    if (!haveFirst) {
                        haveFirst = true;
                        firstRun = h.run;
                        firstSeq = h.seq;
                        if (printFirst) {
                            std::printf("FIRST run=%lu seq=%lu\n", static_cast<unsigned long>(h.run), static_cast<unsigned long>(h.seq));
                            std::fflush(stdout);
                        }
                    }
                    auto iter = runs.find(h.run);
                    if (iter == runs.end()) {
                        runs.insert({h.run, {1, h.seq}});
                    } else {
                        if (h.seq <= iter->second.lastSeq) {
                            ++ooo;
                        } else {
                            gaps += h.seq-iter->second.lastSeq-1;
                        }
                        ++(iter->second.count);
                        iter->second.lastSeq = h.seq;
                    }
                }
            }
            lastRecvNs.store(nowNs(), std::memory_order_relaxed);
            auto r = received.fetch_add(1, std::memory_order_relaxed)+1;
            if (sleepUs != 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
            }
            if (slowEvery != 0 && r % slowEvery == 0) {
                auto until = nowNs()+20000;
                while (nowNs() < until) {
                }
            }
        }
    };

    int runStress(Options const &o) {
        auto segment = opt(o, "segment", "shm_seqlock_stress");
        auto readers = optNum(o, "readers", 2);
        auto count = optNum(o, "count", 3000000);
        auto slotSize = optNum(o, "slotSize", 512);
        auto locator = transport::ConnectionLocator::parse(
            "::::"+segment
            +"[slotSize="+std::to_string(slotSize)
            +",slotCount="+opt(o, "slotCount", "64")
            +",read="+opt(o, "read", "classic")
            +",wait="+opt(o, "wait", "spin")
            +"]"
        );
        std::vector<std::string> topics {"a.x", "a.yy", "b.zzzzzz"};
        std::size_t maxPayload = slotSize-topics[2].size();

        transport::shm_seqlock::ShmSeqlockComponent pubComp;
        auto pub = pubComp.shm_seqlock_getPublisher(locator);
        std::vector<std::unique_ptr<transport::shm_seqlock::ShmSeqlockComponent>> comps;
        std::vector<std::unique_ptr<Checker>> checkers;
        for (uint64_t ii=0; ii<readers; ++ii) {
            auto c = std::make_unique<Checker>();
            c->topics = topics;
            c->slowEvery = optNum(o, "readerSlowEvery", 0)*(ii+1);
            auto comp = std::make_unique<transport::shm_seqlock::ShmSeqlockComponent>();
            auto *cp = c.get();
            comp->shm_seqlock_addSubscriptionClient(
                locator
                , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection {}
                , [cp](basic::ByteDataWithTopic &&d) {
                    cp->on(d);
                }
            );
            comps.push_back(std::move(comp));
            checkers.push_back(std::move(c));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto t0 = nowNs();
        std::thread th([&]() {
            std::string payload;
            for (uint64_t seq=0; seq<count; ++seq) {
                std::size_t len = sizeof(PayloadHeader)+(seq*2654435761ULL) % (maxPayload-sizeof(PayloadHeader)+1);
                uint32_t ti = static_cast<uint32_t>(seq % topics.size());
                makePayload(payload, 1, seq, len, ti);
                pub({topics[ti], payload});
            }
        });
        th.join();
        auto t1 = nowNs();

        //wait for readers to account for every message
        auto deadline = nowNs()+5000000000LL;
        auto statsOf = [&](std::size_t ii) {
            auto m = comps[ii]->shm_seqlock_readerStats();
            return (m.empty() ? transport::shm_seqlock::ShmSeqlockReaderStats {} : m.begin()->second);
        };
        while (nowNs() < deadline) {
            bool all = true;
            for (std::size_t ii=0; ii<readers; ++ii) {
                auto s = statsOf(ii);
                if (checkers[ii]->received.load()+s.lost < count) {
                    all = false;
                }
            }
            if (all) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        bool pass = true;
        auto ps = pubComp.shm_seqlock_publisherStats().begin()->second;
        std::printf("STRESS read=%s wait=%s slots=%s published=%lu rate=%.2fM msg/s\n"
            , opt(o, "read", "classic").c_str(), opt(o, "wait", "spin").c_str(), opt(o, "slotCount", "64").c_str()
            , static_cast<unsigned long>(ps.published), count*1000.0/static_cast<double>(t1-t0));
        for (std::size_t ii=0; ii<readers; ++ii) {
            auto s = statsOf(ii);
            auto &c = *checkers[ii];
            std::lock_guard<std::mutex> _(c.mutex);
            uint64_t rec = c.received.load();
            //a futex-waiting reader may be lapped before its first delivery, so the
            //first seq is not required to be 0; every index must be delivered or lost
            bool ok = (c.bad == 0 && c.ooo == 0 && c.topicBad == 0 && rec+s.lost == count && s.corrupt == 0
                && c.haveFirst && c.firstSeq == s.lost-c.gaps);
            pass = pass && ok;
            std::printf("  reader %zu: received=%lu lost=%lu lapResyncs=%lu bad=%lu ooo=%lu topicBad=%lu gaps=%lu corrupt=%lu first=%lu %s\n"
                , ii, static_cast<unsigned long>(rec), static_cast<unsigned long>(s.lost), static_cast<unsigned long>(s.lapResyncs)
                , static_cast<unsigned long>(c.bad), static_cast<unsigned long>(c.ooo), static_cast<unsigned long>(c.topicBad)
                , static_cast<unsigned long>(c.gaps), static_cast<unsigned long>(s.corrupt), static_cast<unsigned long>(c.firstSeq)
                , (ok ? "OK" : "FAIL"));
        }
        std::printf("STRESS %s\n", (pass ? "PASS" : "FAIL"));
        std::fflush(stdout);
        ::unlink(("/dev/shm/"+segment).c_str());
        ::unlink(("/dev/shm/"+segment+".lock").c_str());
        _exit(pass ? 0 : 1);
    }

    using TheEnvironment = infra::Environment<
        infra::CheckTimeComponent<false>,
        infra::TrivialExitControlComponent,
        basic::TrivialBoostLoggingComponent,
        basic::real_time_clock::ClockComponent,
        transport::shm_seqlock::ShmSeqlockComponent
    >;
    using M = infra::RealTimeApp<TheEnvironment>;
    using R = infra::AppRunner<M>;

    int runPub(Options const &o) {
        auto channel = opt(o, "channel", "");
        auto count = optNum(o, "count", 0);
        auto intervalNs = static_cast<int64_t>(optNum(o, "intervalNs", 0));
        auto payloadLen = optNum(o, "payload", 256);
        auto topics = optList(o, "topics", "a.x,a.y,b.x");
        auto run = optNum(o, "run", 1);
        auto startDelayMs = optNum(o, "startDelayMs", 0);
        auto logEvery = optNum(o, "logEvery", 0);
        auto oversizeEvery = optNum(o, "oversizeEvery", 0);
        auto oversizeLen = optNum(o, "oversizeLen", 0);
        auto holdSec = optNum(o, "holdSec", 0);

        struct PubState {
            std::atomic<bool> done {false};
            uint64_t sent = 0;
            uint64_t oversizeSent = 0;
            int64_t publishNs = 0;
            int64_t elapsedNs = 0;
        };
        auto state = std::make_shared<PubState>();

        TheEnvironment env;
        R r(&env);
        auto importer = M::simpleImporter<basic::ByteDataWithTopic>(
            [=](M::PublisherCall<basic::ByteDataWithTopic> &pub) {
                std::this_thread::sleep_for(std::chrono::milliseconds(startDelayMs));
                std::printf("PUBSTART pid=%d\n", static_cast<int>(::getpid()));
                std::fflush(stdout);
                std::string payload;
                std::string big;
                int64_t publishNs = 0;
                auto start = nowNs();
                auto next = start;
                uint64_t seq = 0;
                uint64_t oversizeSent = 0;
                for (; count == 0 || seq < count; ++seq) {
                    uint32_t ti = static_cast<uint32_t>(seq % topics.size());
                    makePayload(payload, run, seq, payloadLen, ti);
                    auto t0 = nowNs();
                    pub(basic::ByteDataWithTopic {topics[ti], std::move(payload)});
                    publishNs += nowNs()-t0;
                    if (logEvery != 0 && seq % logEvery == 0) {
                        std::printf("SENT seq=%lu\n", static_cast<unsigned long>(seq));
                        std::fflush(stdout);
                    }
                    if (oversizeEvery != 0 && (seq+1) % oversizeEvery == 0) {
                        makePayload(big, run, ~0ULL, oversizeLen, 0);
                        pub(basic::ByteDataWithTopic {topics[0], big});
                        ++oversizeSent;
                    }
                    if (intervalNs > 0) {
                        next += intervalNs;
                        if (intervalNs >= 1000000) {
                            std::this_thread::sleep_for(std::chrono::nanoseconds(next-nowNs()));
                        } else {
                            while (nowNs() < next) {
                            }
                        }
                    }
                }
                state->sent = seq;
                state->oversizeSent = oversizeSent;
                state->publishNs = publishNs;
                state->elapsedNs = nowNs()-start;
                state->done = true;
            }
            , infra::LiftParameters<M::TimePoint>().SuggestThreaded(true)
        );
        auto sink = transport::MultiTransportBroadcastPublisherManagingUtils<R>::oneByteDataBroadcastPublisher(
            r, "pub", channel
        );
        r.connect(r.importItem("source", importer), sink);
        r.finalize();

        while (!state->done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        uint64_t rejected = 0;
        uint64_t waiterResets = 0;
        for (auto const &item : env.shm_seqlock_publisherStats()) {
            rejected += item.second.rejected;
            waiterResets += item.second.waiterResets;
        }
        std::printf("PUBRESULT sent=%lu oversizeSent=%lu rejected=%lu waiterResets=%lu publishNsAvg=%.1f elapsedSec=%.3f\n"
            , static_cast<unsigned long>(state->sent), static_cast<unsigned long>(state->oversizeSent)
            , static_cast<unsigned long>(rejected), static_cast<unsigned long>(waiterResets)
            , (state->sent ? static_cast<double>(state->publishNs)/static_cast<double>(state->sent) : 0.0)
            , static_cast<double>(state->elapsedNs)/1e9);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(holdSec));
        _exit(0);
    }

    int runSub(Options const &o) {
        auto channel = opt(o, "channel", "");
        auto count = optNum(o, "count", 0);
        auto timeoutSec = optNum(o, "timeoutSec", 30);
        auto idleExitSec = optNum(o, "idleExitSec", 0);
        std::optional<std::string> topic;
        if (o.find("topic") != o.end()) {
            topic = opt(o, "topic", "");
        }

        auto checker = std::make_shared<Checker>();
        checker->topics = optList(o, "topics", "a.x,a.y,b.x");
        if (o.find("expectTopicRe") != o.end()) {
            checker->expectRe = std::regex(opt(o, "expectTopicRe", ""));
        }
        checker->payload = optNum(o, "payload", 0);
        checker->sleepUs = optNum(o, "sleepUsPerMsg", 0);
        checker->printFirst = true;

        TheEnvironment env;
        R r(&env);
        auto source = transport::MultiTransportBroadcastListenerManagingUtils<R>::oneByteDataBroadcastListener(
            r, "sub", channel, topic
        );
        auto sink = M::simpleExporter<basic::ByteDataWithTopic>([checker](M::InnerData<basic::ByteDataWithTopic> &&d) {
            checker->on(d.timedData.value);
        });
        r.exportItem("check", sink, std::move(source));
        r.preservePointer(checker);
        r.finalize();
        std::printf("SUBSTART pid=%d\n", static_cast<int>(::getpid()));
        std::fflush(stdout);

        auto start = nowNs();
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            auto rec = checker->received.load();
            auto now = nowNs();
            if (count != 0 && rec >= count) {
                break;
            }
            if (timeoutSec != 0 && now-start > static_cast<int64_t>(timeoutSec)*1000000000LL) {
                break;
            }
            if (idleExitSec != 0 && rec > 0 && now-checker->lastRecvNs.load() > static_cast<int64_t>(idleExitSec)*1000000000LL) {
                break;
            }
        }
        transport::shm_seqlock::ShmSeqlockReaderStats s {};
        for (auto const &item : env.shm_seqlock_readerStats()) {
            s = item.second;
        }
        std::lock_guard<std::mutex> _(checker->mutex);
        std::printf("SUBRESULT received=%lu bad=%lu ooo=%lu gaps=%lu topicBad=%lu sizeMismatch=%lu first=%lu runs=%zu lost=%lu lapResyncs=%lu restarts=%lu reopens=%lu corrupt=%lu"
            , static_cast<unsigned long>(checker->received.load()), static_cast<unsigned long>(checker->bad)
            , static_cast<unsigned long>(checker->ooo), static_cast<unsigned long>(checker->gaps)
            , static_cast<unsigned long>(checker->topicBad), static_cast<unsigned long>(checker->sizeMismatch)
            , static_cast<unsigned long>(checker->firstSeq), checker->runs.size()
            , static_cast<unsigned long>(s.lost), static_cast<unsigned long>(s.lapResyncs)
            , static_cast<unsigned long>(s.publisherRestarts), static_cast<unsigned long>(s.reopens)
            , static_cast<unsigned long>(s.corrupt));
        for (auto const &item : checker->runs) {
            std::printf(" run%lu=%lu", static_cast<unsigned long>(item.first), static_cast<unsigned long>(item.second.count));
        }
        std::printf("\n");
        std::fflush(stdout);
        _exit(0);
    }
}

namespace {
    int runFetch(Options const &o) {
        auto channel = opt(o, "channel", "");
        auto timeoutSec = optNum(o, "timeoutSec", 10);
        std::optional<std::string> topic;
        if (o.find("topic") != o.end()) {
            topic = opt(o, "topic", "");
        }
        TheEnvironment env;
        //component-only Env: this utility does not compile with AllNetworkTransportComponents
        //(pre-existing ambiguous websocket overload)
        auto f = transport::MultiTransportBroadcastFirstUpdateQueryManagingUtils<TheEnvironment>::fetchFirstUpdateAndDisconnect(
            &env, channel, topic
        );
        if (f.wait_for(std::chrono::seconds(timeoutSec)) != std::future_status::ready) {
            std::printf("FETCHRESULT ok=0 reason=timeout\n");
            std::fflush(stdout);
            _exit(1);
        }
        auto d = f.get();
        PayloadHeader h {};
        bool ok = checkPayload(d.content, h);
        //the subscription (and its reader thread) must be gone after the fetch
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto left = env.shm_seqlock_threadHandles().size();
        std::printf("FETCHRESULT ok=%d topic=%s seq=%lu readersLeft=%zu\n"
            , (ok ? 1 : 0), d.topic.c_str(), static_cast<unsigned long>(h.seq), left);
        std::fflush(stdout);
        _exit((ok && left == 0) ? 0 : 1);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s stress|pub|sub --key=value ...\n", argv[0]);
        return 2;
    }
    Options o;
    for (int ii=2; ii<argc; ++ii) {
        std::string a = argv[ii];
        if (!boost::starts_with(a, "--")) {
            std::fprintf(stderr, "bad argument '%s'\n", argv[ii]);
            return 2;
        }
        auto eq = a.find('=');
        if (eq == std::string::npos) {
            o[a.substr(2)] = "true";
        } else {
            o[a.substr(2, eq-2)] = a.substr(eq+1);
        }
    }
    std::string mode = argv[1];
    if (mode == "stress") {
        return runStress(o);
    } else if (mode == "pub") {
        return runPub(o);
    } else if (mode == "sub") {
        return runSub(o);
    } else if (mode == "fetch") {
        return runFetch(o);
    }
    std::fprintf(stderr, "unknown mode '%s'\n", mode.c_str());
    return 2;
}
