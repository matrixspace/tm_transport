// No test probes: these rounds exercise the production component path.
#ifdef TM_KIT_SHM_SEQLOCK_TEST_PROBES
#error Resource and performance rounds must not enable fault probes
#endif
#include <tm_kit/transport/shm_seqlock/ShmSeqlockComponent.hpp>
#include "../ipc_concurrency_test/ConcurrencyCommon.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

using namespace dev::cd606::tm;
using Component = transport::shm_seqlock::ShmSeqlockComponent;
using ipc_concurrency::nowNs;
using ipc_concurrency::opt;
using ipc_concurrency::Options;
using ipc_concurrency::optNum;
namespace
{
    struct Segment {
        std::string name;
        bool owned = false;
        ~Segment()
        {
            if (!owned)
                return;
            ::shm_unlink(("/" + name).c_str());
            ::unlink(("/dev/shm/" + name + ".lock").c_str());
        }
    };
    void require(bool b, std::string const &s)
    {
        if (!b)
            throw std::runtime_error(s);
    }
    std::size_t directoryCount(char const *path)
    {
        auto *d = ::opendir(path);
        if (!d)
            throw std::runtime_error(std::string("cannot inspect ") + path);
        std::size_t n = 0;
        while (auto *p = ::readdir(d))
            if (p->d_name[0] != '.')
                ++n;
        ::closedir(d);
        return n;
    }
    uint64_t memoryField(char const *file, std::string const &key)
    {
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) {
            if (line.compare(0, key.size(), key) == 0) {
                std::istringstream s(line.substr(key.size()));
                uint64_t n = 0;
                s >> n;
                return n;
            }
        }
        return 0;
    }
    struct Resource {
        std::size_t fd, threads, maps = 0, segmentMaps = 0;
        uint64_t rss, pss;
        struct rusage usage;
        Resource(std::string const &segment)
            : fd(directoryCount("/proc/self/fd"))
            , threads(directoryCount("/proc/self/task"))
            , rss(memoryField("/proc/self/status", "VmRSS:"))
            , pss(memoryField("/proc/self/smaps_rollup", "Pss:"))
        {
            std::ifstream in("/proc/self/maps");
            for (std::string line; std::getline(in, line);) {
                ++maps;
                if (line.find("/dev/shm/" + segment) != std::string::npos)
                    ++segmentMaps;
            }
            ::getrusage(RUSAGE_SELF, &usage);
        }
        double cpu() const
        {
            return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
        }
        std::string json() const
        {
            std::ostringstream s;
            s << "{\"rss_kib\":" << rss << ",\"pss_kib\":" << pss << ",\"fd\":" << fd << ",\"threads\":" << threads << ",\"maps\":" << maps << ",\"segment_maps\":" << segmentMaps
              << ",\"cpu_seconds\":" << cpu() << ",\"minor_faults\":" << usage.ru_minflt << ",\"major_faults\":" << usage.ru_majflt << ",\"voluntary_switches\":" << usage.ru_nvcsw
              << ",\"involuntary_switches\":" << usage.ru_nivcsw << '}';
            return s.str();
        }
    };
    // 64 mantissa bins per exponent, all observations retained, no sampling bias.
    struct Histogram {
        std::array<uint64_t, 4096> bins{};
        uint64_t count = 0, max = 0;
        long double sum = 0;
        void add(uint64_t ns)
        {
            unsigned e = ns ? 63 - __builtin_clzll(ns) : 0;
            uint64_t base = uint64_t(1) << e;
            unsigned mantissa = ns > base ? static_cast<unsigned>((static_cast<__uint128_t>(ns - base) * 64) / base) : 0;
            ++bins[e * 64 + mantissa];
            ++count;
            sum += ns;
            max = std::max(max, ns);
        }
        uint64_t quantile(double q) const
        {
            if (!count)
                return 0;
            uint64_t want = std::max<uint64_t>(1, static_cast<uint64_t>(count * q + 0.999999)), n = 0;
            for (unsigned i = 0; i < bins.size(); ++i)
                if ((n += bins[i]) >= want) {
                    unsigned e = i / 64, m = i % 64;
                    auto upper = ((static_cast<__uint128_t>(uint64_t(1) << e) * (65 + m)) + 63) / 64;
                    return std::min(max, static_cast<uint64_t>(upper));
                }
            return max;
        }
        std::string json() const
        {
            std::ostringstream s;
            s << "{\"samples\":" << count << ",\"mean_ns\":" << (count ? static_cast<double>(sum / count) : 0) << ",\"p50_ns\":" << quantile(.5) << ",\"p90_ns\":" << quantile(.9)
              << ",\"p99_ns\":" << quantile(.99);
            if (count >= 1000000)
                s << ",\"p999_ns\":" << quantile(.999);
            s << ",\"max_ns\":" << max << '}';
            return s.str();
        }
    };
    struct Message {
        uint64_t run, sequence, sendNs, scheduledNs;
        uint32_t length, phase, checksum, reserved;
    };
    static_assert(sizeof(Message) == 48, "unexpected message layout");
    uint32_t checksum(Message const &m, std::string const &s)
    {
        auto c = ipc_concurrency::fnv1a(2166136261U, &m, offsetof(Message, checksum));
        return ipc_concurrency::fnv1a(c, s.data() + sizeof(Message), s.size() - sizeof(Message));
    }
    std::string message(uint64_t run, uint64_t seq, uint32_t phase, std::size_t size, int64_t scheduled, bool verify)
    {
        std::string s(size, '\0');
        if (verify)
            for (std::size_t i = sizeof(Message); i < size; ++i)
                s[i] = static_cast<char>((run * 13 + seq * 31 + i * 7) & 255);
        Message m{run, seq, static_cast<uint64_t>(nowNs()), static_cast<uint64_t>(scheduled), static_cast<uint32_t>(size), phase, 0, 0};
        if (verify)
            m.checksum = checksum(m, s);
        std::memcpy(s.data(), &m, sizeof(m));
        return s;
    }
    bool decode(std::string const &s, Message &m, bool verify)
    {
        if (s.size() < sizeof(m))
            return false;
        std::memcpy(&m, s.data(), sizeof(m));
        if (m.length != s.size() || m.reserved || m.phase > 2)
            return false;
        if (verify) {
            if (m.checksum != checksum(m, s))
                return false;
            for (std::size_t i = sizeof(Message); i < s.size(); ++i)
                if (static_cast<unsigned char>(s[i]) != ((m.run * 13 + m.sequence * 31 + i * 7) & 255))
                    return false;
        }
        return true;
    }
    transport::ConnectionLocator locator(Options const &o, std::string const &name, bool small = false)
    {
        auto size = optNum(o, "payload", 256) + 2;
        auto slots = optNum(o, "slotCount", small ? 2 : 4096);
        require(size >= sizeof(Message) + 2 && size <= 1048578, "payload must be 48..1048576 bytes");
        require(slots >= 2 && slots <= 65536 && slots * (size + 256) <= 64 * 1024 * 1024, "requested segment exceeds resource budget");
        return transport::ConnectionLocator::parse(
            "::::" + name + "[slotSize=" + std::to_string(size) + ",slotCount=" + std::to_string(slots) + ",mlock=" + opt(o, "mlock", "false") + ",writerLock=" + opt(o, "writerLock", "false") +
            ",layout=" + opt(o, "layout", "inline") + ",read=" + opt(o, "read", "classic") + ",wait=" + opt(o, "wait", "spin") + ",stride128=" + opt(o, "stride128", "false") +
            ",spinCount=" + opt(o, "spinCount", "20000") + ",futexTimeoutUs=10000,join=latest]");
    }
    struct Reader {
        std::mutex mutex;
        std::condition_variable cv;
        bool ready = false, ended = false;
        uint64_t received = 0, bad = 0, order = 0, first = 0, last = 0, gaps = 0;
        Histogram latency, freshness;
        void on(basic::ByteDataWithTopic const &d, uint64_t run, bool verify)
        {
            auto receivedNs = nowNs();
            std::lock_guard<std::mutex> lock(mutex);
            Message m;
            if (d.topic != "w0" || !decode(d.content, m, verify) || m.run != run || m.sendNs > static_cast<uint64_t>(receivedNs) || m.scheduledNs > m.sendNs) {
                ++bad;
                cv.notify_all();
                return;
            }
            ready = true;
            if (m.phase == 2)
                ended = true;
            if (m.phase == 1) {
                if (ended)
                    ++order;
                if (received) {
                    if (m.sequence <= last)
                        ++order;
                    else
                        gaps += m.sequence - last - 1;
                } else
                    first = m.sequence;
                last = m.sequence;
                ++received;
                latency.add(receivedNs - m.sendNs);
                freshness.add(receivedNs - m.scheduledNs);
            }
            cv.notify_all();
        }
    };
    void waitUntil(int64_t target)
    {
        auto left = target - nowNs();
        if (left > 100000)
            std::this_thread::sleep_for(std::chrono::nanoseconds(left - 50000));
        while (nowNs() < target) {
        }
    }
    int perf(Options const &o, std::string const &name)
    {
        auto readers = optNum(o, "readers", 1), size = optNum(o, "payload", 256), rate = optNum(o, "rate", 10000);
        auto warmupMs = optNum(o, "warmupMs", 5000), durationMs = optNum(o, "durationMs", 30000);
        bool verify = opt(o, "verify", "false") == "true";
        bool progress = opt(o, "progress", "false") == "true";
        require(readers <= 8 && durationMs > 0 && durationMs <= 86400000 && rate <= 1000000000, "invalid readers/duration/rate");
        auto loc = locator(o, name);
        Segment segment{name};
        Component writer;
        auto publish = writer.shm_seqlock_getPublisher(loc);
        segment.owned = true;
        std::vector<std::unique_ptr<Reader>> states;
        std::vector<std::unique_ptr<Component>> components;
        uint64_t run = nowNs(), seq = 0;
        for (unsigned i = 0; i < readers; ++i) {
            states.push_back(std::make_unique<Reader>());
            components.push_back(std::make_unique<Component>());
            auto *s = states.back().get();
            components.back()->shm_seqlock_addSubscriptionClient(loc, Component::NoTopicSelection{}, [s, run, verify](basic::ByteDataWithTopic &&d) { s->on(d, run, verify); });
        }
        auto allReady = [&] {
            for (auto const &s : states) {
                std::lock_guard<std::mutex> lock(s->mutex);
                if (!s->ready)
                    return false;
            }
            return true;
        };
        auto joinDeadline = nowNs() + 5000000000LL;
        while (!allReady()) {
            require(nowNs() < joinDeadline, "reader readiness timeout");
            publish({"w0", message(run, seq++, 0, size, nowNs(), verify)});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto runPhase = [&](uint32_t phase, uint64_t ms, Histogram *cost) {
            auto start = nowNs(), end = start + static_cast<int64_t>(ms) * 1000000;
            uint64_t count = 0;
            auto nextProgress = start + 60000000000LL;
            for (;;) {
                auto scheduled = rate ? start + static_cast<int64_t>((static_cast<__uint128_t>(count) * 1000000000) / rate) : nowNs();
                if (scheduled >= end || nowNs() >= end)
                    break;
                if (rate)
                    waitUntil(scheduled);
                auto s = message(run, seq++, phase, size, scheduled, verify);
                auto t = nowNs();
                publish({"w0", std::move(s)});
                if (cost)
                    cost->add(nowNs() - t);
                ++count;
                if (progress && phase == 1 && nowNs() >= nextProgress) {
                    std::cout << "{\"kind\":\"progress\",\"offered\":" << count << ",\"resource\":" << Resource(name).json() << '}' << std::endl;
                    nextProgress += 60000000000LL;
                }
            }
            // Include the last scheduled interval in elapsed throughput/CPU.
            if (nowNs() < end)
                waitUntil(end);
            return count;
        };
        runPhase(0, warmupMs, nullptr);
        auto beginSeq = seq;
        Resource before(name);
        Histogram publishCost;
        auto t0 = nowNs();
        auto offered = runPhase(1, durationMs, &publishCost);
        auto elapsed = nowNs() - t0;
        auto endSeq = seq;
        Resource after(name);
        auto drainDeadline = nowNs() + 5000000000LL;
        for (;;) {
            bool allEnded = true;
            for (auto const &s : states) {
                std::lock_guard<std::mutex> lock(s->mutex);
                if (!s->ended)
                    allEnded = false;
            }
            if (allEnded)
                break;
            require(nowNs() < drainDeadline, "reader drain timeout");
            publish({"w0", message(run, seq++, 2, size, nowNs(), verify)});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bool clean = true;
        std::cout << "{\"kind\":\"perf\",\"histogram_relative_error_max\":0.016,\"verified_body\":" << (verify ? "true" : "false") << ",\"offered\":" << offered << ",\"elapsed_ns\":" << elapsed
                  << ",\"offered_per_second\":" << offered * 1e9 / elapsed << ",\"process_cpu_percent\":" << (after.cpu() - before.cpu()) * 1e11 / elapsed << ",\"publish\":" << publishCost.json()
                  << ",\"resource_before\":" << before.json() << ",\"resource_after\":" << after.json() << ",\"readers\":[";
        for (unsigned i = 0; i < readers; ++i) {
            auto &s = *states[i];
            std::lock_guard<std::mutex> lock(s.mutex);
            auto lost = offered - s.received;
            auto accounted = s.received ? s.gaps + s.first - beginSeq + (endSeq - 1 - s.last) : offered;
            clean = clean && s.bad == 0 && s.order == 0 && s.received <= offered && accounted == lost && (offered == 0 || s.received > 0);
            auto rs = components[i]->shm_seqlock_readerStats();
            uint64_t transportLost = 0, corrupt = 0;
            for (auto const &p : rs) {
                transportLost += p.second.lost;
                corrupt += p.second.corrupt;
            }
            clean = clean && corrupt == 0;
            if (i)
                std::cout << ',';
            std::cout << "{\"received\":" << s.received << ",\"lost\":" << lost << ",\"loss_rate\":" << (offered ? static_cast<double>(lost) / offered : 0)
                      << ",\"received_per_second\":" << s.received * 1e9 / elapsed << ",\"bad\":" << s.bad << ",\"order_errors\":" << s.order << ",\"gaps\":" << s.gaps
                      << ",\"transport_lost_including_warmup\":" << transportLost << ",\"transport_corrupt\":" << corrupt << ",\"latency\":" << s.latency.json()
                      << ",\"freshness\":" << s.freshness.json() << '}';
        }
        auto ps = writer.shm_seqlock_publisherStats();
        uint64_t rejected = 0;
        for (auto const &p : ps)
            rejected += p.second.rejected;
        clean = clean && rejected == 0;
        std::cout << "],\"accepted\":" << offered << ",\"rejected\":" << rejected << ",\"status\":\"" << (clean ? "PASS" : "FAIL") << "\"}" << std::endl;
        // State is declared before components, so callback owners survive joins.
        return clean ? 0 : 1;
    }
    int churn(Options const &o, std::string const &name)
    {
        auto cycles = optNum(o, "cycles", 10000), sampleEvery = optNum(o, "sampleEvery", std::max<uint64_t>(1, cycles / 10));
        require(cycles > 0 && cycles <= 100000 && sampleEvery > 0, "invalid churn count");
        bool self = opt(o, "selfRemove", "false") == "true";
        auto loc = locator(o, name, true);
        Segment segment{name};
        Component writer;
        auto publish = writer.shm_seqlock_getPublisher(loc);
        segment.owned = true;
        Resource baseline(name);
        Resource live = baseline;
        std::cout << "{\"kind\":\"resource\",\"stage\":\"baseline\",\"cycle\":0,\"resource\":" << baseline.json() << '}' << std::endl;
        uint64_t callbacks = 0;
        {
            Component reader;
            for (uint64_t i = 0; i < cycles; ++i) {
                struct State {
                    std::mutex mutex;
                    std::condition_variable cv;
                    std::atomic<uint32_t> id{0};
                    std::atomic<bool> ready{false};
                    bool done = false;
                    unsigned calls = 0;
                    bool bad = false;
                };
                auto s = std::make_shared<State>();
                auto expected = std::to_string(i);
                auto id = reader.shm_seqlock_addSubscriptionClient(loc, Component::NoTopicSelection{}, [s, &reader, self, expected](basic::ByteDataWithTopic &&d) {
                    if (!s->ready.load(std::memory_order_acquire) || d.content != expected)
                        return;
                    if (self)
                        reader.shm_seqlock_removeSubscriptionClient(s->id.load(std::memory_order_relaxed));
                    std::lock_guard<std::mutex> lock(s->mutex);
                    ++s->calls;
                    s->bad = s->bad || d.topic != "w0";
                    s->done = true;
                    s->cv.notify_all();
                });
                s->id.store(id, std::memory_order_relaxed);
                s->ready.store(true, std::memory_order_release);
                auto deadline = nowNs() + 5000000000LL;
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lock(s->mutex);
                        if (s->done)
                            break;
                    }
                    require(nowNs() < deadline, "churn callback timeout at cycle " + std::to_string(i));
                    publish({"w0", expected});
                    std::unique_lock<std::mutex> lock(s->mutex);
                    s->cv.wait_for(lock, std::chrono::milliseconds(1), [&] { return s->done; });
                }
                if (!self)
                    reader.shm_seqlock_removeSubscriptionClient(id);
                reader.shm_seqlock_removeSubscriptionClient(id); // idempotence
                reader.shm_seqlock_removeSubscriptionClient(0xffffffffU);
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    require(!s->bad && s->calls > 0, "churn callback invalid");
                    if (self)
                        require(s->calls == 1, "self-remove delivered twice");
                    callbacks += s->calls;
                }
                require(reader.shm_seqlock_threadHandles().empty(), "reader handle retained after last removal");
                if ((i + 1) % sampleEvery == 0 || i + 1 == cycles) {
                    live = Resource(name);
                    std::cout << "{\"kind\":\"resource\",\"stage\":\"component_alive\",\"cycle\":" << i + 1 << ",\"resource\":" << live.json() << '}' << std::endl;
                }
            }
        }
        Resource destroyed(name);
        bool released = destroyed.segmentMaps == baseline.segmentMaps && destroyed.fd == baseline.fd;
        bool retention = live.segmentMaps > baseline.segmentMaps;
        std::cout << "{\"kind\":\"churn\",\"cycles\":" << cycles << ",\"self_remove\":" << (self ? "true" : "false") << ",\"callbacks\":" << callbacks
                  << ",\"retained_mappings_while_alive\":" << (retention ? "true" : "false") << ",\"destruction_released_mappings_and_fd\":" << (released ? "true" : "false")
                  << ",\"baseline\":" << baseline.json() << ",\"component_alive\":" << live.json() << ",\"after_destructor\":" << destroyed.json() << ",\"status\":\""
                  << (released && !retention ? "PASS" : "FAIL") << "\"}" << std::endl;
        return released && !retention ? 0 : 1;
    }
    int oracle()
    {
        auto s = message(42, 7, 1, 128, nowNs(), true);
        Message m;
        require(decode(s, m, true), "oracle rejected valid message");
        uint64_t detected = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            auto bad = s;
            bad[i] ^= 1;
            if (!decode(bad, m, true))
                ++detected;
        }
        require(detected == s.size(), "oracle failed deterministic byte corruption");
        Histogram h;
        for (uint64_t i = 1; i <= 1000000; ++i)
            h.add(i);
        require(h.quantile(.5) >= 500000 && h.quantile(.5) < 508000, "histogram p50 outside error bound");
        require(h.quantile(.999) >= 999000, "histogram underestimated quantile");
        std::cout << "{\"kind\":\"oracle\",\"corruptions_detected\":" << detected << ",\"status\":\"PASS\"}" << std::endl;
        return 0;
    }
} // namespace
int main(int argc, char **argv)
{
    try {
        auto o = ipc_concurrency::parseOptions(argc, argv);
        auto mode = opt(o, "mode", "perf");
        auto name = opt(o, "segment", "shmsys_res_" + std::to_string(::getpid()) + "_" + std::to_string(nowNs()));
        require(!name.empty() && name.find('/') == std::string::npos, "invalid test segment");
        if (mode == "environment") {
            std::cout << "{\"kind\":\"environment\",\"build_type\":\"" << TM_SHM_RESOURCE_BUILD_TYPE << "\",\"compiler\":\"" << __VERSION__ << "\",\"fault_probes\":false}" << std::endl;
            return 0;
        }
        if (mode == "oracle")
            return oracle();
        if (mode == "perf")
            return perf(o, name);
        if (mode == "churn")
            return churn(o, name);
        throw std::runtime_error("unknown mode");
    } catch (std::exception const &e) {
        std::cerr << "ERROR " << e.what() << std::endl;
        return 1;
    }
}
