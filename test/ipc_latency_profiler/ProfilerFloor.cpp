#include "ProfilerCommon.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <new>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

//Hardware floor: two processes pinned to the same cores as the e2e run,
//exchanging a payload of the same size through a /dev/shm mapping.
//  phase 1 (one-way): publisher stamps TSC into its local buffer, copies the
//    buffer into the ping slot and releases the sequence number; the
//    subscriber spins, copies the slot out and reads TSC. The subscriber acks
//    each message so there is never more than one in flight.
//  phase 2 (RTT): publisher reads TSC, copies into ping, releases; subscriber
//    copies ping -> pong and releases; publisher spins, copies pong out and
//    reads TSC. RTT/2 is reported.
//Both phases use plain spinning (no pause instruction). Agreement between
//one-way p50 and RTT/2 p50 checks that the TSCs of the two cores are in sync.

namespace ipc_latency_profiler {

    namespace {
        struct FloorShm {
            alignas(64) std::atomic<uint64_t> pingSeq;
            alignas(64) char ping[kMaxPayloadSize];
            alignas(64) std::atomic<uint64_t> pongSeq;
            alignas(64) char pong[kMaxPayloadSize];
            alignas(64) std::atomic<uint32_t> subDone;
            std::atomic<uint32_t> pubDone;
            std::atomic<uint32_t> subFailed;
            double subGhz;
            LatencySummary oneWay;
            TscOverhead subOverhead;
        };

        //Spins without pause until pred() holds. Checks the deadline every 4096
        //iterations so that a dead peer cannot hang the process forever.
        template <class Pred>
        inline bool spinUntil(Pred const &pred, uint64_t timeoutTicks) {
            uint64_t start = __rdtsc();
            uint32_t k = 0;
            while (!pred()) {
                if ((++k & 0xFFFu) == 0 && __rdtsc() - start > timeoutTicks) {
                    return false;
                }
            }
            return true;
        }

        std::string shmPath(std::string const &name) {
            return (name[0] == '/') ? name : ("/" + name);
        }

        int runFloorSubscriber(Options const &opt) {
            double ghz = calibrateTscGHz(opt.calibrateS);
            TscOverhead overhead = measureTscOverhead(ghz);
            uint64_t const timeoutTicks = static_cast<uint64_t>(30.0 * 1e9 * ghz);

            std::string name = shmPath(opt.floorShm);
            ::shm_unlink(name.c_str());
            int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            if (fd < 0) {
                std::perror("shm_open(floor sub)");
                return 2;
            }
            if (::ftruncate(fd, sizeof(FloorShm)) != 0) {
                std::perror("ftruncate(floor sub)");
                ::close(fd);
                ::shm_unlink(name.c_str());
                return 2;
            }
            void *mem = ::mmap(nullptr, sizeof(FloorShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ::close(fd);
            if (mem == MAP_FAILED) {
                std::perror("mmap(floor sub)");
                ::shm_unlink(name.c_str());
                return 2;
            }
            auto *shm = new (mem) FloorShm {};
            shm->subGhz = ghz;
            shm->subOverhead = overhead;

            uint64_t const perPhase = opt.warmup + opt.count;
            std::vector<int64_t> ticks(opt.count, 0);
            std::vector<char> local(opt.payload, 0);
            std::size_t n = 0;

            if (!writeReadyFile(opt.readyFile)) {
                std::cerr << "floor sub: cannot write ready file " << opt.readyFile << "\n";
                ::shm_unlink(name.c_str());
                return 2;
            }

            bool ok = true;
            for (uint64_t s = 1; s <= 2 * perPhase; ++s) {
                if (!spinUntil([shm, s]() { return shm->pingSeq.load(std::memory_order_acquire) == s; }, timeoutTicks)) {
                    std::cerr << "floor sub: timed out waiting for seq " << s << "\n";
                    ok = false;
                    break;
                }
                std::memcpy(local.data(), shm->ping, opt.payload);
                if (s <= perPhase) {
                    uint64_t t = tscRecv();
                    if (s > opt.warmup) {
                        ticks[n++] = static_cast<int64_t>(t - getU64(local.data() + kTscOffset));
                    }
                } else {
                    std::memcpy(shm->pong, local.data(), opt.payload);
                }
                shm->pongSeq.store(s, std::memory_order_release);
            }

            ticks.resize(n);
            shm->oneWay = summarize(ticks, ghz);
            shm->subFailed.store(ok ? 0 : 1, std::memory_order_release);
            shm->subDone.store(1, std::memory_order_release);
            std::cout << "floor sub (cpu " << opt.cpu << "): one-way " << formatSummary(shm->oneWay) << std::endl;

            //Give the publisher a moment to read the summary before unlinking.
            for (int ii = 0; ii < 500 && shm->pubDone.load(std::memory_order_acquire) == 0; ++ii) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ::munmap(mem, sizeof(FloorShm));
            ::shm_unlink(name.c_str());
            return ok ? 0 : 1;
        }

        int runFloorPublisher(Options const &opt) {
            auto loadBefore = loadavgJson();
            double ghz = calibrateTscGHz(opt.calibrateS);
            TscOverhead overhead = measureTscOverhead(ghz);
            uint64_t const timeoutTicks = static_cast<uint64_t>(30.0 * 1e9 * ghz);

            std::string name = shmPath(opt.floorShm);
            int fd = -1;
            for (int ii = 0; ii < 500 && fd < 0; ++ii) {
                fd = ::shm_open(name.c_str(), O_RDWR, 0600);
                if (fd < 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            if (fd < 0) {
                std::perror("shm_open(floor pub)");
                return 2;
            }
            void *mem = ::mmap(nullptr, sizeof(FloorShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ::close(fd);
            if (mem == MAP_FAILED) {
                std::perror("mmap(floor pub)");
                return 2;
            }
            auto *shm = static_cast<FloorShm *>(mem);

            uint64_t const perPhase = opt.warmup + opt.count;
            std::vector<int64_t> rtt(opt.count, 0);
            std::vector<char> local(opt.payload, 0);
            std::size_t n = 0;
            Pacer pacer(static_cast<uint64_t>(opt.intervalUs * 1000.0 * ghz));
            bool ok = true;

            for (uint64_t s = 1; s <= perPhase && ok; ++s) {
                if (!spinUntil([shm, s]() { return shm->pongSeq.load(std::memory_order_acquire) == s - 1; }, timeoutTicks)) {
                    std::cerr << "floor pub: timed out waiting for ack " << (s - 1) << "\n";
                    ok = false;
                    break;
                }
                pacer.wait();
                putU64(local.data() + kSeqOffset, s);
                putU64(local.data() + kTscOffset, tscSend());
                std::memcpy(shm->ping, local.data(), opt.payload);
                shm->pingSeq.store(s, std::memory_order_release);
            }
            if (ok && !spinUntil([shm, perPhase]() { return shm->pongSeq.load(std::memory_order_acquire) == perPhase; }, timeoutTicks)) {
                ok = false;
            }
            for (uint64_t s = perPhase + 1; s <= 2 * perPhase && ok; ++s) {
                pacer.wait();
                uint64_t t0 = tscSend();
                putU64(local.data() + kSeqOffset, s);
                std::memcpy(shm->ping, local.data(), opt.payload);
                shm->pingSeq.store(s, std::memory_order_release);
                if (!spinUntil([shm, s]() { return shm->pongSeq.load(std::memory_order_acquire) == s; }, timeoutTicks)) {
                    std::cerr << "floor pub: timed out waiting for pong " << s << "\n";
                    ok = false;
                    break;
                }
                std::memcpy(local.data(), shm->pong, opt.payload);
                uint64_t t1 = tscRecv();
                if (s - perPhase > opt.warmup) {
                    rtt[n++] = static_cast<int64_t>(t1 - t0);
                }
            }

            for (int ii = 0; ii < 3000 && shm->subDone.load(std::memory_order_acquire) == 0; ++ii) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            bool subOk = shm->subDone.load(std::memory_order_acquire) != 0 && shm->subFailed.load(std::memory_order_acquire) == 0;
            LatencySummary oneWay = shm->oneWay;
            double subGhz = shm->subGhz;
            TscOverhead subOverhead = shm->subOverhead;
            shm->pubDone.store(1, std::memory_order_release);
            ::munmap(mem, sizeof(FloorShm));

            rtt.resize(n);
            //RTT/2: halve by converting with twice the ticks-per-ns.
            LatencySummary rttHalf = summarize(rtt, 2.0 * ghz);

            auto j = commonJson(opt, ghz);
            j["record_type"] = "floor";
            j["ok"] = ok && subOk;
            j["floor_pair"] = {{"publisher_cpu", opt.pubCpu}, {"reader_cpu", opt.readerCpus}};
            j["msgs"] = {{"warmup", opt.warmup}, {"measured", opt.count}};
            j["one_way_ns"] = toJson(oneWay);
            j["rtt_half_ns"] = toJson(rttHalf);
            j["one_way_minus_rtt_half_p50_ns"] = oneWay.p50 - rttHalf.p50;
            j["sub_tsc_ghz"] = subGhz;
            j["stamping_overhead_ns"] = toJson(overhead);
            j["sub_stamping_overhead_ns"] = toJson(subOverhead);
            j["pacing_late"] = pacer.late();
            j["loadavg_before"] = loadBefore;
            j["loadavg_after"] = loadavgJson();

            std::cout << "floor pub (cpu " << opt.cpu << " -> " << opt.readerCpus << ", " << opt.payload << "B, tsc " << ghz << " GHz)\n"
                      << "  one-way : " << formatSummary(oneWay) << "\n"
                      << "  RTT/2   : " << formatSummary(rttHalf) << "\n"
                      << "  one-way p50 - RTT/2 p50 = " << (oneWay.p50 - rttHalf.p50) << " ns\n"
                      << "  rdtsc back-to-back p50 " << overhead.rdtscP50Ns << " ns, lfence+rdtsc p50 " << overhead.sendStampP50Ns
                      << " ns, rdtscp p50 " << overhead.recvStampP50Ns << " ns" << std::endl;

            bool written = appendJsonLine(opt.results, j);
            if (!opt.floorFile.empty()) {
                written = appendJsonLine(opt.floorFile, j) && written;
            }
            return (ok && subOk && written) ? 0 : 1;
        }
    } // namespace

    int runFloor(Options const &opt) {
        if (opt.role == "sub") {
            return runFloorSubscriber(opt);
        }
        return runFloorPublisher(opt);
    }

} // namespace ipc_latency_profiler
