// Control protocol is deliberately separate from the shared-memory data plane.
#include <tm_kit/transport/shm_seqlock/ShmSeqlockComponent.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace dev::cd606::tm;
namespace
{
    std::mutex outputMutex;
    std::string pauseEvent;
    int probeFd = -1;
    std::atomic<unsigned> occurrence{1};

    void emit(std::string const &s)
    {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::cout << s << std::endl;
    }
    std::string hex(std::string const &s)
    {
        if (s.empty())
            return "-";
        static char const digits[] = "0123456789abcdef";
        std::string r;
        for (unsigned char c : s) {
            r += digits[c >> 4];
            r += digits[c & 15];
        }
        return r;
    }
    std::string unhex(std::string const &s)
    {
        if (s == "-")
            return {};
        if (s.size() % 2)
            throw std::runtime_error("odd hex length");
        std::string r;
        for (std::size_t i = 0; i < s.size(); i += 2) {
            auto nibble = [](char c) -> unsigned {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                throw std::runtime_error("invalid hex");
            };
            r += static_cast<char>((nibble(s[i]) << 4) | nibble(s[i + 1]));
        }
        return r;
    }
    std::size_t querySize(transport::ConnectionLocator const &locator, std::string const &key, std::size_t fallback)
    {
        auto s = locator.query(key, "");
        if (s.empty())
            return fallback;
        return static_cast<std::size_t>(std::stoull(s));
    }

    using Component = transport::shm_seqlock::ShmSeqlockComponent;

    //F10: queries run concurrently with subscription add/remove.
    void runQueryStress(Component &comp, std::function<void(basic::ByteDataWithTopic &&)> const &pub,
                        transport::ConnectionLocator const &locator)
    {
        auto threads = querySize(locator, "stressThreads", 4);
        auto cycles = querySize(locator, "stressCycles", 200);
        std::atomic<bool> stop {false};
        std::atomic<std::uint64_t> queries {0}, received {0};
        std::vector<std::thread> workers;
        for (std::size_t i = 0; i < threads; ++i) {
            workers.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    comp.shm_seqlock_readerStats();
                    comp.shm_seqlock_publisherStats();
                    comp.shm_seqlock_threadHandles();
                    queries.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (std::size_t i = 0; i < cycles; ++i) {
            auto id = comp.shm_seqlock_addSubscriptionClient(
                locator
                , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection {}
                , [&received](basic::ByteDataWithTopic &&) { received.fetch_add(1, std::memory_order_relaxed); }
                , std::nullopt);
            pub({"t", "x"});
            comp.shm_seqlock_removeSubscriptionClient(id);
        }
        stop.store(true, std::memory_order_relaxed);
        for (auto &t : workers)
            t.join();
        emit(
            "RESULT queries=" + std::to_string(queries.load()) + " handles=" + std::to_string(comp.shm_seqlock_threadHandles().size())
            + " received=" + std::to_string(received.load()) + " cycles=" + std::to_string(cycles));
    }

    //R05: many clients are added, then all remove at the same time (including
    //duplicate and unknown ids), so the last-client stop races with removal.
    void runRemoveStress(Component &comp, std::function<void(basic::ByteDataWithTopic &&)> const &pub,
                         transport::ConnectionLocator const &locator)
    {
        auto threads = querySize(locator, "stressThreads", 4);
        auto cycles = querySize(locator, "stressCycles", 50);
        for (std::size_t c = 0; c < cycles; ++c) {
            std::atomic<std::size_t> ready {0};
            std::atomic<bool> go {false};
            std::vector<std::thread> workers;
            std::vector<std::uint32_t> ids(threads, 0);
            for (std::size_t i = 0; i < threads; ++i) {
                workers.emplace_back([&, i] {
                    ids[i] = comp.shm_seqlock_addSubscriptionClient(
                        locator
                        , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection {}
                        , [](basic::ByteDataWithTopic &&) {}
                        , std::nullopt);
                    ready.fetch_add(1, std::memory_order_release);
                    while (!go.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    comp.shm_seqlock_removeSubscriptionClient(ids[i]);
                    comp.shm_seqlock_removeSubscriptionClient(ids[i]);
                    comp.shm_seqlock_removeSubscriptionClient(0xffffffffU);
                });
            }
            while (ready.load(std::memory_order_acquire) < threads)
                std::this_thread::yield();
            pub({"t", "x"});
            go.store(true, std::memory_order_release);
            for (auto &t : workers)
                t.join();
            if (!comp.shm_seqlock_threadHandles().empty())
                throw std::runtime_error("reader handle retained after concurrent removal");
        }
        emit("RESULT cycles=" + std::to_string(cycles) + " threads=" + std::to_string(threads) + " handles=0");
    }

    //F11: provider installation, duplicate installation, missing provider and a
    //provider that throws. Every step runs in this process; failures are reported
    //as RESULT fields rather than escaping as worker errors.
    void runCpuProvider(Component &comp, transport::ConnectionLocator const &locator,
                        std::string const &baseProperties, std::string const &mode)
    {
        auto extra = baseProperties.empty() ? std::string("cpu=cpus:16-16") : baseProperties + ",cpu=cpus:16-16";
        auto cpuLocator = transport::ConnectionLocator::parse(std::string("::::") + locator.identifier() + "[" + extra + "]");
        auto addWith = [&](transport::ConnectionLocator const &loc) {
            return comp.shm_seqlock_addSubscriptionClient(
                loc
                , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection {}
                , [](basic::ByteDataWithTopic &&) {}
                , std::nullopt);
        };
        std::string result = "RESULT mode=" + mode;
        if (mode == "noprovider") {
            bool threw = false;
            std::string message;
            try {
                auto id = addWith(cpuLocator);
                comp.shm_seqlock_removeSubscriptionClient(id);
            } catch (std::exception const &e) {
                threw = true;
                message = e.what();
            }
            auto id = addWith(locator);
            comp.shm_seqlock_removeSubscriptionClient(id);
            result += " cpu_threw=" + std::to_string(threw) + " provider_message=" + std::to_string(message.find("no thread CPU lease provider") != std::string::npos)
                + " fallback_ok=1";
        } else if (mode == "provider" || mode == "dup") {
            struct ProviderCalls {
                std::mutex mutex;
                std::vector<std::pair<std::string, std::string>> calls;
            };
            auto calls = std::make_shared<ProviderCalls>();
            comp.shm_seqlock_setThreadCpuLease([calls](std::string const &who, std::string const &spec) -> infra::ThreadCpuLease {
                {
                    std::lock_guard<std::mutex> lock(calls->mutex);
                    calls->calls.push_back({who, spec});
                }
                return infra::ThreadCpuLease {};
            });
            bool duplicateThrew = false;
            if (mode == "dup") {
                try {
                    comp.shm_seqlock_setThreadCpuLease([](std::string const &, std::string const &) { return infra::ThreadCpuLease {}; });
                } catch (std::logic_error const &) {
                    duplicateThrew = true;
                }
            }
            auto id = addWith(cpuLocator);
            std::string who, spec;
            {
                std::lock_guard<std::mutex> lock(calls->mutex);
                if (!calls->calls.empty()) {
                    who = calls->calls.back().first;
                    spec = calls->calls.back().second;
                }
            }
            result += " provider_calls=" + std::to_string(calls->calls.size()) + " spec_ok=" + std::to_string(spec == "cpus:16-16")
                + " who_has_reader=" + std::to_string(who.find("reader") != std::string::npos) + " dup_threw=" + std::to_string(duplicateThrew);
            comp.shm_seqlock_removeSubscriptionClient(id);
        } else if (mode == "failure") {
            comp.shm_seqlock_setThreadCpuLease([](std::string const &, std::string const &) -> infra::ThreadCpuLease {
                throw std::runtime_error("injected provider failure");
            });
            bool threw = false;
            try {
                auto id = addWith(cpuLocator);
                comp.shm_seqlock_removeSubscriptionClient(id);
            } catch (std::runtime_error const &e) {
                threw = std::string(e.what()) == "injected provider failure";
            }
            auto id = addWith(locator);
            comp.shm_seqlock_removeSubscriptionClient(id);
            result += " bind_threw=" + std::to_string(threw) + " fallback_ok=1";
        } else {
            throw std::runtime_error("invalid cpu provider mode " + mode);
        }
        emit(result);
    }

    //R06: a callback removes another client, or removes itself and subscribes
    //again from the reader thread.
    struct CallbackState {
        Component *comp = nullptr;
        transport::ConnectionLocator const *locator = nullptr;
        std::atomic<std::uint32_t> idA {0}, idB {0}, idC {0};
        std::atomic<std::uint64_t> aCount {0}, bCount {0}, cCount {0};
        std::atomic<bool> acted {false};
        std::string mode;
    };

    std::shared_ptr<CallbackState> currentCallbackState;
    std::atomic<bool> slowRelease {false};
    std::atomic<std::uint64_t> slowCount {0}, fastCount {0};

    void runCallbackControl(Component &comp, transport::ConnectionLocator const &locator, std::string const &mode)
    {
        auto state = std::make_shared<CallbackState>();
        state->comp = &comp;
        state->locator = &locator;
        state->mode = mode;
        auto add = [&](std::function<void(basic::ByteDataWithTopic &&)> cb) {
            return comp.shm_seqlock_addSubscriptionClient(
                locator, transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection {}, std::move(cb), std::nullopt);
        };
        if (mode == "remove_other") {
            state->idB.store(add([state](basic::ByteDataWithTopic &&) {
                state->bCount.fetch_add(1, std::memory_order_relaxed);
                emit("BDATA");
            }));
            state->idA.store(add([state](basic::ByteDataWithTopic &&) {
                state->aCount.fetch_add(1, std::memory_order_relaxed);
                emit("ADATA");
                if (!state->acted.exchange(true)) {
                    auto b = state->idB.load();
                    if (b)
                        state->comp->shm_seqlock_removeSubscriptionClient(b);
                    emit("REMOVED_OTHER");
                }
            }));
        } else if (mode == "resubscribe") {
            state->idA.store(add([state](basic::ByteDataWithTopic &&) {
                state->aCount.fetch_add(1, std::memory_order_relaxed);
                emit("ADATA");
                if (!state->acted.exchange(true)) {
                    auto a = state->idA.load();
                    if (a)
                        state->comp->shm_seqlock_removeSubscriptionClient(a);
                    state->idC.store(state->comp->shm_seqlock_addSubscriptionClient(
                        *state->locator
                        , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection {}
                        , [state](basic::ByteDataWithTopic &&) {
                            state->cCount.fetch_add(1, std::memory_order_relaxed);
                            emit("CDATA");
                        }
                        , std::nullopt));
                    emit("RESUBSCRIBED");
                }
            }));
        } else {
            throw std::runtime_error("invalid callback control mode " + mode);
        }
        currentCallbackState = state;
    }
} // namespace
#ifdef TM_KIT_SHM_SEQLOCK_TEST_PROBES
namespace dev
{
    namespace cd606
    {
        namespace tm
        {
            namespace transport
            {
                namespace shm_seqlock
                {
                    void systemTestProbe(char const *event)
                    {
                        if (std::strcmp(event, "reader_joined") == 0)
                            emit("JOINED");
                        if (pauseEvent == event && occurrence.fetch_sub(1) == 1) {
                            emit(std::string("HIT ") + event);
                            char c;
                            ssize_t n;
                            do {
                                n = ::read(probeFd, &c, 1);
                            } while (n < 0 && errno == EINTR);
                            if (n != 1 || c != 'c')
                                ::_exit(91);
                        }
                    }
                } // namespace shm_seqlock
            } // namespace transport
        } // namespace tm
    } // namespace cd606
} // namespace dev
#endif

int main(int argc, char **argv)
{
    try {
        if (argc < 4)
            throw std::runtime_error("usage: worker writer|reader segment properties [pause-event fd occurrence filter hook]");
        auto role = std::string(argv[1]);
        if (argc > 4)
            pauseEvent = argv[4];
        if (argc > 5)
            probeFd = std::stoi(argv[5]);
        if (argc > 6)
            occurrence = std::stoul(argv[6]);
        auto filter = argc > 7 ? std::string(argv[7]) : "all";
        auto hook = argc > 8 ? std::string(argv[8]) : "none";
        auto locator = transport::ConnectionLocator::parse(std::string("::::") + argv[2] + "[" + argv[3] + "]");
        transport::shm_seqlock::ShmSeqlockComponent comp;
        std::function<void(basic::ByteDataWithTopic &&)> pub;
        uint32_t id = 0;
        auto addTo = [&](transport::ConnectionLocator const &which) {
            std::variant<transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection, std::string, std::regex> topic;
            if (filter.compare(0, 6, "exact:") == 0)
                topic = unhex(filter.substr(6));
            else if (filter.compare(0, 6, "regex:") == 0)
                topic = std::regex(filter.substr(6));
            std::optional<transport::WireToUserHook> incoming;
            if (hook == "unwrap")
                incoming = transport::WireToUserHook{[](basic::ByteDataView const &d) -> std::optional<basic::ByteData> {
                    if (d.content.empty() || d.content[0] != '!')
                        return std::nullopt;
                    return basic::ByteData{std::string(d.content.substr(1))};
                }};
            return comp.shm_seqlock_addSubscriptionClient(
                which,
                topic,
                [](basic::ByteDataWithTopic &&d) {
                    emit("DATA " + hex(d.topic) + " " + hex(d.content));
#ifdef TM_KIT_SHM_SEQLOCK_TEST_PROBES
                    transport::shm_seqlock::systemTestProbe("callback");
#endif
                },
                incoming);
        };
        auto add = [&] { return addTo(locator); };
        if (role == "writer") {
            std::optional<transport::UserToWireHook> outgoing;
            if (hook == "wrap")
                outgoing = transport::UserToWireHook{[](basic::ByteData &&d) { return basic::ByteData{"!" + d.content}; }};
            pub = comp.shm_seqlock_getPublisher(locator, outgoing);
        } else if (role == "reader")
            id = add();
        else if (role == "query_stress" || role == "remove_stress" || role == "cpu_provider")
            pub = comp.shm_seqlock_getPublisher(locator);
        else if (role == "callback_control")
            runCallbackControl(comp, locator, filter);
        else if (role == "multi_callback") {
            //R03: two callbacks on the same segment share one reader thread, so
            //a blocked slow callback also holds back the fast one.
            id = comp.shm_seqlock_addSubscriptionClient(
                locator
                , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection {}
                , [](basic::ByteDataWithTopic &&) {
                    slowCount.fetch_add(1, std::memory_order_relaxed);
                    emit("SLOW_IN");
                    while (!slowRelease.load(std::memory_order_acquire))
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    emit("SLOW_OUT");
                }
                , std::nullopt);
            comp.shm_seqlock_addSubscriptionClient(
                locator
                , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection {}
                , [](basic::ByteDataWithTopic &&) {
                    fastCount.fetch_add(1, std::memory_order_relaxed);
                    emit("FAST");
                }
                , std::nullopt);
        }
        else if (role == "throwing_reader")
            id = comp.shm_seqlock_addSubscriptionClient(
                locator
                , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection {}
                , [](basic::ByteDataWithTopic &&) {
                    emit("CB");
                    throw std::runtime_error("injected callback failure");
                }
                , std::nullopt);
        else if (role == "dangling_publisher") {
            auto holder = std::make_unique<Component>();
            pub = holder->shm_seqlock_getPublisher(locator);
            holder.reset();
        } else if (role != "idle")
            throw std::runtime_error("invalid role");
        emit("READY");
        for (std::string line; std::getline(std::cin, line);) {
            std::istringstream in(line);
            std::string cmd;
            in >> cmd;
            if (cmd == "SEND") {
                std::string t, p;
                in >> t >> p;
                if (!pub)
                    throw std::runtime_error("no publisher");
                pub({unhex(t), unhex(p)});
                emit("SENT");
            } else if (cmd == "ALIAS" || cmd == "TRY" || cmd == "COMPETE") {
                std::string properties;
                in >> properties;
                auto alternative = transport::ConnectionLocator::parse(std::string("::::") + argv[2] + "[" + properties + "]");
                try {
                    if (cmd == "COMPETE") {
                        transport::shm_seqlock::ShmSeqlockComponent second;
                        auto unexpected = second.shm_seqlock_getPublisher(alternative);
                        emit("ACQUIRED");
                    } else {
                        pub = comp.shm_seqlock_getPublisher(alternative);
                        emit("ACQUIRED");
                    }
                } catch (std::exception const &e) {
                    emit(std::string("REJECTED ") + e.what());
                }
            } else if (cmd == "REMOVE") {
                comp.shm_seqlock_removeSubscriptionClient(id);
                emit("REMOVED");
            } else if (cmd == "ADD") {
                id = add();
                emit("ADDED");
            } else if (cmd == "ADDP") {
                std::string properties;
                in >> properties;
                auto alternative = transport::ConnectionLocator::parse(std::string("::::") + argv[2] + "[" + properties + "]");
                id = addTo(alternative);
                emit("ADDED");
            } else if (cmd == "RUN") {
                if (role == "query_stress")
                    runQueryStress(comp, pub, locator);
                else if (role == "remove_stress")
                    runRemoveStress(comp, pub, locator);
                else if (role == "cpu_provider")
                    runCpuProvider(comp, locator, argv[3], filter);
                else
                    throw std::runtime_error("RUN unsupported for role " + role);
                emit("RUN_DONE");
            } else if (cmd == "RELEASE") {
                slowRelease.store(true, std::memory_order_release);
                emit("RELEASED");
            } else if (cmd == "REPORT") {
                if (role == "multi_callback") {
                    emit(
                        "REPORT slow=" + std::to_string(slowCount.load()) + " fast=" + std::to_string(fastCount.load())
                        + " handles=" + std::to_string(comp.shm_seqlock_threadHandles().size()));
                } else {
                    auto state = currentCallbackState;
                    if (!state)
                        throw std::runtime_error("no callback control state");
                    emit(
                        "REPORT a=" + std::to_string(state->aCount.load()) + " b=" + std::to_string(state->bCount.load()) + " c="
                        + std::to_string(state->cCount.load()) + " handles=" + std::to_string(comp.shm_seqlock_threadHandles().size()));
                }
            } else if (cmd == "STATS") {
                auto rs = comp.shm_seqlock_readerStats();
                auto ps = comp.shm_seqlock_publisherStats();
                if (role == "writer") {
                    for (auto const &p : ps)
                        emit(
                            "STATS " + std::to_string(p.second.published) + " " + std::to_string(p.second.rejected) + " " + std::to_string(p.second.epoch) + " " +
                            std::to_string(p.second.waiterResets));
                } else {
                    for (auto const &p : rs)
                        emit(
                            "STATS " + std::to_string(p.second.received) + " " + std::to_string(p.second.lost) + " " + std::to_string(p.second.lapResyncs) + " " +
                            std::to_string(p.second.publisherRestarts) + " " + std::to_string(p.second.corrupt) + " " + std::to_string(p.second.reopens));
                }
                emit("STATS_END");
            } else if (cmd == "QUIT")
                break;
            else
                throw std::runtime_error("invalid command: " + cmd);
        }
        emit("EXITING");
    } catch (std::exception const &e) {
        emit(std::string("ERROR ") + e.what());
        return 1;
    }
}
