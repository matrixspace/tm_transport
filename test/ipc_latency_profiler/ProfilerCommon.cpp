#include "ProfilerCommon.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <fcntl.h>
#include <sched.h>
#include <sys/file.h>
#include <unistd.h>

namespace ipc_latency_profiler {

    namespace {
        inline int64_t monoRawNs() {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
            return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
        }
        //Brackets clock_gettime with two rdtsc reads and uses the midpoint,
        //retrying a few times to pick the tightest bracket.
        inline void pairedSample(int64_t &ns, uint64_t &tsc) {
            uint64_t bestWidth = ~0ULL;
            for (int ii = 0; ii < 16; ++ii) {
                uint64_t a = tscSend();
                int64_t t = monoRawNs();
                uint64_t b = tscSend();
                if (b - a < bestWidth) {
                    bestWidth = b - a;
                    ns = t;
                    tsc = a + (b - a) / 2;
                }
            }
        }
        double percentileNs(std::vector<int64_t> const &sorted, double p, double ticksPerNs) {
            if (sorted.empty()) {
                return 0.0;
            }
            auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size())));
            if (rank < 1) {
                rank = 1;
            }
            if (rank > sorted.size()) {
                rank = sorted.size();
            }
            return static_cast<double>(sorted[rank - 1]) / ticksPerNs;
        }
        std::string isoTimestamp() {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
            struct tm tmv;
            gmtime_r(&t, &tmv);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
            std::ostringstream oss;
            oss << buf << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
            return oss.str();
        }
        std::vector<int> parseCpuList(std::string const &s) {
            std::vector<int> ret;
            std::istringstream iss(s);
            std::string item;
            while (std::getline(iss, item, ',')) {
                if (!item.empty()) {
                    ret.push_back(std::stoi(item));
                }
            }
            return ret;
        }
    } // namespace

    double calibrateTscGHz(double seconds) {
        int64_t ns0, ns1;
        uint64_t tsc0, tsc1;
        pairedSample(ns0, tsc0);
        struct timespec req;
        req.tv_sec = static_cast<time_t>(seconds);
        req.tv_nsec = static_cast<long>((seconds - static_cast<double>(req.tv_sec)) * 1e9);
        nanosleep(&req, nullptr);
        pairedSample(ns1, tsc1);
        return static_cast<double>(tsc1 - tsc0) / static_cast<double>(ns1 - ns0);
    }

    TscOverhead measureTscOverhead(double ghz) {
        constexpr std::size_t kN = 200000;
        std::vector<int64_t> a(kN), b(kN), c(kN);
        //Spin ~100ms first: right after the calibration sleep the core may
        //not be at its steady state frequency yet.
        {
            uint64_t until = __rdtsc() + static_cast<uint64_t>(0.1 * 1e9 * ghz);
            while (__rdtsc() < until) {
            }
        }
        for (std::size_t ii = 0; ii < kN; ++ii) {
            uint64_t t0 = __rdtsc();
            uint64_t t1 = __rdtsc();
            a[ii] = static_cast<int64_t>(t1 - t0);
        }
        for (std::size_t ii = 0; ii < kN; ++ii) {
            uint64_t t0 = tscSend();
            uint64_t t1 = tscSend();
            b[ii] = static_cast<int64_t>(t1 - t0);
        }
        for (std::size_t ii = 0; ii < kN; ++ii) {
            uint64_t t0 = tscRecv();
            uint64_t t1 = tscRecv();
            c[ii] = static_cast<int64_t>(t1 - t0);
        }
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        std::sort(c.begin(), c.end());
        TscOverhead o;
        o.rdtscP50Ns = percentileNs(a, 0.5, ghz);
        o.rdtscMinNs = static_cast<double>(a.front()) / ghz;
        o.sendStampP50Ns = percentileNs(b, 0.5, ghz);
        o.recvStampP50Ns = percentileNs(c, 0.5, ghz);
        return o;
    }

    nlohmann::json toJson(TscOverhead const &o) {
        return nlohmann::json {
            {"rdtsc_back_to_back_p50", o.rdtscP50Ns}, {"rdtsc_back_to_back_min", o.rdtscMinNs}, {"send_stamp_lfence_rdtsc_p50", o.sendStampP50Ns}, {"recv_stamp_rdtscp_p50", o.recvStampP50Ns}};
    }

    LatencySummary summarize(std::vector<int64_t> &ticks, double ticksPerNs) {
        LatencySummary s {};
        s.n = ticks.size();
        if (ticks.empty()) {
            return s;
        }
        std::sort(ticks.begin(), ticks.end());
        long double sum = 0;
        for (auto t : ticks) {
            if (t < 0) {
                ++s.negative;
            }
            sum += t;
        }
        s.p50 = percentileNs(ticks, 0.5, ticksPerNs);
        s.p90 = percentileNs(ticks, 0.9, ticksPerNs);
        s.p99 = percentileNs(ticks, 0.99, ticksPerNs);
        s.p999 = percentileNs(ticks, 0.999, ticksPerNs);
        s.p9999 = percentileNs(ticks, 0.9999, ticksPerNs);
        s.max = static_cast<double>(ticks.back()) / ticksPerNs;
        s.min = static_cast<double>(ticks.front()) / ticksPerNs;
        s.mean = static_cast<double>(sum / static_cast<long double>(ticks.size())) / ticksPerNs;
        return s;
    }

    nlohmann::json toJson(LatencySummary const &s) {
        return nlohmann::json {
            {"n", s.n}, {"negative", s.negative}, {"min", s.min}, {"p50", s.p50}, {"p90", s.p90}, {"p99", s.p99}, {"p99_9", s.p999}, {"p99_99", s.p9999}, {"max", s.max}, {"mean", s.mean}};
    }

    std::string formatSummary(LatencySummary const &s) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1)
            << "n=" << s.n
            << " min=" << s.min
            << " p50=" << s.p50
            << " p90=" << s.p90
            << " p99=" << s.p99
            << " p99.9=" << s.p999
            << " p99.99=" << s.p9999
            << " max=" << s.max
            << " mean=" << s.mean
            << " (ns)";
        if (s.negative > 0) {
            oss << " negative=" << s.negative;
        }
        return oss.str();
    }

    std::string usageText(char const *prog) {
        std::ostringstream oss;
        oss << "Usage: " << prog << " --mode e2e|floor --role pub|sub [options]\n"
            << "  Normally launched by run_profile.sh, which handles pinning, ordering and timeouts.\n"
            << "Common:\n"
            << "  --payload N              payload bytes (default 256, " << kMinPayloadSize << ".." << kMaxPayloadSize << ")\n"
            << "  --interval-us X          publish interval (default 20)\n"
            << "  --warmup N               warmup messages excluded from stats (default 100000)\n"
            << "  --count N                measured messages (default 1000000)\n"
            << "  --results PATH           append JSON lines here\n"
            << "  --ready-file PATH        subscriber: create this file once ready to receive\n"
            << "  --cpu N                  core this process is pinned to (recorded only)\n"
            << "  --housekeeping-cpu N     move the main (non hot path) thread here after start\n"
            << "  --pub-cpu N --reader-cpus a,b,..  topology, recorded in results\n"
            << "  --n-readers N --reader-index I\n"
            << "  --calibrate-s X          TSC calibration duration (default 0.5, min 0.5)\n"
            << "  --git-branch S --git-commit S --run-id S --forced 0|1 --forced-reason S\n"
            << "e2e:\n"
            << "  --channel SPEC           MultiTransport channel, e.g. shared_memory_broadcast://:::name[busyLoop=true]\n"
            << "  --topic S                topic (default ipc.profile)\n"
            << "  --clock default|simplified\n"
            << "  --sink simple|custom\n"
            << "  --end-markers N --end-marker-spacing-ms N --start-delay-ms N\n"
            << "  --idle-timeout-s X       subscriber stops this long after the last message (default 5)\n"
            << "  --first-msg-timeout-s X  subscriber gives up if nothing arrives (default 60)\n"
            << "  --reader-ready-delay-ms N  subscriber: wait N ms after subscribing before signalling ready\n"
            << "  --raw PATH               subscriber: dump raw (seq, ticks) samples\n"
            << "  --floor-file PATH        subscriber: floor JSON lines to embed in results\n"
            << "floor:\n"
            << "  --floor-shm NAME         POSIX shm name shared by the floor pair\n"
            << "  --floor-file PATH        publisher: also append the floor result here\n";
        return oss.str();
    }

    std::optional<std::string> parseOptions(int argc, char **argv, Options &opt) {
        auto needValue = [&](int &ii) -> std::optional<std::string> {
            if (ii + 1 >= argc) {
                return std::nullopt;
            }
            return std::string(argv[++ii]);
        };
        int housekeeping = -1;
        try {
            for (int ii = 1; ii < argc; ++ii) {
                std::string k = argv[ii];
                if (k == "--help" || k == "-h") {
                    return std::string("help");
                }
                auto v = needValue(ii);
                if (!v) {
                    return "missing value for " + k;
                }
                if (k == "--mode") {
                    opt.mode = *v;
                } else if (k == "--role") {
                    opt.role = *v;
                } else if (k == "--channel") {
                    opt.channel = *v;
                } else if (k == "--topic") {
                    opt.topic = *v;
                } else if (k == "--clock") {
                    opt.clock = *v;
                } else if (k == "--sink") {
                    opt.sink = *v;
                } else if (k == "--payload") {
                    opt.payload = std::stoul(*v);
                } else if (k == "--interval-us") {
                    opt.intervalUs = std::stod(*v);
                } else if (k == "--warmup") {
                    opt.warmup = std::stoull(*v);
                } else if (k == "--count") {
                    opt.count = std::stoull(*v);
                } else if (k == "--end-markers") {
                    opt.endMarkers = std::stoi(*v);
                } else if (k == "--end-marker-spacing-ms") {
                    opt.endMarkerSpacingMs = std::stoi(*v);
                } else if (k == "--start-delay-ms") {
                    opt.startDelayMs = std::stoi(*v);
                } else if (k == "--idle-timeout-s") {
                    opt.idleTimeoutS = std::stod(*v);
                } else if (k == "--reader-ready-delay-ms") {
                    opt.readerReadyDelayMs = std::stoi(*v);
                } else if (k == "--first-msg-timeout-s") {
                    opt.firstMsgTimeoutS = std::stod(*v);
                } else if (k == "--calibrate-s") {
                    opt.calibrateS = std::max(0.5, std::stod(*v));
                } else if (k == "--results") {
                    opt.results = *v;
                } else if (k == "--raw") {
                    opt.raw = *v;
                } else if (k == "--ready-file") {
                    opt.readyFile = *v;
                } else if (k == "--floor-file") {
                    opt.floorFile = *v;
                } else if (k == "--floor-shm") {
                    opt.floorShm = *v;
                } else if (k == "--cpu") {
                    opt.cpu = std::stoi(*v);
                } else if (k == "--housekeeping-cpu") {
                    housekeeping = std::stoi(*v);
                } else if (k == "--pub-cpu") {
                    opt.pubCpu = std::stoi(*v);
                } else if (k == "--reader-cpus") {
                    opt.readerCpus = *v;
                } else if (k == "--n-readers") {
                    opt.nReaders = std::stoi(*v);
                } else if (k == "--reader-index") {
                    opt.readerIndex = std::stoi(*v);
                } else if (k == "--git-branch") {
                    opt.gitBranch = *v;
                } else if (k == "--git-commit") {
                    opt.gitCommit = *v;
                } else if (k == "--run-id") {
                    opt.runId = *v;
                } else if (k == "--forced") {
                    opt.forced = (*v == "1" || *v == "true");
                } else if (k == "--forced-reason") {
                    opt.forcedReason = *v;
                } else {
                    return "unknown option " + k;
                }
            }
        } catch (std::exception const &ex) {
            return std::string("bad option value: ") + ex.what();
        }
        opt.housekeepingCpu = housekeeping;
        if (opt.mode != "e2e" && opt.mode != "floor") {
            return "--mode must be e2e or floor";
        }
        if (opt.role != "pub" && opt.role != "sub") {
            return "--role must be pub or sub";
        }
        if (opt.payload < kMinPayloadSize || opt.payload > kMaxPayloadSize) {
            return "--payload out of range";
        }
        if (opt.intervalUs <= 0) {
            return "--interval-us must be positive";
        }
        if (opt.count == 0) {
            return "--count must be positive";
        }
        if (opt.mode == "e2e") {
            if (opt.channel.empty()) {
                return "--channel is required for e2e";
            }
            if (opt.clock != "default" && opt.clock != "simplified") {
                return "--clock must be default or simplified";
            }
            if (opt.sink != "simple" && opt.sink != "custom") {
                return "--sink must be simple or custom";
            }
        } else {
            if (opt.floorShm.empty()) {
                return "--floor-shm is required for floor";
            }
        }
        return std::nullopt;
    }

    nlohmann::json loadavgJson() {
        std::ifstream ifs("/proc/loadavg");
        double a = -1, b = -1, c = -1;
        ifs >> a >> b >> c;
        return nlohmann::json::array({a, b, c});
    }

    nlohmann::json commonJson(Options const &opt, double ghz) {
        nlohmann::json readers = nlohmann::json::array();
        for (auto c : parseCpuList(opt.readerCpus)) {
            readers.push_back(c);
        }
        nlohmann::json j;
        j["timestamp"] = isoTimestamp();
        j["run_id"] = opt.runId;
        j["git"] = {{"branch", opt.gitBranch}, {"commit", opt.gitCommit}};
        j["mode"] = opt.mode;
        j["role"] = opt.role;
        j["payload_bytes"] = opt.payload;
        j["interval_us"] = opt.intervalUs;
        j["n_readers"] = opt.nReaders;
        j["reader_index"] = opt.readerIndex;
        j["cores"] = {{"self", opt.cpu}, {"publisher", opt.pubCpu}, {"readers", readers}, {"housekeeping", opt.housekeepingCpu}};
        j["tsc_ghz"] = ghz;
        j["forced"] = opt.forced;
        if (opt.forced) {
            j["forced_reason"] = opt.forcedReason;
        }
        return j;
    }

    bool appendJsonLine(std::string const &path, nlohmann::json const &j) {
        if (path.empty()) {
            return true;
        }
        int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd < 0) {
            std::perror(("open " + path).c_str());
            return false;
        }
        ::flock(fd, LOCK_EX);
        std::string line = j.dump() + "\n";
        bool ok = (::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size()));
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return ok;
    }

    bool writeReadyFile(std::string const &path) {
        if (path.empty()) {
            return true;
        }
        //Write to a temp name and rename, so the script never sees a partial file.
        std::string tmp = path + ".tmp";
        {
            std::ofstream ofs(tmp);
            if (!ofs) {
                return false;
            }
            ofs << ::getpid() << "\n";
        }
        return std::rename(tmp.c_str(), path.c_str()) == 0;
    }

    bool writeRawSamples(std::string const &path, RawSample const *samples, std::size_t n, double ghz) {
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (!f) {
            return false;
        }
        char magic[8] = {'I', 'P', 'C', 'L', 'A', 'T', '1', '\0'};
        uint64_t count = n;
        bool ok = std::fwrite(magic, 1, 8, f) == 8
            && std::fwrite(&count, sizeof(count), 1, f) == 1
            && std::fwrite(&ghz, sizeof(ghz), 1, f) == 1
            && (n == 0 || std::fwrite(samples, sizeof(RawSample), n, f) == n);
        return (std::fclose(f) == 0) && ok;
    }

    void moveCurrentThreadTo(int cpu) {
        if (cpu < 0) {
            return;
        }
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0) {
            std::perror("sched_setaffinity(housekeeping)");
        }
    }

} // namespace ipc_latency_profiler
