#ifndef TM_KIT_TRANSPORT_TEST_IPC_LATENCY_PROFILER_PROFILER_COMMON_HPP_
#define TM_KIT_TRANSPORT_TEST_IPC_LATENCY_PROFILER_PROFILER_COMMON_HPP_

#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <x86intrin.h>

#include <nlohmann/json.hpp>

//Shared pieces of the IPC latency profiler: TSC helpers, payload layout,
//pacing, statistics, command line options and result output.
//Nothing in this header depends on tm_kit, so the floor mode can be built
//without pulling in the transport headers.

namespace ipc_latency_profiler {

    //Payload layout, written by the publisher right before the message is
    //handed to the graph (little endian, native):
    //  [0,8)   seq (starts at 0 for e2e, 1 for floor)
    //  [8,16)  TSC at hand-off
    //  [16,24) flags (kFlagWarmup, kFlagEnd)
    //  [24,32) warmup count (first measured seq)
    //  [32,40) total data messages (warmup + measured), end markers excluded
    inline constexpr std::size_t kSeqOffset = 0;
    inline constexpr std::size_t kTscOffset = 8;
    inline constexpr std::size_t kFlagsOffset = 16;
    inline constexpr std::size_t kWarmupOffset = 24;
    inline constexpr std::size_t kTotalOffset = 32;
    inline constexpr std::size_t kMinPayloadSize = 40;
    inline constexpr std::size_t kMaxPayloadSize = 4096;
    inline constexpr uint64_t kFlagWarmup = 1;
    inline constexpr uint64_t kFlagEnd = 2;

    //Send-side stamp: lfence keeps rdtsc from being hoisted above earlier work.
    inline uint64_t tscSend() {
        _mm_lfence();
        return __rdtsc();
    }
    //Receive-side stamp: rdtscp waits for all earlier loads to complete.
    inline uint64_t tscRecv() {
        unsigned int aux;
        return __rdtscp(&aux);
    }

    inline void putU64(char *p, uint64_t v) {
        std::memcpy(p, &v, sizeof(uint64_t));
    }
    inline uint64_t getU64(char const *p) {
        uint64_t v;
        std::memcpy(&v, p, sizeof(uint64_t));
        return v;
    }

    //Busy-wait pacing on an absolute TSC schedule. If the caller falls
    //behind by more than one interval (preemption etc.) the schedule is
    //reset instead of bursting to catch up.
    class Pacer {
    private:
        uint64_t interval_;
        uint64_t next_;
        uint64_t late_;

    public:
        explicit Pacer(uint64_t intervalTicks)
            : interval_(intervalTicks)
            , next_(__rdtsc() + intervalTicks)
            , late_(0) {}
        inline void wait() {
            uint64_t now;
            while ((now = __rdtsc()) < next_) {
                _mm_pause();
            }
            if (now - next_ > interval_) {
                ++late_;
                next_ = now;
            }
            next_ += interval_;
        }
        uint64_t late() const {
            return late_;
        }
    };

    //Calibrates TSC frequency against CLOCK_MONOTONIC_RAW over the given duration.
    double calibrateTscGHz(double seconds);

    struct TscOverhead {
        double rdtscP50Ns;
        double rdtscMinNs;
        double sendStampP50Ns;
        double recvStampP50Ns;
    };
    //Back-to-back stamp cost measured in-process.
    TscOverhead measureTscOverhead(double ghz);
    nlohmann::json toJson(TscOverhead const &o);

    //Trivially copyable so that the floor mode can pass it through shared memory.
    struct LatencySummary {
        uint64_t n;
        uint64_t negative;
        double p50;
        double p90;
        double p99;
        double p999;
        double p9999;
        double max;
        double min;
        double mean;
    };
    //Sorts ticks in place. Values are converted with ns = ticks / ticksPerNs.
    LatencySummary summarize(std::vector<int64_t> &ticks, double ticksPerNs);
    nlohmann::json toJson(LatencySummary const &s);
    std::string formatSummary(LatencySummary const &s);

    struct Options {
        std::string mode = "e2e"; //e2e | floor
        std::string role; //pub | sub
        std::string channel;
        std::string topic = "ipc.profile";
        std::string clock = "default"; //default | simplified
        std::string sink = "simple"; //simple | custom
        std::size_t payload = 256;
        double intervalUs = 20.0;
        uint64_t warmup = 100000;
        uint64_t count = 1000000;
        int endMarkers = 5;
        int endMarkerSpacingMs = 20;
        int startDelayMs = 500;
        double idleTimeoutS = 5.0;
        double firstMsgTimeoutS = 60.0;
        int readerReadyDelayMs = 0; //sub: extra wait after subscribing, before signalling ready
        double calibrateS = 0.5;
        std::string results;
        std::string raw;
        std::string readyFile;
        std::string floorFile; //floor pub: output copy; e2e sub: input to embed
        std::string floorShm; //floor: POSIX shm name
        int cpu = -1;
        int housekeepingCpu = -1;
        int pubCpu = -1;
        std::string readerCpus;
        int nReaders = 1;
        int readerIndex = 0;
        std::string gitBranch;
        std::string gitCommit;
        std::string runId;
        bool forced = false;
        std::string forcedReason;
    };
    //Returns an error message on failure.
    std::optional<std::string> parseOptions(int argc, char **argv, Options &opt);
    std::string usageText(char const *prog);

    nlohmann::json loadavgJson();
    //Fields shared by every JSON line written by any role.
    nlohmann::json commonJson(Options const &opt, double ghz);
    //Appends one line under an exclusive flock, in a single write().
    bool appendJsonLine(std::string const &path, nlohmann::json const &j);
    bool writeReadyFile(std::string const &path);

    struct RawSample {
        uint64_t seq;
        int64_t ticks;
    };
    //Binary layout: "IPCLAT1\0", uint64 count, double ghz, then RawSample[count].
    bool writeRawSamples(std::string const &path, RawSample const *samples, std::size_t n, double ghz);

    //Moves the calling thread off the hot core once the graph is running.
    void moveCurrentThreadTo(int cpu);

    int runFloor(Options const &opt);

} // namespace ipc_latency_profiler

#endif
