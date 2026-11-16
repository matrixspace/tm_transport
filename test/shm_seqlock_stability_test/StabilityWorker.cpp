// Long-running shm_seqlock stability worker.
//
// The writer self-drives at a configured rate and creates messages with a
// generation id, sequence, send timestamp, size and checksum. The reader
// verifies integrity and sequence, tracks gaps/duplicates/order, and can sleep
// in its callback to emulate a slow reader. Python owns process restarts.
#include <tm_kit/transport/shm_seqlock/ShmSeqlockComponent.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace dev::cd606::tm;

namespace
{
    constexpr std::uint32_t MAGIC = 0x53544142U; // "STAB"
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
    static_assert(sizeof(Header) == 40, "unexpected stability header size");

    std::atomic<bool> stopFlag {false};
    std::mutex outputMutex;

    void emit(std::string const &s)
    {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::cout << s << std::endl;
    }
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
    // Bounded log-linear histogram: 64 mantissa bins per power of two, so the
    // reported quantile is a bucket upper bound with <= ~1.6% relative error.
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

    struct ReaderState {
        std::mutex mutex;
        bool haveGeneration = false;
        std::uint64_t generation = 0;
        std::uint64_t expectedSeq = 0;
        std::uint64_t runs = 0;
        std::uint64_t received = 0, lost = 0, duplicate = 0, outOfOrder = 0, corrupt = 0, badSequence = 0;
        std::uint64_t maxLatencyNs = 0;
        Histogram latency;
    };

    void signalHandler(int)
    {
        stopFlag.store(true);
    }
    std::uint64_t randomSeed(std::string const &s, int fallback)
    {
        try {
            return static_cast<std::uint64_t>(std::stoull(s));
        } catch (...) {
            return static_cast<std::uint64_t>(fallback);
        }
    }
    std::size_t chooseFillSize(std::string const &mode, std::mt19937_64 &rng)
    {
        if (mode.compare(0, 6, "fixed:") == 0)
            return static_cast<std::size_t>(std::stoull(mode.substr(6)));
        auto first = mode.find(':', 7);
        auto lo = static_cast<std::size_t>(std::stoull(mode.substr(7, first - 7)));
        auto hi = static_cast<std::size_t>(std::stoull(mode.substr(first + 1)));
        if (hi < lo)
            std::swap(lo, hi);
        return lo + static_cast<std::size_t>(rng() % (hi - lo + 1));
    }
} // namespace

int main(int argc, char **argv)
{
    try {
        if (argc < 8)
            throw std::runtime_error(
                "usage: worker writer|reader segment properties rate payload-mode seed slowUs [durationMs [warmupMs]]");
        auto role = std::string(argv[1]);
        auto locator = transport::ConnectionLocator::parse(std::string("::::") + argv[2] + "[" + argv[3] + "]");
        auto rate = std::stoll(argv[4]);
        auto payloadMode = std::string(argv[5]);
        auto seed = randomSeed(argv[6], 1);
        auto slowUs = static_cast<long>(std::stol(argv[7]));
        auto durationMs = argc > 8 ? std::stoll(argv[8]) : 0;
        auto warmupMs = argc > 9 ? std::stoll(argv[9]) : 0;
        std::signal(SIGTERM, signalHandler);
        std::signal(SIGINT, signalHandler);
        transport::shm_seqlock::ShmSeqlockComponent component;
        std::mt19937_64 rng(seed);
        // Every writer process must have a distinct generation even when the
        // orchestrator restarts it with the same seed argument.
        std::random_device randomDevice;
        std::uint64_t generation =
            ((static_cast<std::uint64_t>(randomDevice()) << 32) ^ randomDevice() ^ nowNs()) | 1ULL;
        std::atomic<std::uint64_t> sent {0};
        std::atomic<std::uint64_t> lateMaxUs {0};

        if (role == "writer") {
            auto pub = component.shm_seqlock_getPublisher(locator);
            Histogram publishCost;
            emit("READY");
            auto intervalNs = rate > 0 ? (1000000000LL / rate) : 0;
            auto nextSend = nowNs();
            auto begin = nextSend;
            auto lastStat = begin;
            std::uint64_t seq = 0;
            bool warmupDone = warmupMs <= 0;
            std::uint64_t baseSent = 0, baseRejected = 0;
            while (!stopFlag.load(std::memory_order_relaxed)) {
                auto now = nowNs();
                if (!warmupDone && now - begin >= static_cast<std::uint64_t>(warmupMs) * 1000000ULL) {
                    baseSent = sent.load(std::memory_order_relaxed);
                    baseRejected = 0;
                    for (auto const &item : component.shm_seqlock_publisherStats())
                        baseRejected += item.second.rejected;
                    lateMaxUs.store(0, std::memory_order_relaxed);
                    publishCost.reset();
                    warmupDone = true;
                }
                if (durationMs > 0 && now - begin >= static_cast<std::uint64_t>(durationMs) * 1000000ULL)
                    break;
                if (intervalNs > 0 && now < nextSend) {
                    auto wait = nextSend - now;
                    if (wait > 200000ULL) {
                        // sleep_for is far too coarse for intervals below a few
                        // hundred microseconds (it overshoots by tens of us).
                        std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<std::uint64_t>(wait, 100000000ULL)));
                    } else {
                        while (nowNs() < nextSend && !stopFlag.load(std::memory_order_relaxed))
                            std::this_thread::yield();
                    }
                    continue;
                }
                auto fill = chooseFillSize(payloadMode, rng);
                basic::ByteDataWithTopic message;
                message.topic = "s";
                message.content = buildMessage(generation, seq, static_cast<std::uint32_t>(fill));
                auto t0 = nowNs();
                pub(std::move(message));
                publishCost.add(nowNs() - t0);
                ++seq;
                sent.fetch_add(1, std::memory_order_relaxed);
                if (intervalNs > 0) {
                    nextSend += intervalNs;
                    auto current = nowNs();
                    if (current > nextSend + static_cast<std::uint64_t>(intervalNs)) {
                        auto lateUs = (current - nextSend) / 1000;
                        auto seen = lateMaxUs.load(std::memory_order_relaxed);
                        while (lateUs > seen && !lateMaxUs.compare_exchange_weak(seen, lateUs)) {
                        }
                        nextSend = current;
                    }
                }
                if (now - lastStat >= 1000000000ULL) {
                    lastStat = now;
                    std::uint64_t rejected = 0;
                    for (auto const &item : component.shm_seqlock_publisherStats())
                        rejected += item.second.rejected;
                    emit(
                        "STAT W gen=" + std::to_string(generation) + " sent=" + std::to_string(sent.load() - baseSent)
                        + " rejected=" + std::to_string(rejected - baseRejected) + " lateMaxUs=" + std::to_string(lateMaxUs.load()));
                }
            }
            {
                std::uint64_t rejected = 0;
                for (auto const &item : component.shm_seqlock_publisherStats())
                    rejected += item.second.rejected;
                emit(
                    "STAT W gen=" + std::to_string(generation) + " sent=" + std::to_string(sent.load() - baseSent)
                    + " rejected=" + std::to_string(rejected - baseRejected) + " lateMaxUs=" + std::to_string(lateMaxUs.load())
                    + " final=1 pubP50ns=" + std::to_string(publishCost.percentile(0.50))
                    + " pubP90ns=" + std::to_string(publishCost.percentile(0.90))
                    + " pubP99ns=" + std::to_string(publishCost.percentile(0.99)));
            }
            return 0;
        }

        if (role != "reader")
            throw std::runtime_error("invalid role " + role);

        auto state = std::make_shared<ReaderState>();
        auto id = component.shm_seqlock_addSubscriptionClient(
            locator
            , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection {}
            , [state, slowUs](basic::ByteDataWithTopic &&data) {
                Header h {};
                if (!parseMessage(data.content, h)) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    ++state->corrupt;
                } else {
                    auto now = nowNs();
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (!state->haveGeneration || h.generation != state->generation) {
                        state->haveGeneration = true;
                        state->generation = h.generation;
                        state->expectedSeq = h.seq + 1;
                        ++state->runs;
                    } else if (h.seq == state->expectedSeq) {
                        state->expectedSeq = h.seq + 1;
                    } else if (h.seq > state->expectedSeq) {
                        state->lost += h.seq - state->expectedSeq;
                        state->expectedSeq = h.seq + 1;
                    } else if (h.seq + 1 == state->expectedSeq) {
                        ++state->duplicate;
                    } else {
                        ++state->outOfOrder;
                    }
                    ++state->received;
                    if (now > h.sendNs) {
                        auto latency = now - h.sendNs;
                        if (latency > 60000000000ULL) {
                            ++state->badSequence;
                        } else {
                            if (latency > state->maxLatencyNs)
                                state->maxLatencyNs = latency;
                            state->latency.add(latency);
                        }
                    }
                }
                if (slowUs > 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(slowUs));
            }
            , std::nullopt);
        emit("READY");
        auto begin = nowNs();
        auto lastStat = begin;
        bool warmupDone = warmupMs <= 0;
        std::uint64_t baseReceived = 0, baseLost = 0, baseDup = 0, baseOoo = 0, baseCorrupt = 0, baseBad = 0;
        std::uint64_t baseLap = 0, baseReopens = 0, baseRestarts = 0;
        auto report = [&](bool finalFlag) {
            std::uint64_t lap = 0, reopens = 0, restarts = 0;
            for (auto const &item : component.shm_seqlock_readerStats()) {
                lap += item.second.lapResyncs;
                reopens += item.second.reopens;
                restarts += item.second.publisherRestarts;
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            emit(
                "STAT R gen=" + std::to_string(state->generation) + " runs=" + std::to_string(state->runs)
                + " received=" + std::to_string(state->received - baseReceived)
                + " lost=" + std::to_string(state->lost - baseLost)
                + " dup=" + std::to_string(state->duplicate - baseDup)
                + " ooo=" + std::to_string(state->outOfOrder - baseOoo)
                + " corrupt=" + std::to_string(state->corrupt - baseCorrupt)
                + " bad=" + std::to_string(state->badSequence - baseBad)
                + " lap=" + std::to_string(lap - baseLap) + " reopens=" + std::to_string(reopens - baseReopens)
                + " publisherRestarts=" + std::to_string(restarts - baseRestarts)
                + " p50ns=" + std::to_string(state->latency.percentile(0.50))
                + " p90ns=" + std::to_string(state->latency.percentile(0.90))
                + " p99ns=" + std::to_string(state->latency.percentile(0.99))
                + " maxns=" + std::to_string(state->maxLatencyNs)
                + (finalFlag ? " final=1" : ""));
        };
        while (!stopFlag.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto now = nowNs();
            if (!warmupDone && now - begin >= static_cast<std::uint64_t>(warmupMs) * 1000000ULL) {
                std::lock_guard<std::mutex> lock(state->mutex);
                baseReceived = state->received;
                baseLost = state->lost;
                baseDup = state->duplicate;
                baseOoo = state->outOfOrder;
                baseCorrupt = state->corrupt;
                baseBad = state->badSequence;
                state->latency.reset();
                state->maxLatencyNs = 0;
                for (auto const &item : component.shm_seqlock_readerStats()) {
                    baseLap += item.second.lapResyncs;
                    baseReopens += item.second.reopens;
                    baseRestarts += item.second.publisherRestarts;
                }
                warmupDone = true;
            }
            if (durationMs > 0 && now - begin >= static_cast<std::uint64_t>(durationMs) * 1000000ULL)
                break;
            if (now - lastStat >= 1000000000ULL) {
                lastStat = now;
                report(false);
            }
        }
        component.shm_seqlock_removeSubscriptionClient(id);
        report(true);
        return 0;
    } catch (std::exception const &e) {
        emit(std::string("ERROR ") + e.what());
        return 1;
    }
}
