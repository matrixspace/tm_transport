// Functional tests for binding the IPC transports' internal threads to CPUs
// through the ConnectionLocator cpu / senderCpu / driverCpu properties.
//
// Every transport that owns a worker thread is exercised with a fake
// IThreadCpuAllocator, so the test verifies both the provider plumbing and the
// actual thread affinity (pthread_getaffinity_np). The hwloc cases run only when
// the build found hwloc (TEST_HAS_HWLOC).
//
// Exit code: 0 when every check passed, 1 otherwise.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>

#include <tm_kit/basic/ByteData.hpp>
#ifdef TEST_HAS_HWLOC
#include <tm_kit/basic/HwlocThreadCpuAllocator.hpp>
#endif

#include <tm_kit/transport/ThreadCpuLeaseProviderInstall.hpp>

#include <tm_kit/transport/multicast/MulticastComponent.hpp>
#include <tm_kit/transport/multicast/MulticastImporterExporter.hpp>
#include <tm_kit/transport/zeromq/ZeroMQComponent.hpp>
#include <tm_kit/transport/zeromq/ZeroMQImporterExporter.hpp>
#include <tm_kit/transport/nng/NNGComponent.hpp>
#include <tm_kit/transport/shared_memory_broadcast/SharedMemoryBroadcastComponent.hpp>
#include <tm_kit/transport/singlecast/SinglecastComponent.hpp>
#include <tm_kit/transport/shm_seqlock/ShmSeqlockComponent.hpp>
#include <tm_kit/transport/shm_cursor/ShmCursorComponent.hpp>
#ifdef TM_KIT_TRANSPORT_AERON_IPC
#include <tm_kit/transport/aeron_ipc/AeronIpcComponent.hpp>
#endif

using namespace dev::cd606::tm;
using transport::ConnectionLocator;

namespace {

    int failures = 0;
    int checks = 0;

    void check(bool ok, std::string const &name)
    {
        ++checks;
        std::printf("%s: %s\n", ok ? "ok" : "FAIL", name.c_str());
        if (!ok) {
            ++failures;
        }
    }
    template <class F>
    bool waitUntil(F pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
    {
        auto end = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    struct BindFailure : std::runtime_error {
        explicit BindFailure(std::string const &s) : std::runtime_error(s) {}
    };

    std::vector<int> affinityOf(pthread_t h)
    {
        std::vector<int> r;
        if (h == 0) {
            return r;
        }
        cpu_set_t set;
        CPU_ZERO(&set);
        if (pthread_getaffinity_np(h, sizeof(set), &set) == 0) {
            for (int c = 0; c < CPU_SETSIZE; ++c) {
                if (CPU_ISSET(c, &set)) {
                    r.push_back(c);
                }
            }
        }
        return r;
    }
    std::vector<int> allowedCpus()
    {
        return affinityOf(pthread_self());
    }

    bool isSubset(std::vector<int> const &sub, std::vector<int> const &super)
    {
        for (int c : sub) {
            bool found = false;
            for (int x : super) {
                if (x == c) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }

    // Fake allocator: hands out the process' allowed CPUs in order.
    struct FakeAllocator : infra::IThreadCpuAllocator {
        std::mutex m;
        std::vector<int> pool;
        std::multiset<int> held, reserved;
        int acquireCount = 0;
        int reserveCount = 0;
        int releaseCount = 0;
        std::vector<std::string> whos;
        int lastAcquired = -1;
        std::string lastError;

        void setPool(std::vector<int> const &p)
        {
            std::lock_guard<std::mutex> _(m);
            pool = p;
        }
        void reset()
        {
            std::lock_guard<std::mutex> _(m);
            held.clear();
            reserved.clear();
            acquireCount = reserveCount = releaseCount = 0;
            whos.clear();
            lastAcquired = -1;
            lastError.clear();
        }
        int acquire(infra::CpuAutoBindRequest const &req, std::string const &who) override
        {
            std::lock_guard<std::mutex> _(m);
            ++acquireCount;
            whos.push_back(who);
            if (req.numaNode == 99) {
                lastError = "no CPU on numa node 99 for " + who;
                throw BindFailure(lastError);
            }
            for (int c : pool) {
                if (!held.count(c) && !reserved.count(c)) {
                    held.insert(c);
                    lastAcquired = c;
                    return c;
                }
            }
            lastError = "fake allocator exhausted for " + who;
            throw BindFailure(lastError);
        }
        void reserve(std::vector<int> const &cpus, std::string const &who) override
        {
            std::lock_guard<std::mutex> _(m);
            ++reserveCount;
            whos.push_back(who);
            for (int c : cpus) {
                if (held.count(c) || reserved.count(c)) {
                    lastError = "reserve conflict on " + std::to_string(c) + " for " + who;
                    throw BindFailure(lastError);
                }
            }
            reserved.insert(cpus.begin(), cpus.end());
        }
        void release(std::vector<int> const &cpus) noexcept override
        {
            std::lock_guard<std::mutex> _(m);
            ++releaseCount;
            for (int c : cpus) {
                auto it = held.find(c);
                if (it != held.end()) {
                    held.erase(it);
                } else if ((it = reserved.find(c)) != reserved.end()) {
                    reserved.erase(it);
                }
            }
        }
        std::size_t heldCount()
        {
            std::lock_guard<std::mutex> _(m);
            return held.size();
        }
        std::size_t reservedCount()
        {
            std::lock_guard<std::mutex> _(m);
            return reserved.size();
        }
        bool sawWhoContaining(std::string const &s)
        {
            std::lock_guard<std::mutex> _(m);
            for (auto const &w : whos) {
                if (w.find(s) != std::string::npos) {
                    return true;
                }
            }
            return false;
        }
    };
    FakeAllocator fake;
    struct FakeAccess {
        static infra::IThreadCpuAllocator &instance()
        {
            return fake;
        }
    };

    struct TestClockComponent {
        using TimePointType = std::chrono::system_clock::time_point;
        using DurationType = std::chrono::system_clock::duration;
        static constexpr bool PreserveInputRelativeOrder = false;
        static constexpr bool CanBeActualTimeClock = true;
        TimePointType now() { return std::chrono::system_clock::now(); }
        TimePointType resolveTime() { return now(); }
        TimePointType resolveTime(TimePointType const &) { return now(); }
        void log(infra::LogLevel, std::string const &) {}
    };

    using TestBase = infra::Environment<
        infra::CheckTimeComponent<false>,
        infra::TrivialExitControlComponent,
        TestClockComponent>;
    using AllComponents = infra::Environment<
        transport::multicast::MulticastComponent,
        transport::zeromq::ZeroMQComponent,
        transport::nng::NNGComponent,
        transport::shared_memory_broadcast::SharedMemoryBroadcastComponent,
        transport::singlecast::SinglecastComponent,
        transport::shm_seqlock::ShmSeqlockComponent,
        transport::shm_cursor::ShmCursorComponent
#ifdef TM_KIT_TRANSPORT_AERON_IPC
        , transport::aeron_ipc::AeronIpcComponent
#endif
    >;
    using Env = infra::Environment<TestBase, infra::ThreadCpuAllocatorComponent<FakeAccess>, AllComponents>;

    std::atomic<int> ordinalCounter{1};
    int nextOrdinal()
    {
        return ordinalCounter.fetch_add(1);
    }
    std::string const &testPrefix()
    {
        static std::string const p = [] {
            char const *t = std::getenv("IPC_AFFINITY_TAG");
            return std::string((t != nullptr && *t != '\0') ? t : "tm_aff");
        }();
        return p;
    }
    std::string tag(int ordinal)
    {
        return testPrefix() + "_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(ordinal);
    }

    void noopHandler(basic::ByteDataWithTopic &&) {}

    struct TransportCase {
        std::string name;
        bool sharedThread;
        // false when unsubscribing does not stop the transport's worker thread
        // (aeron_ipc keeps its subscriber, and the binding, until teardown)
        bool stopOnUnsubscribe;
        std::function<uint32_t(Env &, ConnectionLocator const &)> sub;
        std::function<void(Env &, uint32_t)> unsub;
        std::function<std::thread::native_handle_type(Env &)> readerHandle;
        // null for transports without a component-wide sender thread
        std::function<void(Env &, ConnectionLocator const &)> startSender;
        std::function<std::thread::native_handle_type(Env &)> senderHandle;
        std::function<void(Env &, transport::ThreadCpuLeaseProvider)> setProvider;
        // ordinal fixes the shared identity; cpuProp is like "cpu=auto" or ""
        std::function<ConnectionLocator(int, std::string const &)> locator;
    };

    std::thread::native_handle_type pickThread(
        std::unordered_map<ConnectionLocator, std::thread::native_handle_type> const &m)
    {
        for (auto const &kv : m) {
            if (!(kv.first == ConnectionLocator())) {
                return kv.second;
            }
        }
        if (!m.empty()) {
            return m.begin()->second;
        }
        return std::thread::native_handle_type{};
    }

    std::vector<TransportCase> makeCases()
    {
        std::vector<TransportCase> cases;

        cases.push_back({
            "shm_seqlock", true, true,
            [](Env &e, ConnectionLocator const &l) {
                return e.shm_seqlock_addSubscriptionClient(l, transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection(), noopHandler);
            },
            [](Env &e, uint32_t id) { e.shm_seqlock_removeSubscriptionClient(id); },
            [](Env &e) { return pickThread(e.shm_seqlock_threadHandles()); },
            nullptr, nullptr,
            [](Env &e, transport::ThreadCpuLeaseProvider p) { e.shm_seqlock_setThreadCpuLease(std::move(p)); },
            [](int o, std::string const &cpuProp) {
                return ConnectionLocator::parse("::::" + tag(o) + (cpuProp.empty() ? "" : "[" + cpuProp + "]"));
            }
        });

        cases.push_back({
            "shm_cursor", true, true,
            [](Env &e, ConnectionLocator const &l) {
                return e.shm_cursor_addSubscriptionClient(l, transport::shm_cursor::ShmCursorComponent::NoTopicSelection(), noopHandler);
            },
            [](Env &e, uint32_t id) { e.shm_cursor_removeSubscriptionClient(id); },
            [](Env &e) { return pickThread(e.shm_cursor_threadHandles()); },
            nullptr, nullptr,
            [](Env &e, transport::ThreadCpuLeaseProvider p) { e.shm_cursor_setThreadCpuLease(std::move(p)); },
            [](int o, std::string const &cpuProp) {
                return ConnectionLocator::parse("::::" + tag(o) + (cpuProp.empty() ? "" : "[" + cpuProp + "]"));
            }
        });

        cases.push_back({
            "shared_memory_broadcast", true, true,
            [](Env &e, ConnectionLocator const &l) {
                return e.shared_memory_broadcast_addSubscriptionClient(l, transport::shared_memory_broadcast::SharedMemoryBroadcastComponent::NoTopicSelection(), noopHandler);
            },
            [](Env &e, uint32_t id) { e.shared_memory_broadcast_removeSubscriptionClient(id); },
            [](Env &e) { return pickThread(e.shared_memory_broadcast_threadHandles()); },
            nullptr, nullptr,
            [](Env &e, transport::ThreadCpuLeaseProvider p) { e.shared_memory_broadcast_setThreadCpuLease(std::move(p)); },
            [](int o, std::string const &cpuProp) {
                std::string props = "size=1048576";
                if (!cpuProp.empty()) {
                    props += "," + cpuProp;
                }
                return ConnectionLocator::parse("::::" + tag(o) + "[" + props + "]");
            }
        });

        cases.push_back({
            "zeromq", true, true,
            [](Env &e, ConnectionLocator const &l) {
                return e.zeroMQ_addSubscriptionClient(l, transport::zeromq::ZeroMQComponent::NoTopicSelection(), noopHandler);
            },
            [](Env &e, uint32_t id) { e.zeroMQ_removeSubscriptionClient(id); },
            [](Env &e) { return pickThread(e.zeromq_threadHandles()); },
            nullptr, nullptr,
            [](Env &e, transport::ThreadCpuLeaseProvider p) { e.zeroMQ_setThreadCpuLease(std::move(p)); },
            [](int o, std::string const &cpuProp) {
                return ConnectionLocator::parse("ipc:0:::/dev/shm/" + tag(o) + (cpuProp.empty() ? "" : "[" + cpuProp + "]"));
            }
        });

        cases.push_back({
            "nng", true, true,
            [](Env &e, ConnectionLocator const &l) {
                return e.nng_addSubscriptionClient(l, transport::nng::NNGComponent::NoTopicSelection(), noopHandler);
            },
            [](Env &e, uint32_t id) { e.nng_removeSubscriptionClient(id); },
            [](Env &e) { return pickThread(e.nng_threadHandles()); },
            nullptr, nullptr,
            [](Env &e, transport::ThreadCpuLeaseProvider p) { e.nng_setThreadCpuLease(std::move(p)); },
            [](int o, std::string const &cpuProp) {
                return ConnectionLocator::parse("ipc:0:::/dev/shm/" + tag(o) + (cpuProp.empty() ? "" : "[" + cpuProp + "]"));
            }
        });

        cases.push_back({
            "multicast", true, true,
            [](Env &e, ConnectionLocator const &l) {
                return e.multicast_addSubscriptionClient(l, transport::multicast::MulticastComponent::NoTopicSelection(), noopHandler);
            },
            [](Env &e, uint32_t id) { e.multicast_removeSubscriptionClient(id); },
            [](Env &e) { return pickThread(e.multicast_threadHandles()); },
            [](Env &e, ConnectionLocator const &l) {
                (void)e.multicast_getPublisher(l);
            },
            [](Env &e) {
                auto m = e.multicast_threadHandles();
                auto it = m.find(ConnectionLocator());
                return (it == m.end()) ? std::thread::native_handle_type{} : it->second;
            },
            [](Env &e, transport::ThreadCpuLeaseProvider p) { e.multicast_setThreadCpuLease(std::move(p)); },
            [](int o, std::string const &cpuProp) {
                int port = 40000 + static_cast<int>(::getpid() % 1000) * 2 + (o % 2);
                std::string props = "interface=lo";
                if (!cpuProp.empty()) {
                    props += "," + cpuProp;
                }
                return ConnectionLocator::parse("239.1.2.3:" + std::to_string(port) + "[" + props + "]");
            }
        });

        cases.push_back({
            "singlecast", true, true,
            [](Env &e, ConnectionLocator const &l) {
                return e.singlecast_addSubscriptionClient(l, transport::singlecast::SinglecastComponent::NoTopicSelection(), noopHandler);
            },
            [](Env &e, uint32_t id) { e.singlecast_removeSubscriptionClient(id); },
            [](Env &e) { return pickThread(e.singlecast_threadHandles()); },
            [](Env &e, ConnectionLocator const &l) {
                (void)e.singlecast_getPublisher(l);
            },
            [](Env &e) {
                auto m = e.singlecast_threadHandles();
                auto it = m.find(ConnectionLocator());
                return (it == m.end()) ? std::thread::native_handle_type{} : it->second;
            },
            [](Env &e, transport::ThreadCpuLeaseProvider p) { e.singlecast_setThreadCpuLease(std::move(p)); },
            [](int o, std::string const &cpuProp) {
                int port = 41000 + static_cast<int>(::getpid() % 1000) * 2 + (o % 2);
                return ConnectionLocator::parse("127.0.0.1:" + std::to_string(port) + (cpuProp.empty() ? "" : "[" + cpuProp + "]"));
            }
        });

#ifdef TM_KIT_TRANSPORT_AERON_IPC
        cases.push_back({
            "aeron_ipc", false, false,
            [](Env &e, ConnectionLocator const &l) {
                return e.aeron_ipc_addSubscriptionClient(l, transport::aeron_ipc::AeronIpcComponent::NoTopicSelection(), noopHandler);
            },
            [](Env &e, uint32_t id) { e.aeron_ipc_removeSubscriptionClient(id); },
            [](Env &e) { return pickThread(e.aeron_ipc_threadHandles()); },
            nullptr, nullptr,
            [](Env &e, transport::ThreadCpuLeaseProvider p) { e.aeron_ipc_setThreadCpuLease(std::move(p)); },
            [](int o, std::string const &cpuProp) {
                std::string props = "driver=external,dir=/dev/shm/aeron_" + tag(o);
                if (!cpuProp.empty()) {
                    props += "," + cpuProp;
                }
                return ConnectionLocator::parse("::::" + tag(o) + "[" + props + "]");
            }
        });
#endif

        return cases;
    }

    std::string cpuProp(std::string const &spec)
    {
        return spec.empty() ? std::string() : ("cpu=" + spec);
    }

    // ---------------------------------------------------------------------

    void autoBindTest(TransportCase const &c)
    {
        fake.reset();
        auto env = std::make_unique<Env>();
        c.setProvider(*env, transport::threadCpuLeaseProviderFor<Env>());
        auto id = c.sub(*env, c.locator(nextOrdinal(), cpuProp("auto")));
        auto h = c.readerHandle(*env);
        check(h != std::thread::native_handle_type{}, c.name + ": cpu=auto starts the worker thread");
        check(fake.acquireCount == 1, c.name + ": cpu=auto asks the allocator once");
        int expected = fake.lastAcquired;
        check(expected >= 0 && affinityOf(h) == std::vector<int>{expected},
            c.name + ": worker thread affinity is the acquired CPU");
        check(fake.sawWhoContaining(c.name), c.name + ": allocator sees the transport in who");
        c.unsub(*env, id);
        if (c.stopOnUnsubscribe) {
            check(waitUntil([&] { return fake.heldCount() == 0; }), c.name + ": lease released when the thread stops");
        }
        env.reset();
        check(waitUntil([&] { return fake.heldCount() == 0; }), c.name + ": lease released at teardown");
    }

    void explicitCpuTest(TransportCase const &c, std::vector<int> const &allowed)
    {
        if (allowed.empty()) {
            check(false, c.name + ": no allowed CPU for cpus:<list>");
            return;
        }
        int target = allowed.back();
        fake.reset();
        auto env = std::make_unique<Env>();
        c.setProvider(*env, transport::threadCpuLeaseProviderFor<Env>());
        auto id = c.sub(*env, c.locator(nextOrdinal(), cpuProp("cpus:" + std::to_string(target))));
        auto h = c.readerHandle(*env);
        check(h != std::thread::native_handle_type{} && affinityOf(h) == std::vector<int>{target},
            c.name + ": cpus:<list> pins the worker thread");
        check(fake.reserveCount == 1, c.name + ": cpus:<list> reserves through the allocator");
        check(fake.acquireCount == 0, c.name + ": cpus:<list> does not acquire");
        c.unsub(*env, id);
        if (c.stopOnUnsubscribe) {
            check(waitUntil([&] { return fake.heldCount() == 0 && fake.reservedCount() == 0; }),
                c.name + ": explicit reservation released when the thread stops");
        }
        env.reset();
        check(waitUntil([&] { return fake.heldCount() == 0 && fake.reservedCount() == 0; }),
            c.name + ": explicit reservation released at teardown");
    }

    void noneTest(TransportCase const &c, std::vector<int> const &allowed)
    {
        fake.reset();
        auto env = std::make_unique<Env>();
        c.setProvider(*env, transport::threadCpuLeaseProviderFor<Env>());
        auto id = c.sub(*env, c.locator(nextOrdinal(), cpuProp("none")));
        auto h = c.readerHandle(*env);
        check(h != std::thread::native_handle_type{} && isSubset(affinityOf(h), allowed)
            && affinityOf(h).size() == allowed.size(),
            c.name + ": cpu=none leaves the inherited affinity");
        check(fake.acquireCount == 0 && fake.reserveCount == 0, c.name + ": cpu=none does not call the provider");
        c.unsub(*env, id);
        if (c.stopOnUnsubscribe) {
            check(waitUntil([&] { return fake.heldCount() == 0; }), c.name + ": cpu=none released");
        }

        // no property at all (zero-cost path)
        auto id2 = c.sub(*env, c.locator(nextOrdinal(), ""));
        auto h2 = c.readerHandle(*env);
        check(h2 != std::thread::native_handle_type{} && affinityOf(h2).size() == allowed.size(),
            c.name + ": missing cpu leaves the inherited affinity");
        check(fake.acquireCount == 0 && fake.reserveCount == 0, c.name + ": missing cpu does not call the provider");
        c.unsub(*env, id2);
    }

    void syntaxErrorTest(TransportCase const &c)
    {
        fake.reset();
        Env env;
        c.setProvider(env, transport::threadCpuLeaseProviderFor<Env>());
        bool threw = false;
        std::string msg;
        try {
            c.sub(env, c.locator(nextOrdinal(), "cpu=abc"));
        } catch (std::invalid_argument const &e) {
            threw = true;
            msg = e.what();
        }
        check(threw && msg.find("cpu") != std::string::npos && msg.find("abc") != std::string::npos,
            c.name + ": bad cpu syntax throws std::invalid_argument naming the property and value");

        threw = false;
        try {
            c.sub(env, c.locator(nextOrdinal(), "cpu=-1"));
        } catch (std::invalid_argument const &) {
            threw = true;
        }
        check(threw, c.name + ": cpu=-1 throws std::invalid_argument");
    }

    void noProviderTest(TransportCase const &c)
    {
        fake.reset();
        {
            Env env;
            bool threw = false;
            std::string msg;
            try {
                c.sub(env, c.locator(nextOrdinal(), cpuProp("auto")));
            } catch (std::invalid_argument const &e) {
                threw = true;
                msg = e.what();
            }
            check(threw && msg.find(c.name) != std::string::npos && msg.find("provider") != std::string::npos,
                c.name + ": binding without a provider throws std::invalid_argument");
            check(c.readerHandle(env) == std::thread::native_handle_type{},
                c.name + ": failed binding leaves no worker thread");
        }
        {
            Env env;
            auto id = c.sub(env, c.locator(nextOrdinal(), ""));
            check(c.readerHandle(env) != std::thread::native_handle_type{},
                c.name + ": no provider is fine without a cpu property");
            c.unsub(env, id);
        }
    }

    void throwingProviderTest(TransportCase const &c)
    {
        fake.reset();
        Env env;
        c.setProvider(env, [](std::string const &who, std::string const &) -> infra::ThreadCpuLease {
            throw BindFailure("provider refuses " + who);
        });
        bool threw = false;
        try {
            c.sub(env, c.locator(nextOrdinal(), cpuProp("auto")));
        } catch (BindFailure const &) {
            threw = true;
        }
        check(threw, c.name + ": provider exception reaches the subscription caller unchanged");
        check(c.readerHandle(env) == std::thread::native_handle_type{},
            c.name + ": provider failure leaves no cached subscription thread");
        auto id = c.sub(env, c.locator(nextOrdinal(), ""));
        check(c.readerHandle(env) != std::thread::native_handle_type{},
            c.name + ": a later plain subscription succeeds after the failed one");
        c.unsub(env, id);
    }

    void duplicateProviderTest(TransportCase const &c)
    {
        Env env;
        c.setProvider(env, transport::threadCpuLeaseProviderFor<Env>());
        bool threw = false;
        try {
            c.setProvider(env, transport::threadCpuLeaseProviderFor<Env>());
        } catch (std::logic_error const &) {
            threw = true;
        }
        check(threw, c.name + ": setting the provider twice throws std::logic_error");
        // the original provider still works
        fake.reset();
        auto id = c.sub(env, c.locator(nextOrdinal(), cpuProp("auto")));
        check(fake.acquireCount == 1, c.name + ": the first provider stays in effect");
        c.unsub(env, id);
        if (c.stopOnUnsubscribe) {
            check(waitUntil([&] { return fake.heldCount() == 0; }), c.name + ": lease released");
        }
    }

    void sharedThreadWarnTest(TransportCase const &c, std::vector<int> const &allowed)
    {
        if (allowed.size() < 2) {
            check(false, c.name + ": needs 2 allowed CPUs for the shared-thread test");
            return;
        }
        int first = allowed[allowed.size() - 1];
        int second = allowed[allowed.size() - 2];
        fake.reset();
        auto env = std::make_unique<Env>();
        c.setProvider(*env, transport::threadCpuLeaseProviderFor<Env>());
        int o = nextOrdinal();
        auto id1 = c.sub(*env, c.locator(o, cpuProp("cpus:" + std::to_string(first))));
        auto h1 = c.readerHandle(*env);

        std::ostringstream captured;
        auto *old = std::cerr.rdbuf(captured.rdbuf());
        uint32_t id2 = 0;
        bool threw = false;
        try {
            id2 = c.sub(*env, c.locator(o, cpuProp("cpus:" + std::to_string(second))));
        } catch (...) {
            threw = true;
        }
        std::cerr.rdbuf(old);
        check(!threw, c.name + ": a second locator with a different cpu does not throw");
        check(captured.str().find("already uses") != std::string::npos,
            c.name + ": a second locator with a different cpu warns");
        auto h2 = c.readerHandle(*env);
        check(h1 == h2 && h1 != std::thread::native_handle_type{},
            c.name + ": both locators share one receive thread");
        check(affinityOf(h1) == std::vector<int>{first}, c.name + ": the first cpu wins");
        check(fake.reserveCount == 1, c.name + ": the second cpu is not reserved");
        c.unsub(*env, id1);
        c.unsub(*env, id2);
        if (c.stopOnUnsubscribe) {
            check(waitUntil([&] { return fake.heldCount() == 0 && fake.reservedCount() == 0; }),
                c.name + ": shared thread lease released after the last unsubscribe");
        }
    }

    void senderThreadTest(TransportCase const &c, std::vector<int> const &allowed)
    {
        if (!c.startSender) {
            return;
        }
        if (allowed.size() < 2) {
            check(false, c.name + ": needs 2 allowed CPUs for the sender test");
            return;
        }
        int first = allowed[allowed.size() - 1];
        int second = allowed[allowed.size() - 2];
        fake.reset();
        auto env = std::make_unique<Env>();
        c.setProvider(*env, transport::threadCpuLeaseProviderFor<Env>());
        int o = nextOrdinal();
        c.startSender(*env, c.locator(o, "senderCpu=" + std::to_string(first)));
        auto h = c.senderHandle(*env);
        check(h != std::thread::native_handle_type{} && affinityOf(h) == std::vector<int>{first},
            c.name + ": senderCpu pins the component-wide sender thread");

        std::ostringstream captured;
        auto *old = std::cerr.rdbuf(captured.rdbuf());
        bool threw = false;
        try {
            c.startSender(*env, c.locator(nextOrdinal(), "senderCpu=" + std::to_string(second)));
        } catch (...) {
            threw = true;
        }
        std::cerr.rdbuf(old);
        check(!threw, c.name + ": a later different senderCpu does not throw");
        check(captured.str().find("already uses senderCpu") != std::string::npos,
            c.name + ": a later different senderCpu warns");
    }

    void noAllocatorTest()
    {
        using NoAllocEnv = infra::Environment<TestBase, AllComponents>;
        fake.reset();
        {
            NoAllocEnv env;
            env.shm_seqlock_setThreadCpuLease(transport::threadCpuLeaseProviderFor<NoAllocEnv>());
            bool threw = false;
            try {
                env.shm_seqlock_addSubscriptionClient(
                    ConnectionLocator::parse("::::" + tag(nextOrdinal()) + "[cpu=" + std::to_string(allowedCpus().back()) + "]")
                    , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection()
                    , noopHandler);
            } catch (infra::ThreadCpuBindError const &) {
                threw = true;
            }
            check(threw, "no allocator: threadCpuLeaseProviderFor binding throws ThreadCpuBindError");
        }
        {
            NoAllocEnv env;
            env.shm_seqlock_setThreadCpuLease(transport::threadCpuLeaseProviderFor<NoAllocEnv>());
            auto id = env.shm_seqlock_addSubscriptionClient(
                ConnectionLocator::parse("::::" + tag(nextOrdinal()) + "[cpu=none]")
                , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection()
                , noopHandler);
            check(env.shm_seqlock_threadHandles().size() == 1,
                "no allocator: cpu=none starts the reader without an allocator");
            env.shm_seqlock_removeSubscriptionClient(id);
        }
    }

    void templatePathTest(std::vector<int> const &allowed)
    {
        if (allowed.empty()) {
            check(false, "template path: no allowed CPU");
            return;
        }
        fake.reset();
        // No provider is installed by hand here: ZeroMQImporterExporter::start(env)
        // must install the default provider before creating the subscription.
        Env env;
        using M = infra::RealTimeApp<Env>;
        infra::AppRunner<M> r(&env);
        auto loc = ConnectionLocator::parse("ipc:0:::/dev/shm/" + tag(nextOrdinal()) + "[cpu=auto]");
        auto imp = transport::zeromq::ZeroMQImporterExporter<Env>::createImporter(loc);
        auto sink = M::simpleExporter<basic::ByteDataWithTopic>(
            [](typename M::template InnerData<basic::ByteDataWithTopic> &&) {});
        r.exportItem("sink", sink, r.importItem("imp", imp));
        r.finalize();
        auto handles = env.zeromq_threadHandles();
        auto h = pickThread(handles);
        check(h != std::thread::native_handle_type{} && fake.acquireCount == 1,
            "ImporterExporter path: start() installs the provider and cpu=auto binds");
        check(h != std::thread::native_handle_type{}
            && affinityOf(h).size() == 1 && affinityOf(h)[0] == fake.lastAcquired,
            "ImporterExporter path: worker thread affinity is the acquired CPU");
    }

    void churnTest(TransportCase const &c)
    {
        fake.reset();
        auto env = std::make_unique<Env>();
        c.setProvider(*env, transport::threadCpuLeaseProviderFor<Env>());
        bool ok = true;
        for (int i = 0; i < 10; ++i) {
            auto id = c.sub(*env, c.locator(nextOrdinal(), cpuProp("auto")));
            if (c.readerHandle(*env) == std::thread::native_handle_type{}) {
                ok = false;
                break;
            }
            c.unsub(*env, id);
            if (c.stopOnUnsubscribe
                && !waitUntil([&] { return fake.heldCount() == 0; }, std::chrono::milliseconds(2000))) {
                ok = false;
                break;
            }
        }
        check(ok, c.name + ": 10 subscribe/unsubscribe cycles keep working");
        env.reset();
        check(waitUntil([&] { return fake.heldCount() == 0; }), c.name + ": no bound thread survives teardown");
    }

#ifdef TEST_HAS_HWLOC
    void hwlocTest()
    {
        using HwlocEnv = infra::Environment<TestBase, basic::HwlocThreadCpuAllocatorComponent, AllComponents>;
        HwlocEnv env;
        transport::installThreadCpuLeaseProviders<HwlocEnv>(&env);
        auto id = env.shm_seqlock_addSubscriptionClient(
            ConnectionLocator::parse("::::" + tag(nextOrdinal()) + "[cpu=auto]")
            , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection()
            , noopHandler);
        auto handles = env.shm_seqlock_threadHandles();
        auto h = pickThread(handles);
        auto aff = affinityOf(h);
        check(h != std::thread::native_handle_type{} && aff.size() == 1,
            "hwloc: installThreadCpuLeaseProviders + cpu=auto binds the reader to one CPU");
        if (aff.size() == 1) {
            // the same CPU is now owned by the transport thread: a second
            // explicit reservation of it must be refused by the hwloc allocator
            bool threw = false;
            try {
                env.shm_cursor_addSubscriptionClient(
                    ConnectionLocator::parse("::::" + tag(nextOrdinal()) + "[cpu=cpus:" + std::to_string(aff[0]) + "]")
                    , transport::shm_cursor::ShmCursorComponent::NoTopicSelection()
                    , noopHandler);
            } catch (basic::CpuAutoBindError const &) {
                threw = true;
            }
            check(threw, "hwloc: a second explicit binding of the same core throws CpuAutoBindError");
        }
        env.shm_seqlock_removeSubscriptionClient(id);
    }
#endif

#ifdef TM_KIT_TRANSPORT_AERON_IPC
    void aeronDriverTest(std::vector<int> const &allowed)
    {
        if (allowed.size() < 4) {
            check(false, "aeron driver: needs at least 4 allowed CPUs");
            return;
        }
        fake.reset();
        int o = nextOrdinal();
        {
            Env env;
            env.aeron_ipc_setThreadCpuLease(transport::threadCpuLeaseProviderFor<Env>());
            std::string dir = "/dev/shm/aeron_" + tag(o);
            auto pubLoc = ConnectionLocator::parse(
                "::::" + tag(o) + "[dir=" + dir
                + ",driver=embedded,driverThreading=shared,driverCpu=auto]");
            auto pub = env.aeron_ipc_getPublisher(pubLoc);
            auto subLoc = ConnectionLocator::parse(
                "::::" + tag(o) + "_sub[dir=" + dir + ",driver=external,cpu=auto]");
            auto subId = env.aeron_ipc_addSubscriptionClient(
                subLoc, transport::aeron_ipc::AeronIpcComponent::NoTopicSelection(), noopHandler);
            check(waitUntil([&] { return fake.acquireCount >= 2; }),
                "aeron: driverCpu=auto and a subscription cpu=auto each acquire a CPU");
            auto stats = env.aeron_ipc_publisherStats(pubLoc);
            check(stats && stats->driverBindFailed == 0, "aeron: driver agent binding did not fail");
            env.aeron_ipc_removeSubscriptionClient(subId);
            (void)pub;
        }
        check(waitUntil([&] { return fake.heldCount() == 0; }),
            "aeron: driver and subscription leases released with the environment");

        // invalid driverCpu syntax fails in the calling thread, before the driver starts
        {
            Env env;
            env.aeron_ipc_setThreadCpuLease(transport::threadCpuLeaseProviderFor<Env>());
            auto loc = ConnectionLocator::parse(
                "::::" + tag(nextOrdinal()) + "[dir=/dev/shm/aeron_" + tag(o)
                + ",driver=embedded,driverCpu=abc]");
            bool threw = false;
            try {
                (void)env.aeron_ipc_getPublisher(loc);
            } catch (std::invalid_argument const &) {
                threw = true;
            }
            check(threw, "aeron: bad driverCpu syntax throws std::invalid_argument");
        }
        // driverCpu without a provider
        {
            Env env;
            auto loc = ConnectionLocator::parse(
                "::::" + tag(nextOrdinal()) + "[dir=/dev/shm/aeron_" + tag(o)
                + ",driver=embedded,driverCpu=auto]");
            bool threw = false;
            try {
                (void)env.aeron_ipc_getPublisher(loc);
            } catch (std::invalid_argument const &) {
                threw = true;
            }
            check(threw, "aeron: driverCpu without a provider throws std::invalid_argument");
        }
    }
#endif

} // namespace

int main()
{
    auto allowed = allowedCpus();
    if (allowed.empty()) {
        std::printf("FAIL: cannot read the process CPU affinity\n");
        return 1;
    }
    std::printf("allowed CPUs: %zu\n", allowed.size());
    fake.setPool(allowed);

    auto cases = makeCases();
    char const *onlyCase = std::getenv("IPC_AFFINITY_CASE");
    for (auto const &c : cases) {
        if (onlyCase != nullptr && c.name != onlyCase) {
            continue;
        }
        autoBindTest(c);
        explicitCpuTest(c, allowed);
        noneTest(c, allowed);
        syntaxErrorTest(c);
        noProviderTest(c);
        throwingProviderTest(c);
        duplicateProviderTest(c);
        if (c.sharedThread) {
            sharedThreadWarnTest(c, allowed);
        }
        senderThreadTest(c, allowed);
        churnTest(c);
    }
    noAllocatorTest();
    templatePathTest(allowed);
#ifdef TEST_HAS_HWLOC
    hwlocTest();
#endif
#ifdef TM_KIT_TRANSPORT_AERON_IPC
    aeronDriverTest(allowed);
#endif

    std::printf("SUMMARY: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
