//Concurrency test for the IPC transports (shm_seqlock, shm_cursor,
//shared_memory_broadcast, aeron_ipc). Correctness only, not a latency test.
//
//Modes (options are --key=value):
//  inproc  writer threads and reader threads in one process
//  pub     writer threads only (use several pub processes for multi-process write)
//  sub     reader threads only (use several sub processes for multi-process read)
//  graph   writers are threaded importers of a TM Kit graph, all connected to ONE
//          sink built by MultiTransportBroadcastPublisherManagingUtils
//          (--threadedSink=true puts the threaded wrapper in front of the exporter)
//
//Writer options:
//  --channel=<channel>        e.g. shm_seqlock://::::seg[writerLock=true]
//  --altChannel=<channel>     odd writer threads call getPublisher with this channel
//                             (same segment, different properties)
//  --writers=N                writer threads (default 4)
//  --writerBase=K             id of the first writer thread (multi-process write)
//  --totalWriters=T           writer ids known to readers (default writerBase+writers)
//  --count=n                  messages per writer (default 200000)
//  --maxPayload=bytes         largest payload incl. 24 byte header (default 200)
//  --intervalNs=ns            pacing per writer, 0 = as fast as possible
//  --sharedPublisher=true     all writers call ONE publisher function object
//                             (default: each writer calls getPublisher itself; both
//                             end up on the same underlying sender)
//  --warmupMs=ms              wait before writing so readers can attach (default 500)
//  --holdMs=ms                pub mode: keep the publisher alive after writing (default 1000)
//Reader options:
//  --readerComps=R            component instances = reader threads (default 2)
//  --clientsPerComp=C         subscription clients per component (default 2)
//  --churn=true               extra thread adding/removing clients during the run
//  --drainMs=ms               stop when nothing arrived for this long (default 1000)
//  --timeoutSec=s             overall reader deadline (default 60)
//  --expect=n                 sub mode: stop early once every client received n
//
//Each mode prints READER lines and one RESULT line:
//  RESULT mode=.. transport=.. status=ok|error clean=1|0 written=.. received=..
//         corrupt=.. topicBad=.. ooo=.. dup=.. unknownWriter=.. gaps=.. [transport stats]
//clean=1 means no reader saw a torn, mixed, duplicated or reordered message;
//gaps (lost messages) are allowed because the shm transports are lossy.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/transport/MultiTransportBroadcastPublisherManagingUtils.hpp>

#include "ConcurrencyCommon.hpp"
#include "TransportAdapter.hpp"

using namespace ipc_concurrency;

namespace {
    using M = infra::RealTimeApp<Env>;
    using R = infra::AppRunner<M>;

    void emit(std::string const &line) {
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    //Readers: each component instance (own Env) has its own transport reader
    //thread for the segment, every client on it gets its own Checker.
    class ReaderSet {
    private:
        Channel channel_;
        std::size_t totalWriters_;
        std::vector<std::unique_ptr<Env>> envs_;
        std::vector<std::unique_ptr<Checker>> checkers_;
        std::vector<std::size_t> checkerEnv_;
        std::atomic<uint64_t> lastActivityNs_ {0};
        std::atomic<uint64_t> anyReceived_ {0};
        //churn clients only check message validity, their ordering is not tracked
        std::atomic<uint64_t> churnReceived_ {0};
        std::atomic<uint64_t> churnCorrupt_ {0};
        std::atomic<uint64_t> churnCycles_ {0};
        std::atomic<bool> stopChurn_ {false};
        std::thread churnThread_;
    public:
        ReaderSet(Channel const &c, std::size_t totalWriters, std::size_t comps, std::size_t clientsPerComp)
            : channel_(c), totalWriters_(totalWriters) {
            for (std::size_t ii=0; ii<comps; ++ii) {
                envs_.push_back(std::make_unique<Env>());
                for (std::size_t jj=0; jj<clientsPerComp; ++jj) {
                    auto ck = std::make_unique<Checker>(totalWriters_);
                    auto *cp = ck.get();
                    subscribe(envs_.back().get(), channel_, [this,cp](basic::ByteDataWithTopic &&d) {
                        cp->on(d.topic, d.content);
                        lastActivityNs_.store(static_cast<uint64_t>(nowNs()), std::memory_order_relaxed);
                        anyReceived_.fetch_add(1, std::memory_order_relaxed);
                    });
                    checkers_.push_back(std::move(ck));
                    checkerEnv_.push_back(ii);
                }
            }
        }
        void startChurn() {
            churnThread_ = std::thread([this]() {
                auto *env = envs_.front().get();
                while (!stopChurn_.load()) {
                    auto id = subscribe(env, channel_, [this](basic::ByteDataWithTopic &&d) {
                        churnReceived_.fetch_add(1, std::memory_order_relaxed);
                        if (!validPayload(d.content)) {
                            churnCorrupt_.fetch_add(1, std::memory_order_relaxed);
                        }
                    });
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    unsubscribe(env, channel_, id);
                    churnCycles_.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        void stopChurn() {
            stopChurn_ = true;
            if (churnThread_.joinable()) {
                churnThread_.join();
            }
        }
        uint64_t received() const {
            return anyReceived_.load();
        }
        int64_t idleNs() const {
            return nowNs()-static_cast<int64_t>(lastActivityNs_.load());
        }
        bool everyClientHas(uint64_t n) {
            for (auto &c : checkers_) {
                if (c->receivedCount() < n) {
                    return false;
                }
            }
            return true;
        }
        //waits until no message arrived for drainMs (after at least one arrived,
        //or until the deadline), optionally stopping early at expect per client
        void drain(uint64_t drainMs, uint64_t timeoutSec, uint64_t expect) {
            auto deadline = nowNs()+static_cast<int64_t>(timeoutSec)*1000000000LL;
            while (nowNs() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (expect > 0 && everyClientHas(expect)) {
                    break;
                }
                if (received() > 0 && idleNs() > static_cast<int64_t>(drainMs)*1000000LL) {
                    break;
                }
            }
        }
        Totals report() {
            Totals t;
            for (std::size_t ii=0; ii<checkers_.size(); ++ii) {
                emit("READER comp="+std::to_string(checkerEnv_[ii])+" client="+std::to_string(ii)
                    +" "+checkers_[ii]->summary());
                checkers_[ii]->addTo(t);
            }
            for (std::size_t ii=0; ii<envs_.size(); ++ii) {
                auto s = subscriberStats(envs_[ii].get(), channel_);
                if (!s.empty()) {
                    emit("READERSTATS comp="+std::to_string(ii)+" "+s);
                }
            }
            if (churnCycles_.load() > 0) {
                emit("CHURN cycles="+std::to_string(churnCycles_.load())
                    +" received="+std::to_string(churnReceived_.load())
                    +" corrupt="+std::to_string(churnCorrupt_.load()));
                t.corrupt += churnCorrupt_.load();
            }
            return t;
        }
        std::size_t clientCount() const {
            return checkers_.size();
        }
    };

    struct WriterConfig {
        Channel channel;
        std::optional<Channel> altChannel;
        uint32_t writers = 4;
        uint32_t writerBase = 0;
        uint64_t count = 200000;
        std::size_t maxPayload = 200;
        int64_t intervalNs = 0;
        bool sharedPublisher = false;
        uint64_t warmupMs = 500;
    };
    WriterConfig writerConfig(Options const &o) {
        WriterConfig w;
        w.channel = parseChannel(opt(o, "channel", ""));
        if (o.find("altChannel") != o.end()) {
            w.altChannel = parseChannel(opt(o, "altChannel", ""));
        }
        w.writers = static_cast<uint32_t>(optNum(o, "writers", 4));
        w.writerBase = static_cast<uint32_t>(optNum(o, "writerBase", 0));
        w.count = optNum(o, "count", 200000);
        w.maxPayload = optNum(o, "maxPayload", 200);
        w.intervalNs = static_cast<int64_t>(optNum(o, "intervalNs", 0));
        w.sharedPublisher = (opt(o, "sharedPublisher", "false") == "true");
        w.warmupMs = optNum(o, "warmupMs", 500);
        return w;
    }

    //one writer loop; the publisher is whatever the caller hands in
    template <class Pub>
    void writeLoop(Pub &&pub, uint32_t writer, WriterConfig const &w) {
        std::string buf;
        std::string topic = topicFor(writer);
        int64_t next = nowNs();
        for (uint64_t seq=0; seq<w.count; ++seq) {
            makePayload(buf, writer, seq, lengthFor(writer, seq, w.maxPayload));
            pub(basic::ByteDataWithTopic {topic, buf});
            if (w.intervalNs > 0) {
                next += w.intervalNs;
                while (nowNs() < next) {
                }
            }
        }
    }

    //plain component writers: all threads start together after the warmup
    uint64_t runWriters(Env *env, WriterConfig const &w) {
        std::this_thread::sleep_for(std::chrono::milliseconds(w.warmupMs));
        Publisher shared;
        if (w.sharedPublisher) {
            shared = getPublisher(env, w.channel);
        }
        std::atomic<uint32_t> ready {0};
        std::atomic<bool> go {false};
        std::vector<std::thread> ths;
        for (uint32_t ii=0; ii<w.writers; ++ii) {
            ths.emplace_back([&,ii]() {
                Publisher own;
                if (!w.sharedPublisher) {
                    //getPublisher itself is called concurrently here on purpose
                    bool alt = (w.altChannel && (ii % 2 == 1));
                    own = getPublisher(env, alt ? *w.altChannel : w.channel);
                }
                Publisher &pub = (w.sharedPublisher ? shared : own);
                ready.fetch_add(1);
                while (!go.load()) {
                }
                writeLoop(pub, w.writerBase+ii, w);
            });
        }
        while (ready.load() < w.writers) {
            std::this_thread::yield();
        }
        auto t0 = nowNs();
        go = true;
        for (auto &t : ths) {
            t.join();
        }
        emit("WRITERS done writers="+std::to_string(w.writers)+" count="+std::to_string(w.count)
            +" elapsedMs="+std::to_string((nowNs()-t0)/1000000));
        return static_cast<uint64_t>(w.writers)*w.count;
    }

    std::size_t totalWritersOf(Options const &o, WriterConfig const *w) {
        uint64_t def = w ? (w->writerBase+w->writers) : 4;
        return static_cast<std::size_t>(optNum(o, "totalWriters", def));
    }

    int finish(std::string const &mode, Channel const &c, uint64_t written, Totals const &t
        , std::string const &extraStats, bool status) {
        std::string line = "RESULT mode="+mode+" transport="+c.name
            +" status="+(status ? "ok" : "error")
            +" clean="+(t.clean() ? "1" : "0")
            +" written="+std::to_string(written)+" "+t.str();
        if (!extraStats.empty()) {
            line += " "+extraStats;
        }
        emit(line);
        //skip destructors: reader threads of several components are still running
        _exit(t.clean() && status ? 0 : 1);
    }

    int runInproc(Options const &o) {
        auto w = writerConfig(o);
        ReaderSet readers(w.channel, totalWritersOf(o, &w)
            , optNum(o, "readerComps", 2), optNum(o, "clientsPerComp", 2));
        bool churn = (opt(o, "churn", "false") == "true");
        //the publisher must exist before shm readers can open the segment
        Env pubEnv;
        auto first = getPublisher(&pubEnv, w.channel);
        (void) first;
        if (churn) {
            readers.startChurn();
        }
        uint64_t written = runWriters(&pubEnv, w);
        readers.drain(optNum(o, "drainMs", 1000), optNum(o, "timeoutSec", 60), 0);
        readers.stopChurn();
        auto t = readers.report();
        return finish("inproc", w.channel, written, t, publisherStats(&pubEnv, w.channel), true);
    }

    int runPub(Options const &o) {
        auto w = writerConfig(o);
        Env env;
        //opening the publisher fails if another process already publishes to the
        //segment (shm_seqlock / shm_cursor flock); report that as status=error
        getPublisher(&env, w.channel);
        emit("PUBREADY pid="+std::to_string(::getpid()));
        uint64_t written = runWriters(&env, w);
        std::this_thread::sleep_for(std::chrono::milliseconds(optNum(o, "holdMs", 1000)));
        return finish("pub", w.channel, written, Totals {}, publisherStats(&env, w.channel), true);
    }

    int runSub(Options const &o) {
        auto c = parseChannel(opt(o, "channel", ""));
        ReaderSet readers(c, totalWritersOf(o, nullptr)
            , optNum(o, "readerComps", 2), optNum(o, "clientsPerComp", 2));
        emit("SUBREADY pid="+std::to_string(::getpid()));
        if (opt(o, "churn", "false") == "true") {
            readers.startChurn();
        }
        readers.drain(optNum(o, "drainMs", 1000), optNum(o, "timeoutSec", 60), optNum(o, "expect", 0));
        readers.stopChurn();
        auto t = readers.report();
        return finish("sub", c, 0, t, "", t.received > 0);
    }

    //writers are threaded importers in one graph, all connected to the same sink
    int runGraph(Options const &o) {
        auto w = writerConfig(o);
        bool threadedSink = (opt(o, "threadedSink", "false") == "true");
        bool heartbeatLike = (opt(o, "extraDirectPublisher", "false") == "true");
        ReaderSet readers(w.channel, totalWritersOf(o, &w)+(heartbeatLike ? 1 : 0)
            , optNum(o, "readerComps", 2), optNum(o, "clientsPerComp", 2));
        Env env;
        R r(&env);
        auto sink = transport::MultiTransportBroadcastPublisherManagingUtils<R>::oneByteDataBroadcastPublisher(
            r, "pub", w.channel.text, std::nullopt, threadedSink);
        std::atomic<uint32_t> done {0};
        for (uint32_t ii=0; ii<w.writers; ++ii) {
            auto importer = M::simpleImporter<basic::ByteDataWithTopic>(
                [w,ii,&done](M::PublisherCall<basic::ByteDataWithTopic> &pub) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(w.warmupMs));
                    writeLoop([&pub](basic::ByteDataWithTopic &&d) { pub(std::move(d)); }
                        , w.writerBase+ii, w);
                    done.fetch_add(1);
                }
                , infra::LiftParameters<M::TimePoint>().SuggestThreaded(true)
            );
            r.connect(r.importItem("writer"+std::to_string(ii), importer), sink);
        }
        r.finalize();
        //like HeartbeatAndAlertComponent: a second getPublisher on the same
        //channel, called from another thread while the graph is publishing
        std::thread extra;
        if (heartbeatLike) {
            extra = std::thread([&]() {
                auto p = getPublisher(&env, w.channel);
                std::this_thread::sleep_for(std::chrono::milliseconds(w.warmupMs));
                writeLoop(p, w.writerBase+w.writers, w);
                done.fetch_add(1);
            });
        }
        uint32_t expected = w.writers+(heartbeatLike ? 1 : 0);
        while (done.load() < expected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (extra.joinable()) {
            extra.join();
        }
        readers.drain(optNum(o, "drainMs", 1000), optNum(o, "timeoutSec", 60), 0);
        auto t = readers.report();
        return finish("graph", w.channel, static_cast<uint64_t>(expected)*w.count, t
            , publisherStats(&env, w.channel), true);
    }
}

int main(int argc, char **argv) {
    auto o = parseOptions(argc, argv);
    auto mode = opt(o, "mode", "");
    try {
        if (mode == "inproc") {
            return runInproc(o);
        } else if (mode == "pub") {
            return runPub(o);
        } else if (mode == "sub") {
            return runSub(o);
        } else if (mode == "graph") {
            return runGraph(o);
        }
        std::cerr << "usage: ipc_concurrency_test --mode=inproc|pub|sub|graph --channel=<channel> [options]\n";
        return 2;
    } catch (std::exception const &ex) {
        std::string what = ex.what();
        for (auto &ch : what) {
            if (ch == ' ' || ch == '\n') {
                ch = '_';
            }
        }
        emit("RESULT mode="+mode+" status=error clean=1 error="+what);
        _exit(3);
    }
}
