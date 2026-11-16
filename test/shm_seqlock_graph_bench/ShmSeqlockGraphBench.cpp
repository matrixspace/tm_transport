// Graph/MultiTransport performance benchmark for shm_seqlock.
//
// Builds a RealTimeApp graph: trigger importer -> publisher -> shared memory
// -> importer/listener -> sink. The sender paces messages at a target rate;
// every sink verifies sequence and checksum and records send->callback latency.
// Warmup messages are excluded by a start sequence number.
#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/basic/IntIDComponent.hpp>
#include <tm_kit/basic/TrivialBoostLoggingComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>
#include <tm_kit/transport/shm_seqlock/ShmSeqlockImporterExporter.hpp>
#include <tm_kit/transport/MultiTransportBroadcastListenerManagingUtils.hpp>
#include <tm_kit/transport/MultiTransportBroadcastPublisherManagingUtils.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace dev::cd606::tm;

namespace
{
    using Component = transport::shm_seqlock::ShmSeqlockComponent;
    using Bytes = basic::ByteDataWithTopic;
    using Env = infra::Environment<
        infra::CheckTimeComponent<false>,
        infra::TrivialExitControlComponent,
        basic::IntIDComponent<>,
        basic::TrivialBoostLoggingComponent,
        basic::real_time_clock::ClockComponent,
        Component>;
    using M = infra::RealTimeApp<Env>;
    using R = infra::AppRunner<M>;
    using Factory = transport::shm_seqlock::ShmSeqlockImporterExporter<Env>;

    constexpr std::uint32_t MAGIC = 0x47524150U; // "GRAP"
    constexpr std::uint16_t VERSION = 1;

    struct Header {
        std::uint32_t magic;
        std::uint16_t version;
        std::uint16_t headerSize;
        std::uint64_t generation;
        std::uint64_t seq;
        std::uint64_t sendNs;
        std::uint32_t payloadSize;
        std::uint32_t crc;
    };
    static_assert(sizeof(Header) == 40, "unexpected graph bench header size");

    std::uint64_t nowNs()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
    std::uint32_t fnv1a(char const *p, std::size_t n)
    {
        std::uint32_t h = 2166136261U;
        for (std::size_t i = 0; i < n; ++i) {
            h ^= static_cast<unsigned char>(p[i]);
            h *= 16777619U;
        }
        return h;
    }
    char fillByte(std::uint64_t generation, std::uint64_t seq, std::size_t i)
    {
        std::uint64_t x = generation * 0x9E3779B97F4A7C15ULL ^ (seq + i) * 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        return static_cast<char>(x >> 56);
    }
    std::string buildMessage(std::uint64_t generation, std::uint64_t seq, std::uint32_t fillSize)
    {
        Header h {};
        h.magic = MAGIC;
        h.version = VERSION;
        h.headerSize = sizeof(Header);
        h.generation = generation;
        h.seq = seq;
        h.sendNs = nowNs();
        h.payloadSize = fillSize;
        std::string body(sizeof(Header) + fillSize, '\0');
        for (std::size_t i = 0; i < fillSize; ++i)
            body[sizeof(Header) + i] = fillByte(generation, seq, i);
        h.crc = fnv1a(body.data() + sizeof(Header), fillSize);
        std::memcpy(body.data(), &h, sizeof(h));
        return body;
    }
    bool parseMessage(std::string const &body, Header &h)
    {
        if (body.size() < sizeof(Header))
            return false;
        std::memcpy(&h, body.data(), sizeof(h));
        if (h.magic != MAGIC || h.version != VERSION || h.headerSize != sizeof(Header))
            return false;
        if (h.payloadSize != body.size() - sizeof(Header))
            return false;
        return fnv1a(body.data() + sizeof(Header), h.payloadSize) == h.crc;
    }

    // Log-linear histogram: 64 mantissa bins per power of two.
    struct Histogram {
        std::array<std::uint64_t, 4096> bins {};
        std::uint64_t count = 0, max = 0;
        void add(std::uint64_t ns)
        {
            unsigned e = ns ? 63 - __builtin_clzll(ns) : 0;
            std::uint64_t base = std::uint64_t(1) << e;
            unsigned mantissa = ns > base ? static_cast<unsigned>((static_cast<__uint128_t>(ns - base) * 64) / base) : 0;
            ++bins[e * 64 + mantissa];
            ++count;
            max = std::max(max, ns);
        }
        std::uint64_t percentile(double q) const
        {
            if (!count)
                return 0;
            std::uint64_t want = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(count * q + 0.999999)), seen = 0;
            for (unsigned i = 0; i < bins.size(); ++i)
                if ((seen += bins[i]) >= want) {
                    unsigned e = i / 64, m = i % 64;
                    auto upper = ((static_cast<__uint128_t>(std::uint64_t(1) << e) * (65 + m)) + 63) / 64;
                    return std::min(max, static_cast<std::uint64_t>(upper));
                }
            return max;
        }
        void reset()
        {
            bins.fill(0);
            count = 0;
            max = 0;
        }
    };

    struct SinkState {
        std::mutex mutex;
        std::atomic<bool> ready {false};
        std::atomic<bool> measuring {false};
        std::uint64_t lastSeq = 0;
        bool haveSeq = false;
        std::uint64_t startSeq = 1, expected = 1;
        std::uint64_t received = 0, lost = 0, dup = 0, ooo = 0, corrupt = 0, bad = 0;
        Histogram latency;

        void observe(Bytes const &d)
        {
            if (d.topic == "__ready") {
                ready.store(true, std::memory_order_release);
                return;
            }
            Header h {};
            bool ok = parseMessage(d.content, h);
            std::lock_guard<std::mutex> lock(mutex);
            if (!ok) {
                if (measuring.load(std::memory_order_acquire))
                    ++corrupt;
                return;
            }
            if (!haveSeq || h.seq > lastSeq)
                lastSeq = h.seq;
            haveSeq = true;
            if (!measuring.load(std::memory_order_acquire) || h.seq < startSeq)
                return;
            auto now = nowNs();
            if (h.seq == expected) {
                expected = h.seq + 1;
            } else if (h.seq > expected) {
                lost += h.seq - expected;
                expected = h.seq + 1;
            } else if (h.seq + 1 == expected) {
                ++dup;
            } else {
                ++ooo;
            }
            ++received;
            if (now > h.sendNs) {
                auto latency = now - h.sendNs;
                if (latency > 60000000000ULL)
                    ++bad;
                else
                    this->latency.add(latency);
            }
        }

        void start(std::uint64_t nextSeq)
        {
            std::lock_guard<std::mutex> lock(mutex);
            startSeq = nextSeq;
            expected = nextSeq;
            received = lost = dup = ooo = corrupt = bad = 0;
            latency.reset();
            measuring.store(true, std::memory_order_release);
        }
        void stop()
        {
            measuring.store(false, std::memory_order_release);
        }
        std::string json() const
        {
            std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(mutex));
            std::ostringstream s;
            s << "{\"received\":" << received << ",\"lost\":" << lost << ",\"dup\":" << dup
              << ",\"ooo\":" << ooo << ",\"corrupt\":" << corrupt << ",\"bad\":" << bad
              << ",\"p50_ns\":" << latency.percentile(0.50)
              << ",\"p90_ns\":" << latency.percentile(0.90)
              << ",\"p99_ns\":" << latency.percentile(0.99)
              << ",\"max_ns\":" << latency.max << '}';
            return s.str();
        }
    };

    void pace(std::uint64_t intervalNs, std::uint64_t &nextSend)
    {
        if (!intervalNs)
            return;
        auto now = nowNs();
        if (now < nextSend) {
            auto wait = nextSend - now;
            if (wait > 200000ULL) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<std::uint64_t>(wait, 100000000ULL)));
            } else {
                while (nowNs() < nextSend) {
                    std::this_thread::yield();
                }
            }
            now = nowNs();
        }
        nextSend += intervalNs;
        if (now > nextSend + intervalNs) {
            nextSend = now + intervalNs;
        }
    }
} // namespace

int main(int argc, char **argv)
{
    try {
        if (argc < 8) {
            std::cerr << "usage: graph-bench raw|multi segment listeners rate payload-bytes duration-ms "
                         "warmup-ms [layout] [read] [wait]\n";
            return 2;
        }
        std::string mode = argv[1];
        std::string segment = argv[2];
        auto listeners = static_cast<int>(std::stol(argv[3]));
        auto rate = std::stoll(argv[4]);
        auto payloadBytes = static_cast<std::uint32_t>(std::stoul(argv[5]));
        auto durationMs = std::stoll(argv[6]);
        auto warmupMs = std::stoll(argv[7]);
        std::string layout = argc > 8 ? argv[8] : "inline";
        std::string read = argc > 9 ? argv[9] : "classic";
        std::string wait = argc > 10 ? argv[10] : "spin";
        if (listeners < 1 || listeners > 8) {
            throw std::runtime_error("listeners must be 1..8");
        }
        if (payloadBytes < 1 || payloadBytes > 1000) {
            throw std::runtime_error("payload-bytes must be 1..1000");
        }
        auto loc = transport::ConnectionLocator::parse(
            "::::" + segment + "[slotCount=8192,slotSize=2048,mlock=false,layout=" + layout
            + ",read=" + read + ",wait=" + wait + ",spinCount=1,writerLock=false]");
        auto channel = std::string("shm_seqlock://") + loc.toSerializationFormat();

        auto env = std::make_unique<Env>();
        R runner(env.get());
        auto source = M::triggerImporter<Bytes, false>();
        auto send = std::get<1>(source);
        std::vector<std::shared_ptr<SinkState>> states;
        for (int i = 0; i < listeners; ++i)
            states.push_back(std::make_shared<SinkState>());
        for (int i = 0; i < listeners; ++i) {
            auto index = static_cast<std::size_t>(i);
            auto sink = M::simpleExporter<Bytes>(
                [state = states[index]](M::InnerData<Bytes> &&d) { state->observe(d.timedData.value); });
            if (mode == "multi") {
                auto in = transport::MultiTransportBroadcastListenerManagingUtils<R>::oneByteDataBroadcastListener(
                    runner, "listener" + std::to_string(i), channel, std::string("r/(__ready|b)/"));
                runner.exportItem("sink" + std::to_string(i), sink, std::move(in));
            } else {
                runner.exportItem("sink" + std::to_string(i), sink,
                    runner.importItem("listener" + std::to_string(i), Factory::createImporter(loc)));
            }
        }
        if (mode == "multi") {
            auto out = transport::MultiTransportBroadcastPublisherManagingUtils<R>::oneByteDataBroadcastPublisher(
                runner, "publisher", channel, std::nullopt, false);
            runner.connect(runner.importItem("trigger", std::get<0>(source)), out);
        } else {
            runner.exportItem("publisher", Factory::createExporter(loc),
                runner.importItem("trigger", std::get<0>(source)));
        }
        runner.finalize();

        std::random_device randomDevice;
        std::uint64_t generation = ((static_cast<std::uint64_t>(randomDevice()) << 32) ^ randomDevice() ^ nowNs()) | 1ULL;
        std::uint64_t intervalNs = rate > 0 ? (1000000000ULL / static_cast<std::uint64_t>(rate)) : 0;
        std::uint64_t measuredSent = 0;
        auto began = nowNs();
        std::thread sender([&] {
            auto allReady = [&] {
                for (auto const &state : states)
                    if (!state->ready.load(std::memory_order_acquire))
                        return false;
                return true;
            };
            auto deadline = nowNs() + 10000000000ULL;
            while (!allReady() && nowNs() < deadline) {
                send(Bytes {"__ready", "x"});
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!allReady()) {
                throw std::runtime_error("sinks did not become ready");
            }
            std::uint64_t seq = 1;
            auto nextSend = nowNs();
            auto warmupEnd = nextSend + static_cast<std::uint64_t>(warmupMs) * 1000000ULL;
            while (nowNs() < warmupEnd) {
                send(Bytes {"b", buildMessage(generation, seq++, payloadBytes)});
                pace(intervalNs, nextSend);
            }
            for (auto const &state : states)
                state->start(seq);
            auto measureEnd = nowNs() + static_cast<std::uint64_t>(durationMs) * 1000000ULL;
            while (nowNs() < measureEnd) {
                send(Bytes {"b", buildMessage(generation, seq++, payloadBytes)});
                ++measuredSent;
                pace(intervalNs, nextSend);
            }
            for (auto const &state : states)
                state->stop();
        });
        sender.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto elapsedSeconds = static_cast<double>(nowNs() - began) / 1e9;

        std::ostringstream sinks;
        sinks << '[';
        for (std::size_t i = 0; i < states.size(); ++i) {
            if (i)
                sinks << ',';
            sinks << states[i]->json();
        }
        sinks << ']';
        std::ostringstream out;
        out << "{\"kind\":\"graph-bench\",\"mode\":\"" << mode << "\",\"listeners\":" << listeners
            << ",\"rate\":" << rate << ",\"payload_bytes\":" << payloadBytes
            << ",\"offered\":" << measuredSent
            << ",\"elapsed_seconds\":" << elapsedSeconds
            << ",\"sinks\":" << sinks.str() << '}';
        env.reset();
        std::cout << out.str() << std::endl;
        return 0;
    } catch (std::exception const &e) {
        std::cerr << "ERROR " << e.what() << std::endl;
        return 1;
    }
}
