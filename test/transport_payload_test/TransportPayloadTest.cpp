// Functional correctness, not a latency benchmark. The sequence lives in the
// topic so even zero-byte payloads can be checked for loss, duplication and order.
// Every data message is sent exactly once; only readiness probes are retried.
#include <tm_kit/transport/shm_seqlock/ShmSeqlockComponent.hpp>
#include <tm_kit/transport/shm_cursor/ShmCursorComponent.hpp>
#include <tm_kit/transport/multicast/MulticastComponent.hpp>
#include <tm_kit/transport/zeromq/ZeroMQComponent.hpp>
#ifdef TM_KIT_TRANSPORT_AERON_IPC
#include <tm_kit/transport/aeron_ipc/AeronIpcComponent.hpp>
#endif

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <algorithm>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

using namespace dev::cd606::tm;
namespace
{
    using Clock = std::chrono::steady_clock;
    using Callback = std::function<void(basic::ByteDataWithTopic &&)>;
    using Publisher = std::function<void(basic::ByteDataWithTopic &&)>;
    using Locator = transport::ConnectionLocator;

    std::string content(uint64_t seq, std::size_t n)
    {
        std::string s(n, '\0');
        uint64_t x = seq + 0x9e3779b97f4a7c15ULL;
        for (auto &c : s) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            c = static_cast<char>(x & 255);
        }
        return s;
    }

    struct Collector {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::size_t> sizes;
        std::vector<bool> seen;
        uint64_t received = 0, bad = 0, duplicates = 0, reordered = 0;
        uint64_t highest = 0;
        bool haveData = false, ready = false, blocked = false, release = false;
        bool slow;

        Collector(std::vector<std::size_t> const &s, std::size_t count, bool b)
            : sizes(s)
            , seen(count, false)
            , slow(b)
        {
        }
        ~Collector()
        {
            unblock();
        }
        void unblock()
        {
            {
                std::lock_guard<std::mutex> g(mutex);
                release = true;
            }
            cv.notify_all();
        }
        void on(basic::ByteDataWithTopic &&d)
        {
            std::unique_lock<std::mutex> g(mutex);
            if (d.topic == "ready") {
                ready = true;
                cv.notify_all();
                return;
            }
            uint64_t seq;
            try {
                std::size_t used = 0;
                if (d.topic.compare(0, 2, "p.") != 0)
                    throw std::runtime_error("topic");
                seq = std::stoull(d.topic.substr(2), &used);
                if (used != d.topic.size() - 2 || seq >= seen.size())
                    throw std::runtime_error("sequence");
            } catch (...) {
                ++bad;
                cv.notify_all();
                return;
            }
            ++received;
            if (d.content != content(seq, sizes[seq % sizes.size()]))
                ++bad;
            if (seen[seq])
                ++duplicates;
            else {
                if (haveData && seq < highest)
                    ++reordered;
                seen[seq] = true;
                highest = haveData ? std::max(highest, seq) : seq;
                haveData = true;
            }
            if (slow && seq == 0) {
                blocked = true;
                cv.notify_all();
                // The timeout also makes teardown safe if the publisher throws.
                cv.wait_for(g, std::chrono::seconds(10), [this] { return release; });
            }
            cv.notify_all();
        }
        bool waitReady()
        {
            std::unique_lock<std::mutex> g(mutex);
            return cv.wait_for(g, std::chrono::milliseconds(20), [this] { return ready; });
        }
        bool waitBlocked()
        {
            std::unique_lock<std::mutex> g(mutex);
            return cv.wait_for(g, std::chrono::seconds(5), [this] { return blocked; });
        }
        bool waitLast()
        {
            std::unique_lock<std::mutex> g(mutex);
            return cv.wait_for(g, std::chrono::seconds(5), [this] { return seen.back(); });
        }
        bool report(std::string const &label, bool expectLoss)
        {
            std::lock_guard<std::mutex> g(mutex);
            std::size_t unique = 0;
            for (bool b : seen)
                if (b)
                    ++unique;
            auto missing = seen.size() - unique;
            bool ok = bad == 0 && duplicates == 0 && reordered == 0 && seen.front() && seen.back() && (expectLoss ? missing > 0 : missing == 0);
            std::cout << (ok ? "PASS " : "FAIL ") << label << " sent=" << seen.size() << " received=" << received << " missing=" << missing << " duplicates=" << duplicates
                      << " reordered=" << reordered << " bad=" << bad << std::endl;
            return ok;
        }
    };

    // Adapters keep all transport objects alive until callbacks have been stopped.
    struct Endpoint {
        Publisher publish;
        virtual ~Endpoint() = default;
    };
    struct SeqlockEndpoint : Endpoint {
        transport::shm_seqlock::ShmSeqlockComponent pub, sub;
        SeqlockEndpoint(Locator const &l, Callback cb)
        {
            sub.shm_seqlock_addSubscriptionClient(l, transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection{}, std::move(cb));
            publish = pub.shm_seqlock_getPublisher(l);
        }
    };
    struct CursorEndpoint : Endpoint {
        transport::shm_cursor::ShmCursorComponent pub, sub;
        CursorEndpoint(Locator const &l, Callback cb)
        {
            sub.shm_cursor_addSubscriptionClient(l, transport::shm_cursor::ShmCursorComponent::NoTopicSelection{}, std::move(cb));
            publish = pub.shm_cursor_getPublisher(l);
        }
    };
    struct ZmqEndpoint : Endpoint {
        transport::zeromq::ZeroMQComponent pub, sub;
        ZmqEndpoint(Locator const &l, Callback cb)
        {
            publish = pub.zeroMQ_getPublisher(l);
            sub.zeroMQ_addSubscriptionClient(l, transport::zeromq::ZeroMQComponent::NoTopicSelection{}, std::move(cb));
        }
        ZmqEndpoint(Locator const &l, Callback cb, bool withPublisher)
        {
            if (withPublisher)
                publish = pub.zeroMQ_getPublisher(l);
            sub.zeroMQ_addSubscriptionClient(l, transport::zeromq::ZeroMQComponent::NoTopicSelection{}, std::move(cb));
        }
    };
    struct MulticastEndpoint : Endpoint {
        transport::multicast::MulticastComponent pub, sub;
        MulticastEndpoint(Locator const &l, Callback cb)
        {
            sub.multicast_addSubscriptionClient(l, transport::multicast::MulticastComponent::NoTopicSelection{}, std::move(cb));
            auto p = pub.multicast_getPublisher(l);
            publish = [p](basic::ByteDataWithTopic &&d) { p(std::move(d), 1); };
        }
    };
#ifdef TM_KIT_TRANSPORT_AERON_IPC
    struct AeronEndpoint : Endpoint {
        // Destroy subscribers before the publisher's embedded driver.
        transport::aeron_ipc::AeronIpcComponent pub, sub;
        AeronEndpoint(Locator const &l, Callback cb)
        {
            publish = pub.aeron_ipc_getPublisher(l);
            sub.aeron_ipc_addSubscriptionClient(l, transport::aeron_ipc::AeronIpcComponent::NoTopicSelection{}, std::move(cb));
        }
    };
#endif
    // Each run uses its own resources; cleanup runs after endpoints are destroyed.
    struct Cleanup {
        std::string shm, lock, socket;
        ~Cleanup()
        {
            if (!shm.empty())
                ::shm_unlink(shm.c_str());
            if (!lock.empty())
                ::unlink(lock.c_str());
            if (!socket.empty())
                ::unlink(socket.c_str());
        }
    };
    std::string tag()
    {
        static unsigned serial = 0;
        return "tm_payload_" + std::to_string(::getpid()) + "_" + std::to_string(++serial);
    }

    template <class E>
    bool exercise(std::string const &label, Locator const &l, std::vector<std::size_t> const &sizes, bool slow = false)
    {
        constexpr uint64_t count = 2048;
        Collector c(sizes, count, slow);
        // c must outlive endpoint even on failure/exception.
        auto e = std::make_unique<E>(l, [&c](basic::ByteDataWithTopic &&d) { c.on(std::move(d)); });
        auto deadline = Clock::now() + std::chrono::seconds(10);
        bool ready = false;
        do {
            e->publish({"ready", ""});
            ready = c.waitReady();
        } while (!ready && Clock::now() < deadline);
        if (!ready) {
            std::cout << "FAIL " << label << " readiness timeout" << std::endl;
            return false;
        }
        // No duplicate probes remain in flight when data measurement begins.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (uint64_t seq = 0; seq < count; ++seq) {
            e->publish({"p." + std::to_string(seq), content(seq, sizes[seq % sizes.size()])});
            if (slow && seq == 0 && !c.waitBlocked()) {
                c.unblock();
                std::cout << "FAIL " << label << " slow reader did not receive first message" << std::endl;
                return false;
            }
            if (!slow)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        c.unblock();
        c.waitLast();
        // Wait for any duplicate or out-of-order trailing delivery, then join.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        e.reset();
        return c.report(label, slow);
    }

    template <class E>
    bool shmCase(std::string const &label, std::string const &properties, bool cursor, bool slow = false)
    {
        auto id = tag();
        Cleanup cleanup;
        cleanup.shm = cursor ? "/tm_shm_cursor." + id : "/" + id;
        cleanup.lock = "/dev/shm" + cleanup.shm + ".lock";
        auto l = Locator::parse("::::" + id + "[slotSize=8192,slotCount=" + (slow ? "8" : "4096") + "," + properties + "]");
        // Max sequence topic is p.2047 (6 bytes). Cursor's slotSize includes
        // its 16-byte header; seqlock's slotSize is topic+payload capacity.
        auto max = cursor ? 8192 - 16 - 6 : 8192 - 6;
        std::vector<std::size_t> sizes{0, 1, 7, 8, 31, 32, 39, 40, 41, 63, 64, 65, 255, 256, 257, 1023, 1024, 1025, 4095, 4096, 4097, static_cast<std::size_t>(max)};
        return exercise<E>(label, l, sizes, slow);
    }

    template <class C>
    struct ShmOps;
    template <>
    struct ShmOps<transport::shm_seqlock::ShmSeqlockComponent> {
        using C = transport::shm_seqlock::ShmSeqlockComponent;
        static uint32_t add(C &c, Locator const &l, Callback cb)
        {
            return c.shm_seqlock_addSubscriptionClient(l, C::NoTopicSelection{}, std::move(cb));
        }
        static void remove(C &c, uint32_t id)
        {
            c.shm_seqlock_removeSubscriptionClient(id);
        }
        static Publisher pub(C &c, Locator const &l)
        {
            return c.shm_seqlock_getPublisher(l);
        }
    };
    template <>
    struct ShmOps<transport::shm_cursor::ShmCursorComponent> {
        using C = transport::shm_cursor::ShmCursorComponent;
        static uint32_t add(C &c, Locator const &l, Callback cb)
        {
            return c.shm_cursor_addSubscriptionClient(l, C::NoTopicSelection{}, std::move(cb));
        }
        static void remove(C &c, uint32_t id)
        {
            c.shm_cursor_removeSubscriptionClient(id);
        }
        static Publisher pub(C &c, Locator const &l)
        {
            return c.shm_cursor_getPublisher(l);
        }
    };

    template <class C>
    bool lifecycle(std::string const &label, Locator const &l)
    {
        using O = ShmOps<C>;
        std::mutex m;
        std::vector<uint64_t> a, b;
        auto cb = [&](std::vector<uint64_t> &v) {
            return [&](basic::ByteDataWithTopic &&d) {
                if (d.topic.rfind("l.", 0) != 0)
                    return;
                std::lock_guard<std::mutex> g(m);
                v.push_back(std::stoull(d.topic.substr(2)));
            };
        };
        C pub;
        C readerA;
        auto idA = O::add(readerA, l, cb(a));
        auto p = O::pub(pub, l);
        p({"l.0", content(0, 256)});
        auto deadline = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> g(m);
                if (!a.empty())
                    break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        {
            std::lock_guard<std::mutex> g(m);
            if (a.size() != 1 || a[0] != 0) {
                std::cout << "FAIL " << label << " initial reader\n";
                return false;
            }
        }
        O::remove(readerA, idA);
        p({"l.1", content(1, 256)});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        {
            std::lock_guard<std::mutex> g(m);
            if (a.size() != 1) {
                std::cout << "FAIL " << label << " reader leave callback\n";
                return false;
            }
        }
        C readerB;
        auto idB = O::add(readerB, l, cb(b));
        p({"l.2", content(2, 256)});
        deadline = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> g(m);
                if (!b.empty())
                    break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        {
            std::lock_guard<std::mutex> g(m);
            if (b.empty() || b.back() != 2) {
                std::cout << "FAIL " << label << " new reader\n";
                return false;
            }
        }
        // The reader remains attached while the publisher object is replaced.
        pub.~C();
        new (&pub) C();
        auto p2 = O::pub(pub, l);
        p2({"l.3", content(3, 256)});
        deadline = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> g(m);
                if (std::find(b.begin(), b.end(), 3) != b.end())
                    break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        bool ok;
        {
            std::lock_guard<std::mutex> g(m);
            ok = std::find(b.begin(), b.end(), 3) != b.end() && std::is_sorted(b.begin(), b.end());
        }
        O::remove(readerB, idB);
        std::cout << (ok ? "PASS " : "FAIL ") << label << " reader leave/join/publisher restart" << std::endl;
        return ok;
    }

    template <class MakePub, class MakeSub, class Send>
    bool genericLifecycle(std::string const &label, MakePub makePub, MakeSub makeSub, Send send)
    {
        std::mutex m;
        std::vector<uint64_t> a, b;
        auto cb = [&](std::vector<uint64_t> &v) {
            return [&](basic::ByteDataWithTopic &&d) {
                if (d.topic.rfind("l.", 0) == 0) {
                    std::lock_guard<std::mutex> g(m);
                    v.push_back(std::stoull(d.topic.substr(2)));
                }
            };
        };
        auto readerA = makeSub(cb(a));
        auto publisher = makePub();
        for (int i = 0; i < 10; ++i) {
            send(*publisher, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        {
            std::lock_guard<std::mutex> g(m);
            if (a.empty()) {
                std::cout << "FAIL " << label << " initial reader\n";
                return false;
            }
        }
        readerA.reset();
        send(*publisher, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        {
            std::lock_guard<std::mutex> g(m);
            if (a.empty()) {
                std::cout << "FAIL " << label << " reader leave\n";
                return false;
            }
        }
        auto readerB = makeSub(cb(b));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (int i = 0; i < 20; ++i) {
            send(*publisher, 2);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard<std::mutex> g(m);
            if (!b.empty())
                break;
        }
        publisher.reset();
        publisher = makePub();
        for (int i = 0; i < 20; ++i) {
            send(*publisher, 3);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard<std::mutex> g(m);
            if (!b.empty() && b.back() == 3)
                break;
        }
        bool ok;
        {
            std::lock_guard<std::mutex> g(m);
            ok = !b.empty() && b.back() == 3 && std::is_sorted(b.begin(), b.end());
        }
        readerB.reset();
        publisher.reset();
        std::cout << (ok ? "PASS " : "FAIL ") << label << " reader leave/join/publisher restart" << std::endl;
        return ok;
    }
} // namespace

int main()
{
    int failures = 0;
    auto run = [&](auto f) {
        try {
            if (!f())
                ++failures;
        } catch (std::exception const &e) {
            ++failures;
            std::cout << "FAIL exception: " << e.what() << std::endl;
        }
    };
    for (auto layout : {"inline", "split", "inline-premark"}) {
        for (auto read : {"classic", "spec"}) {
            auto label = std::string("seqlock ") + layout + " " + read;
            auto props = std::string("layout=") + layout + ",read=" + read;
            run([&] { return shmCase<SeqlockEndpoint>(label, props, false); });
            run([&] { return shmCase<SeqlockEndpoint>(label + " lapping", props, false, true); });
        }
    }
    for (auto wait : {"spin", "futex"}) {
        auto label = std::string("cursor ") + wait;
        auto props = std::string("wait=") + wait + ",spinCount=100";
        run([&] { return shmCase<CursorEndpoint>(label, props, true); });
        run([&] { return shmCase<CursorEndpoint>(label + " lapping", props, true, true); });
    }
    run([&] {
        auto id = tag();
        Cleanup c;
        c.shm = "/" + id;
        c.lock = "/dev/shm" + c.shm + ".lock";
        return lifecycle<transport::shm_seqlock::ShmSeqlockComponent>("seqlock lifecycle", Locator::parse("::::" + id + "[slotSize=1024,slotCount=64]"));
    });
    run([&] {
        auto id = tag();
        Cleanup c;
        c.shm = "/tm_shm_cursor." + id;
        c.lock = "/dev/shm" + c.shm + ".lock";
        return lifecycle<transport::shm_cursor::ShmCursorComponent>("cursor lifecycle", Locator::parse("::::" + id + "[slotSize=1024,slotCount=64]"));
    });
    run([&] {
        auto id = tag();
        Cleanup cleanup;
        cleanup.socket = "/tmp/" + id + ".sock";
        Locator l{"ipc", 0, "", "", cleanup.socket};
        return exercise<ZmqEndpoint>("zeromq ipc", l, {0, 1, 7, 8, 31, 32, 63, 64, 65, 255, 256, 257, 1023, 1024, 4096, 65536});
    });
    run([&] {
        //TCP avoids ZeroMQ's ipc pathname lifetime while the old PUB socket is
        //being torn down; the lifecycle assertion is about subscriptions and
        //publisher restart, not filesystem socket cleanup.
        static int zport = 50000 + static_cast<int>(::getpid() % 10000);
        auto id = tag();
        Locator l{"127.0.0.1", ++zport};
        using E = ZmqEndpoint;
        return genericLifecycle(
            "zeromq lifecycle",
            [&] { return std::make_unique<E>(l, [](basic::ByteDataWithTopic &&) {}, true); },
            [&](Callback cb) { return std::make_unique<E>(l, std::move(cb), false); },
            [](E &e, uint64_t n) { e.publish({"l." + std::to_string(n), content(n, 256)}); });
    });
    for (auto mode : {"cbor", "binary-adhoc"}) {
        run([&] {
            static int serial = 0;
            Locator l{"239.255.44." + std::to_string(++serial), 40000 + static_cast<int>(::getpid() % 20000)};
            l = l.addProperty("interface", "lo").addProperty("envelop", mode);
            //CBOR's array/text/bytes headers consume 11 bytes at the largest
            //topic length; binary-adhoc consumes 4+6. Both fit in 1200 bytes.
            return exercise<MulticastEndpoint>(std::string("multicast ") + mode, l, {0, 1, 7, 8, 31, 32, 63, 64, 65, 255, 256, 257, 1023, 1024, 1189});
        });
        run([&, mode] {
            static int serial = 100;
            Locator l{"239.255.45." + std::to_string(++serial), 45000 + static_cast<int>(::getpid() % 10000)};
            l = l.addProperty("interface", "lo").addProperty("envelop", mode);
            using E = MulticastEndpoint;
            return genericLifecycle(
                std::string("multicast ") + mode + " lifecycle",
                [&] { return std::make_unique<E>(l, [](basic::ByteDataWithTopic &&) {}); },
                [&](Callback cb) { return std::make_unique<E>(l, std::move(cb)); },
                [](E &e, uint64_t n) { e.publish({"l." + std::to_string(n), content(n, 256)}); });
        });
    }
#ifdef TM_KIT_TRANSPORT_AERON_IPC
    run([&] {
        auto id = tag();
        auto l = Locator::parse("::::" + id + "[dir=/dev/shm/" + id + ",slotSize=8192,termLength=1048576,joinLatest=false]");
        return exercise<AeronEndpoint>("aeron ipc", l, {0, 1, 7, 8, 31, 32, 63, 64, 65, 255, 256, 257, 1023, 1024, 4095, 4096, 4097, 8186});
    });
    run([&] {
        auto id = tag();
        Locator l = Locator::parse("::::" + id + "[dir=/dev/shm/" + id + ",slotSize=1024,termLength=1048576,joinLatest=false]");
        using E = AeronEndpoint;
        return genericLifecycle(
            "aeron ipc lifecycle",
            [&] { return std::make_unique<E>(l, [](basic::ByteDataWithTopic &&) {}); },
            [&](Callback cb) { return std::make_unique<E>(l, std::move(cb)); },
            [](E &e, uint64_t n) { e.publish({"l." + std::to_string(n), content(n, 256)}); });
    });
#else
    std::cout << "SKIP aeron ipc: dependency unavailable" << std::endl;
#endif
    std::cout << "SUMMARY failures=" << failures << std::endl;
    return failures ? 1 : 0;
}
