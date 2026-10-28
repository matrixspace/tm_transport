#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>

#include <tm_kit/basic/ByteData.hpp>
#include <tm_kit/basic/SpdLoggingComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>

#include <tm_kit/transport/MultiTransportBroadcastListenerManagingUtils.hpp>
#include <tm_kit/transport/MultiTransportBroadcastPublisherManagingUtils.hpp>
#ifdef TM_KIT_TRANSPORT_AERON_IPC
#include <tm_kit/transport/aeron_ipc/AeronIpcComponent.hpp>
#endif

#include "ProfilerCommon.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>

#include <unistd.h>

//One-way latency profiler for MultiTransport broadcast channels.
//
//  publisher process:  PacedSource (importer) -> oneByteDataBroadcastPublisher
//  subscriber process: oneByteDataBroadcastListener -> sink (exporter)
//
//The measured interval starts right before the publisher importer hands the
//ByteDataWithTopic to the graph (so it includes pureInnerData/resolveTime and
//the exporter) and ends at the first instruction of the subscriber sink's
//user code (so it includes the listener's pureInnerData and, for the simple
//sink, the SimpleExporter TimeChecker mutex). Serialization of user types is
//not involved since the ByteData path is used. The channel string selects the
//transport, so new transports only need a new channel prefix.
//
//All hot path nodes are non-threaded. Each process is expected to be pinned
//to one core by run_profile.sh; the main thread moves itself to the
//housekeeping core once the graph is started.

using namespace dev::cd606::tm;
using namespace ipc_latency_profiler;

namespace {

    // clang-format off
    template <class ClockComponent>
    using ProfilerEnvironment = infra::Environment<
        infra::CheckTimeComponent<false>
        , infra::LockFreeTimeCheckerComponent
        , infra::TrivialExitControlComponent
        , basic::TimeComponentEnhancedWithSpdLogging<ClockComponent, false>
        , transport::AllNetworkTransportComponents
#ifdef TM_KIT_TRANSPORT_AERON_IPC
        , transport::aeron_ipc::AeronIpcComponent
#endif
    >;
    // clang-format on

    double secondsSince(std::chrono::steady_clock::time_point const &t) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    }

    //Never returns. Skips destructors of the transport components, some of
    //which join background threads that may block.
    [[noreturn]] void finish(int rc) {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(nullptr);
        ::_exit(rc);
    }

    // ---------------------------------------------------------------- publisher

    struct PublisherState {
        std::vector<int64_t> handoffTicks; //publish() call duration per measured message
        std::atomic<bool> done {false};
        uint64_t sent = 0;
        uint64_t endMarkersSent = 0;
        uint64_t pacingLate = 0;
    };

    template <class M>
    class PacedSource final : public M::template AbstractImporter<basic::ByteDataWithTopic> {
    private:
        using Env = typename M::EnvironmentType;
        Options opt_;
        double ghz_;
        PublisherState *state_;

    public:
        PacedSource(Options const &opt, double ghz, PublisherState *state)
            : opt_(opt)
            , ghz_(ghz)
            , state_(state) {}
        virtual void start(Env *env) override final {
            //The thread inherits the hot core affinity of the main thread.
            std::thread th([this, env]() {
                run(env);
            });
            th.detach();
        }

    private:
        void run(Env *env) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt_.startDelayMs));
            uint64_t const warmup = opt_.warmup;
            uint64_t const total = opt_.warmup + opt_.count;
            Pacer pacer(static_cast<uint64_t>(opt_.intervalUs * 1000.0 * ghz_));
            for (uint64_t seq = 0; seq < total; ++seq) {
                pacer.wait();
                basic::ByteDataWithTopic d {opt_.topic, std::string(opt_.payload, '\0')};
                char *p = d.content.data();
                putU64(p + kFlagsOffset, (seq < warmup) ? kFlagWarmup : 0);
                putU64(p + kWarmupOffset, warmup);
                putU64(p + kTotalOffset, total);
                putU64(p + kSeqOffset, seq);
                uint64_t t0 = tscSend();
                putU64(p + kTscOffset, t0);
                this->publish(M::template pureInnerData<basic::ByteDataWithTopic>(env, std::move(d)));
                uint64_t t1 = tscRecv();
                if (seq >= warmup) {
                    state_->handoffTicks[seq - warmup] = static_cast<int64_t>(t1 - t0);
                }
            }
            state_->sent = total;
            state_->pacingLate = pacer.late();
            for (int ii = 0; ii < opt_.endMarkers; ++ii) {
                std::this_thread::sleep_for(std::chrono::milliseconds(opt_.endMarkerSpacingMs));
                basic::ByteDataWithTopic d {opt_.topic, std::string(opt_.payload, '\0')};
                char *p = d.content.data();
                putU64(p + kFlagsOffset, kFlagEnd);
                putU64(p + kWarmupOffset, warmup);
                putU64(p + kTotalOffset, total);
                putU64(p + kSeqOffset, total + static_cast<uint64_t>(ii));
                putU64(p + kTscOffset, tscSend());
                this->publish(M::template pureInnerData<basic::ByteDataWithTopic>(env, std::move(d)));
                ++state_->endMarkersSent;
            }
            state_->done.store(true, std::memory_order_release);
        }
    };

    template <class M>
    int runPublisher(Options const &opt) {
        using Env = typename M::EnvironmentType;
        using R = infra::AppRunner<M>;

        auto loadBefore = loadavgJson();
        double ghz = calibrateTscGHz(opt.calibrateS);
        TscOverhead overhead = measureTscOverhead(ghz);

        auto *state = new PublisherState();
        state->handoffTicks.assign(opt.count, 0);

        auto *env = new Env();
        auto *r = new R(env);
        auto source = M::importer(new PacedSource<M>(opt, ghz, state));
        auto sink = transport::MultiTransportBroadcastPublisherManagingUtils<R>::oneByteDataBroadcastPublisher(
            *r, "publisher", opt.channel);
        r->connect(r->importItem("source", source), sink);
        r->finalize();
        moveCurrentThreadTo(opt.housekeepingCpu);

        while (!state->done.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto handoff = summarize(state->handoffTicks, ghz);
        auto j = commonJson(opt, ghz);
        j["record_type"] = "e2e_publisher";
        j["channel"] = opt.channel;
        j["topic"] = opt.topic;
        j["variant"] = {{"clock", opt.clock}, {"sink", opt.sink}};
        j["msgs"] = {{"warmup", opt.warmup}, {"measured", opt.count}, {"sent", state->sent}, {"end_markers", state->endMarkersSent}};
        j["publish_call_ns"] = toJson(handoff);
        j["pacing_late"] = state->pacingLate;
        j["stamping_overhead_ns"] = toJson(overhead);
        j["loadavg_before"] = loadBefore;
        j["loadavg_after"] = loadavgJson();
        bool written = appendJsonLine(opt.results, j);

        std::cout << "e2e publisher (cpu " << opt.cpu << ", " << opt.channel << ", clock=" << opt.clock
                  << ", sink=" << opt.sink << ", tsc " << ghz << " GHz)\n"
                  << "  sent " << state->sent << " (+" << state->endMarkersSent << " end markers), pacing late " << state->pacingLate << "\n"
                  << "  publish() call: " << formatSummary(handoff) << std::endl;
        finish(written ? 0 : 1);
    }

    // --------------------------------------------------------------- subscriber

    //Written only by the delivery thread except for the atomics. The main
    //thread reads the plain fields after setting stop_ and waiting.
    class Recorder {
    private:
        uint64_t warmup_;
        uint64_t count_;
        std::vector<RawSample> samples_;
        std::vector<uint8_t> seen_;
        std::size_t n_ = 0;
        uint64_t expectedNext_ = 0;

    public:
        uint64_t gapEvents = 0;
        uint64_t outOfOrder = 0;
        uint64_t warmupReceived = 0;
        uint64_t duplicates = 0;
        uint64_t outOfRange = 0;
        uint64_t malformed = 0;
        uint64_t endMarkers = 0;
        uint64_t totalReported = 0;
        std::atomic<uint64_t> lastArrival {0};
        std::atomic<bool> endSeen {false};
        std::atomic<bool> stop {false};

        Recorder(uint64_t warmup, uint64_t count)
            : warmup_(warmup)
            , count_(count)
            , samples_(count, RawSample {0, 0})
            , seen_(count, 0) {}
        inline void onMessage(uint64_t t, std::string const &content) {
            if (stop.load(std::memory_order_relaxed)) {
                return;
            }
            lastArrival.store(t, std::memory_order_relaxed);
            if (content.size() < kMinPayloadSize) {
                ++malformed;
                return;
            }
            char const *p = content.data();
            if (getU64(p + kFlagsOffset) & kFlagEnd) {
                ++endMarkers;
                totalReported = getU64(p + kTotalOffset);
                endSeen.store(true, std::memory_order_release);
                return;
            }
            uint64_t seq = getU64(p + kSeqOffset);
            int64_t ticks = static_cast<int64_t>(t - getU64(p + kTscOffset));
            if (seq > expectedNext_) {
                ++gapEvents;
            } else if (seq < expectedNext_) {
                ++outOfOrder;
            }
            if (seq >= expectedNext_) {
                expectedNext_ = seq + 1;
            }
            if (seq < warmup_) {
                ++warmupReceived;
                return;
            }
            uint64_t idx = seq - warmup_;
            if (idx >= count_) {
                ++outOfRange;
                return;
            }
            if (seen_[idx]) {
                ++duplicates;
                return;
            }
            seen_[idx] = 1;
            samples_[n_++] = RawSample {seq, ticks};
        }
        std::size_t received() const {
            return n_;
        }
        RawSample const *samples() const {
            return samples_.data();
        }
    };

    //Hand-written exporter: no TimeChecker, no node setter, no tracer.
    template <class M>
    class CustomSink final : public M::template AbstractExporter<basic::ByteDataWithTopic> {
    private:
        Recorder *rec_;

    public:
        explicit CustomSink(Recorder *rec)
            : rec_(rec) {}
        virtual void start(typename M::EnvironmentType *) override final {}
        virtual void handle(typename M::template InnerData<basic::ByteDataWithTopic> &&d) override final {
            uint64_t t = tscRecv();
            rec_->onMessage(t, d.timedData.value.content);
        }
    };

    template <class M, bool UseCustomSink>
    auto createSink(Recorder *rec) {
        if constexpr (UseCustomSink) {
            return M::exporter(new CustomSink<M>(rec));
        } else {
            //SimpleExporter<T,F,false>: TimeChecker<true> (mutex) before calling us.
            return M::template simpleExporter<basic::ByteDataWithTopic>(
                [rec](typename M::template InnerData<basic::ByteDataWithTopic> &&d) {
                    uint64_t t = tscRecv();
                    rec->onMessage(t, d.timedData.value.content);
                });
        }
    }

    std::optional<nlohmann::json> findFloorRecord(std::string const &path, int readerCpu) {
        if (path.empty()) {
            return std::nullopt;
        }
        std::ifstream ifs(path);
        std::string line;
        std::optional<nlohmann::json> found;
        while (std::getline(ifs, line)) {
            try {
                auto j = nlohmann::json::parse(line);
                if (j.value("record_type", "") == "floor" && j["floor_pair"]["reader_cpu"] == std::to_string(readerCpu)) {
                    found = j;
                }
            } catch (...) {
            }
        }
        return found;
    }

    template <class M, bool UseCustomSink>
    int runSubscriber(Options const &opt) {
        using Env = typename M::EnvironmentType;
        using R = infra::AppRunner<M>;

        auto loadBefore = loadavgJson();
        double ghz = calibrateTscGHz(opt.calibrateS);
        TscOverhead overhead = measureTscOverhead(ghz);

        auto *rec = new Recorder(opt.warmup, opt.count);

        auto *env = new Env();
        auto *r = new R(env);
        auto source = transport::MultiTransportBroadcastListenerManagingUtils<R>::oneByteDataBroadcastListener(
            *r, "listener", opt.channel, opt.topic);
        auto sink = createSink<M, UseCustomSink>(rec);
        r->connect(std::move(source), r->exporterAsSink("sink", sink));
        r->finalize();
        moveCurrentThreadTo(opt.housekeepingCpu);

        //finalize() has run every importer's start(), i.e. the transport's
        //addSubscriptionClient has returned. Transports that connect
        //asynchronously can ask for an extra settle time before the
        //publisher is allowed to start.
        if (opt.readerReadyDelayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.readerReadyDelayMs));
        }
        if (!writeReadyFile(opt.readyFile)) {
            std::cerr << "subscriber: cannot write ready file " << opt.readyFile << "\n";
            finish(2);
        }

        auto started = std::chrono::steady_clock::now();
        std::string stopReason;
        uint64_t const idleTicks = static_cast<uint64_t>(opt.idleTimeoutS * 1e9 * ghz);
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (rec->endSeen.load(std::memory_order_acquire)) {
                stopReason = "end_marker";
                break;
            }
            uint64_t last = rec->lastArrival.load(std::memory_order_relaxed);
            if (last == 0) {
                if (secondsSince(started) > opt.firstMsgTimeoutS) {
                    stopReason = "no_data_timeout";
                    break;
                }
            } else if (static_cast<int64_t>(__rdtsc() - last) > static_cast<int64_t>(idleTicks)) {
                //Signed: last comes from another core and may be a few ticks ahead.
                stopReason = "idle_timeout";
                break;
            }
        }
        rec->stop.store(true, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::atomic_thread_fence(std::memory_order_acquire);

        std::size_t n = rec->received();
        std::vector<int64_t> ticks(n);
        for (std::size_t ii = 0; ii < n; ++ii) {
            ticks[ii] = rec->samples()[ii].ticks;
        }
        auto lat = summarize(ticks, ghz);
        uint64_t loss = opt.count - n;
        double lossRatio = static_cast<double>(loss) / static_cast<double>(opt.count);

        auto j = commonJson(opt, ghz);
        j["record_type"] = "e2e_reader";
        j["channel"] = opt.channel;
        j["topic"] = opt.topic;
        j["variant"] = {{"clock", opt.clock}, {"sink", opt.sink}};
        j["stop_reason"] = stopReason;
        j["reader_ready_delay_ms"] = opt.readerReadyDelayMs;
        j["msgs"] = {
            {"warmup", opt.warmup}, {"measured_expected", opt.count}, {"measured_received", n}, {"warmup_received", rec->warmupReceived}, {"duplicates", rec->duplicates}, {"out_of_range", rec->outOfRange}, {"malformed", rec->malformed}, {"end_markers_received", rec->endMarkers}, {"total_sent_reported", rec->totalReported}};
        j["loss"] = {{"count", loss}, {"ratio", lossRatio}, {"gap_events", rec->gapEvents}, {"out_of_order", rec->outOfOrder}};
        j["latency_ns"] = toJson(lat);
        j["stamping_overhead_ns"] = toJson(overhead);
        j["loadavg_before"] = loadBefore;
        j["loadavg_after"] = loadavgJson();
        auto floor = findFloorRecord(opt.floorFile, opt.cpu);
        if (floor) {
            j["floor"] = {
                {"one_way_ns", (*floor)["one_way_ns"]}, {"rtt_half_ns", (*floor)["rtt_half_ns"]}, {"one_way_minus_rtt_half_p50_ns", (*floor)["one_way_minus_rtt_half_p50_ns"]}, {"ok", (*floor)["ok"]}, {"msgs", (*floor)["msgs"]}};
        } else {
            j["floor"] = nullptr;
        }
        bool written = appendJsonLine(opt.results, j);
        if (!opt.raw.empty()) {
            if (!writeRawSamples(opt.raw, rec->samples(), n, ghz)) {
                std::cerr << "subscriber: failed to write raw samples to " << opt.raw << "\n";
                written = false;
            }
        }

        std::cout << "e2e reader " << opt.readerIndex << " (cpu " << opt.cpu << ", " << opt.channel << ", clock=" << opt.clock
                  << ", sink=" << opt.sink << ", tsc " << ghz << " GHz, stop=" << stopReason << ")\n"
                  << "  received " << n << "/" << opt.count << " measured, loss " << loss << " (" << lossRatio * 100.0
                  << "%), gap events " << rec->gapEvents << ", out of order " << rec->outOfOrder
                  << ", duplicates " << rec->duplicates << ", end markers " << rec->endMarkers << "\n"
                  << "  latency: " << formatSummary(lat) << std::endl;
        if (floor) {
            std::cout << "  floor one-way p50 " << (*floor)["one_way_ns"]["p50"] << " ns, RTT/2 p50 " << (*floor)["rtt_half_ns"]["p50"] << " ns" << std::endl;
        }
        int rc = 0;
        if (stopReason == "no_data_timeout") {
            rc = 3;
        } else if (!written) {
            rc = 1;
        }
        finish(rc);
    }

    template <class ClockComponent, bool UseCustomSink>
    int runE2E(Options const &opt) {
        using Env = ProfilerEnvironment<ClockComponent>;
        using M = infra::RealTimeApp<Env>;
        if (opt.role == "sub") {
            return runSubscriber<M, UseCustomSink>(opt);
        }
        return runPublisher<M>(opt);
    }

} // namespace

int main(int argc, char **argv) {
    Options opt;
    auto err = parseOptions(argc, argv, opt);
    if (err) {
        if (*err == "help") {
            std::cout << usageText(argv[0]);
            return 0;
        }
        std::cerr << "Error: " << *err << "\n"
                  << usageText(argv[0]);
        return 2;
    }
    try {
        if (opt.mode == "floor") {
            return runFloor(opt);
        }
        if (opt.clock == "default") {
            if (opt.sink == "simple") {
                return runE2E<basic::real_time_clock::ClockComponent, false>(opt);
            }
            return runE2E<basic::real_time_clock::ClockComponent, true>(opt);
        }
        if (opt.clock == "tsc") {
            if (opt.sink == "simple") {
                return runE2E<basic::real_time_clock::TscClockComponent, false>(opt);
            }
            return runE2E<basic::real_time_clock::TscClockComponent, true>(opt);
        }
        if (opt.sink == "simple") {
            return runE2E<basic::real_time_clock::SimplifiedClockComponent, false>(opt);
        }
        return runE2E<basic::real_time_clock::SimplifiedClockComponent, true>(opt);
    } catch (std::exception const &ex) {
        std::cerr << "Fatal: " << ex.what() << std::endl;
        finish(4);
    }
}
