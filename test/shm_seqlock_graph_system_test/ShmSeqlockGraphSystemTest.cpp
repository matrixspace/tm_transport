#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/infra/SynchronousRunner.hpp>
#include <tm_kit/basic/IntIDComponent.hpp>
#include <tm_kit/basic/TrivialBoostLoggingComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>
#include <tm_kit/transport/shm_seqlock/ShmSeqlockImporterExporter.hpp>
#include <tm_kit/transport/MultiTransportBroadcastListenerManagingUtils.hpp>
#include <tm_kit/transport/MultiTransportBroadcastListenerManagingUtils_SynchronousRunner.hpp>
#include <tm_kit/transport/MultiTransportBroadcastPublisherManagingUtils.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace dev::cd606::tm;
namespace
{
    using Component = transport::shm_seqlock::ShmSeqlockComponent;
    using Bytes = basic::ByteDataWithTopic;
    using Value = basic::CBOR<std::vector<int>>;
    using Typed = basic::TypedDataWithTopic<Value>;
    struct OutHooks : transport::AbstractOutgoingHookFactoryComponent<Value> {
        std::atomic<unsigned> calls{0};
        std::optional<transport::UserToWireHook> defaultHook() override
        {
            return transport::UserToWireHook{[this](basic::ByteData &&d) {
                ++calls;
                return basic::ByteData{"!" + d.content};
            }};
        }
    };
    struct InHooks : transport::AbstractIncomingHookFactoryComponent<Value> {
        std::atomic<unsigned> calls{0};
        std::optional<transport::WireToUserHook> defaultHook() override
        {
            return transport::WireToUserHook{[this](basic::ByteDataView const &d) -> std::optional<basic::ByteData> {
                ++calls;
                if (d.content.empty() || d.content[0] != '!')
                    return std::nullopt;
                return basic::ByteData{std::string(d.content.substr(1))};
            }};
        }
    };
    using Env = infra::Environment<
        infra::CheckTimeComponent<false>,
        infra::TrivialExitControlComponent,
        basic::IntIDComponent<>,
        basic::TrivialBoostLoggingComponent,
        basic::real_time_clock::ClockComponent,
        Component,
        OutHooks,
        InHooks>;
    using M = infra::RealTimeApp<Env>;
    using R = infra::AppRunner<M>;
    using Factory = transport::shm_seqlock::ShmSeqlockImporterExporter<Env>;
    using Clock = std::chrono::steady_clock;
    void require(bool ok, std::string const &why)
    {
        if (!ok)
            throw std::runtime_error(why);
    }
    template <class F>
    void until(F f, std::string const &why)
    {
        auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!f()) {
            require(Clock::now() < deadline, "timeout: " + why);
            std::this_thread::yield();
        }
    }
    std::string payload(unsigned q, std::size_t n)
    {
        std::string r(n, '\0');
        uint64_t s = q + 0x9e3779b97f4a7c15ULL;
        for (char &c : r) {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            c = static_cast<char>(s);
        }
        return r;
    }
    struct Oracle {
        std::mutex mutex;
        std::condition_variable cv;
        std::map<std::string, std::string> expected;
        std::map<std::string, unsigned> seen;
        std::vector<std::string> errors;
        unsigned ready = 0, received = 0, nextSequential = 0;
        std::map<unsigned, unsigned> nextByThread;
        void expect(Bytes const &d)
        {
            std::lock_guard<std::mutex> lock(mutex);
            expected.emplace(d.topic, d.content);
        }
        void on(Bytes const &d)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (d.topic == "__ready") {
                ++ready;
                cv.notify_all();
                return;
            }
            auto i = expected.find(d.topic);
            if (i == expected.end() || i->second != d.content)
                errors.push_back("unexpected/corrupt topic " + d.topic);
            if (++seen[d.topic] != 1)
                errors.push_back("duplicate topic " + d.topic);
            if (d.topic.compare(0, 3, "g.t") == 0) {
                auto dot = d.topic.find('.', 3);
                auto t = static_cast<unsigned>(std::stoul(d.topic.substr(3, dot - 3)));
                auto q = static_cast<unsigned>(std::stoul(d.topic.substr(dot + 1)));
                if (q != nextByThread[t]++)
                    errors.push_back("thread sequence gap/reorder");
            } else if (d.topic.compare(0, 2, "g.") == 0) {
                if (std::stoul(d.topic.substr(2)) != nextSequential++)
                    errors.push_back("sequence gap/reorder");
            }
            ++received;
            cv.notify_all();
        }
        void await(unsigned n)
        {
            std::unique_lock<std::mutex> lock(mutex);
            require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return received >= n || !errors.empty(); }), "delivery deadline");
            require(errors.empty(), errors.empty() ? "" : errors.front());
        }
        template <class Send>
        void handshake(Send send)
        {
            auto deadline = Clock::now() + std::chrono::seconds(5);
            std::unique_lock<std::mutex> lock(mutex);
            while (!ready) {
                lock.unlock();
                send(Bytes{"__ready", "ready"});
                lock.lock();
                cv.wait_for(lock, std::chrono::milliseconds(2), [&] { return ready != 0; });
                require(Clock::now() < deadline, "reader handshake");
            }
        }
        void verify(unsigned n)
        {
            std::lock_guard<std::mutex> lock(mutex);
            require(received == n && expected.size() == n && errors.empty(), "final exact delivery/content count");
        }
    };
    transport::ConnectionLocator locator(std::string const &segment, std::string const &props)
    {
        return transport::ConnectionLocator::parse("::::" + segment + "[slotCount=8192,slotSize=1024,mlock=false," + props + "]");
    }
    void rawGraph(std::string const &mode, transport::ConnectionLocator const &loc)
    {
        Oracle oracle;
        auto env = std::make_unique<Env>();
        R runner(env.get());
        auto source = M::triggerImporter<Bytes, false>();
        auto send = std::get<1>(source);
        bool multi = mode == "multitransport" || mode == "threaded_exporter";
        bool threaded = mode == "threaded_exporter";
        auto sink = M::simpleExporter<Bytes>([&](M::InnerData<Bytes> &&d) { oracle.on(d.timedData.value); });
        if (multi) {
            auto in =
                transport::MultiTransportBroadcastListenerManagingUtils<R>::oneByteDataBroadcastListener(runner, "listener", "shm_seqlock://" + loc.toSerializationFormat(), "r/(__ready|g\\..*)/");
            runner.exportItem("sink", sink, std::move(in));
            auto out =
                transport::MultiTransportBroadcastPublisherManagingUtils<R>::oneByteDataBroadcastPublisher(runner, "publisher", "shm_seqlock://" + loc.toSerializationFormat(), std::nullopt, threaded);
            runner.connect(runner.importItem("trigger", std::get<0>(source)), out);
        } else {
            runner.exportItem("sink", sink, runner.importItem("listener", Factory::createImporter(loc)));
            runner.exportItem("publisher", Factory::createExporter(loc), runner.importItem("trigger", std::get<0>(source)));
        }
        runner.finalize();
        oracle.handshake(send);
        if (multi)
            env->shm_seqlock_getPublisher(loc)({"excluded", "must be filtered"});
        static std::size_t const sizes[] = {0, 1, 39, 40, 41, 63, 64, 65, 319, 320, 321, 1020};
        unsigned n = 0;
        for (auto size : sizes) {
            Bytes d{"g." + std::to_string(n), payload(n, size)};
            // slot capacity includes topic; 1020 leaves four bytes for g.11.
            if (size + d.topic.size() > 1024)
                d.content.resize(1024 - d.topic.size());
            oracle.expect(d);
            send(Bytes{d});
            oracle.await(++n);
        }
        if (mode == "shared_exporter" || mode == "concurrent" || threaded) {
            auto direct = env->shm_seqlock_getPublisher(loc);
            std::vector<std::thread> threads;
            unsigned base = n;
            for (unsigned t = 0; t < 4; ++t) {
                for (unsigned k = 0; k < 250; ++k)
                    oracle.expect({"g.t" + std::to_string(t) + "." + std::to_string(k), payload(t * 250 + k, 256)});
                threads.emplace_back([&, t] {
                    for (unsigned k = 0; k < 250; ++k) {
                        Bytes d{"g.t" + std::to_string(t) + "." + std::to_string(k), payload(t * 250 + k, 256)};
                        if (mode == "shared_exporter" && t % 2)
                            direct(std::move(d));
                        else
                            send(std::move(d));
                    }
                });
            }
            for (auto &t : threads)
                t.join();
            n = base + 1000;
            oracle.await(n);
        }
        oracle.verify(n);
        env.reset();
        std::cout << "RESULT status=PASS count=" << n << std::endl;
    }
    void slowDownstream(transport::ConnectionLocator const &loc, bool threaded)
    {
        Oracle oracle;
        auto env = std::make_unique<Env>();
        R runner(env.get());
        std::mutex gateMutex;
        std::condition_variable gate;
        bool blocked = false, release = false;
        auto sink = M::simpleExporter<Bytes>(
            [&](M::InnerData<Bytes> &&d) {
                if (d.timedData.value.topic == "g.0") {
                    std::unique_lock<std::mutex> lock(gateMutex);
                    blocked = true;
                    gate.notify_all();
                    gate.wait(lock, [&] { return release; });
                }
                oracle.on(d.timedData.value);
            },
            infra::LiftParameters<M::TimePoint>().SuggestThreaded(threaded));
        runner.exportItem("slowSink", sink, runner.importItem("listener", Factory::createImporter(loc)));
        auto pub = env->shm_seqlock_getPublisher(loc);
        runner.finalize();
        oracle.handshake(pub);
        oracle.expect({"g.0", payload(0, 256)});
        pub({"g.0", payload(0, 256)});
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            require(gate.wait_for(lock, std::chrono::seconds(5), [&] { return blocked; }), "slow sink reached gate");
        }
        auto before = env->shm_seqlock_readerStats().begin()->second.received;
        for (unsigned n = 1; n < 1000; ++n) {
            Bytes d{"g." + std::to_string(n), payload(n, 256)};
            oracle.expect(d);
            pub(std::move(d));
        }
        bool progress;
        if (threaded) {
            until([&] { return env->shm_seqlock_readerStats().begin()->second.received >= before + 999; }, "asynchronous sink queues while reader progresses");
            progress = true;
        } else {
            progress = env->shm_seqlock_readerStats().begin()->second.received != before;
        }
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            release = true;
        }
        gate.notify_all();
        oracle.await(1000);
        oracle.verify(1000);
        require(progress == threaded, "synchronous sink stalls reader; asynchronous sink queues downstream");
        require(env->shm_seqlock_readerStats().begin()->second.lost == 0, "bounded slow sink has no transport loss");
        env.reset();
        std::cout << "RESULT status=PASS count=1000 queued=" << threaded << std::endl;
    }
    void control(transport::ConnectionLocator const &loc)
    {
        Oracle oracle;
        auto env = std::make_unique<Env>();
        R runner(env.get());
        auto sink = M::simpleExporter<Bytes>([&](M::InnerData<Bytes> &&d) { oracle.on(d.timedData.value); });
        runner.exportItem("sink", sink, runner.importItem("listener", Factory::createImporter(loc)));
        auto pub = env->shm_seqlock_getPublisher(loc);
        runner.finalize();
        oracle.handshake(pub);
        for (unsigned n = 0; n < 20; ++n) {
            // Keep a handshake marker as the latest value before each interval.
            // join=latest may replay it on restart; that legal replay must not be
            // counted as a duplicate business message from the previous interval.
            {
                std::lock_guard<std::mutex> lock(oracle.mutex);
                oracle.ready = 0;
            }
            oracle.handshake(pub);
            runner.controlAll("listener", "stop", {});
            until([&] { return env->shm_seqlock_threadHandles().empty(); }, "importer stop");
            runner.controlAll("listener", "restart", {});
            {
                std::lock_guard<std::mutex> lock(oracle.mutex);
                oracle.ready = 0;
            }
            oracle.handshake(pub);
            Bytes d{"g." + std::to_string(n), payload(n, 321)};
            oracle.expect(d);
            pub(Bytes{d});
            oracle.await(n + 1);
        }
        oracle.verify(20);
        env.reset();
        std::cout << "RESULT status=PASS count=20" << std::endl;
    }
    void typedGraph(transport::ConnectionLocator const &loc, bool explicitHooks)
    {
        auto env = std::make_unique<Env>();
        R runner(env.get());
        std::mutex mutex;
        std::condition_variable cv;
        unsigned count = 0, ready = 0;
        bool bad = false;
        auto source = M::triggerImporter<Typed, false>();
        auto send = std::get<1>(source);
        std::optional<transport::UserToWireHook> out;
        std::optional<transport::WireToUserHook> in;
        if (explicitHooks) {
            out = transport::UserToWireHook{[](basic::ByteData &&d) { return basic::ByteData{"?" + d.content}; }};
            in = transport::WireToUserHook{[](basic::ByteDataView const &d) -> std::optional<basic::ByteData> {
                if (d.content.empty() || d.content[0] != '?')
                    return std::nullopt;
                return basic::ByteData{std::string(d.content.substr(1))};
            }};
        }
        auto sink = M::simpleExporter<Typed>([&](M::InnerData<Typed> &&d) {
            std::lock_guard<std::mutex> lock(mutex);
            if (d.timedData.value.topic == "__ready")
                ++ready;
            else {
                auto const &v = d.timedData.value;
                if (v.topic != "g." + std::to_string(count) || v.content.value != std::vector<int>{static_cast<int>(count), -1, 65536})
                    bad = true;
                ++count;
            }
            cv.notify_all();
        });
        runner.exportItem("sink", sink, runner.importItem("listener", Factory::createTypedImporter<Value>(loc, Component::NoTopicSelection{}, in)));
        runner.exportItem("publisher", Factory::createTypedExporter<Value>(loc, out), runner.importItem("trigger", std::get<0>(source)));
        runner.finalize();
        auto deadline = Clock::now() + std::chrono::seconds(5);
        while (true) {
            send({"__ready", Value{{1}}});
            std::unique_lock<std::mutex> lock(mutex);
            if (cv.wait_for(lock, std::chrono::milliseconds(2), [&] { return ready != 0; }))
                break;
            require(Clock::now() < deadline, "typed handshake");
        }
        auto raw = env->shm_seqlock_getPublisher(loc);
        std::string prefix = explicitHooks ? "?" : "!";
        // Invalid CBOR, wrong CBOR type and truncation must not reach the sink.
        for (auto const &invalid :
             {std::string("\xff", 1),
              std::string(
                  "\x63"
                  "abc",
                  4),
              std::string("\x83\x01", 2)})
            raw({"invalid", prefix + invalid});
        raw({"hook-rejected", "bad-prefix"});
        for (unsigned n = 0; n < 20; ++n) {
            send({"g." + std::to_string(n), Value{{static_cast<int>(n), -1, 65536}}});
            std::unique_lock<std::mutex> lock(mutex);
            require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return count >= n + 1 || bad; }), "typed delivery");
            require(!bad && count == n + 1, "typed decode/content/count");
        }
        if (explicitHooks)
            require(env->OutHooks::calls == 0 && env->InHooks::calls == 0, "explicit hooks override defaults");
        else
            require(env->OutHooks::calls >= 21 && env->InHooks::calls >= 25, "default typed hooks invoked");
        env.reset();
        std::cout << "RESULT status=PASS count=20" << std::endl;
    }
    void fetch(transport::ConnectionLocator const &loc, bool typed, bool multi)
    {
        Oracle oracle;
        auto env = std::make_unique<Env>();
        auto pub = env->shm_seqlock_getPublisher(loc);
        auto observer = env->shm_seqlock_addSubscriptionClient(loc, std::string("__ready"), [&](Bytes &&d) { oracle.on(d); });
        std::string channel = "shm_seqlock://" + loc.toSerializationFormat();
        if (typed) {
            auto predicate = [](Value const &v) { return v.value == std::vector<int>{42, 7}; };
            auto f = multi ? transport::MultiTransportBroadcastFirstUpdateQueryManagingUtils<Env>::fetchTypedFirstUpdateAndDisconnect<Value>(env.get(), channel, "g", predicate)
                           : Factory::fetchTypedFirstUpdateAndDisconnect<Value>(env.get(), loc, std::string("g"), predicate);
            oracle.handshake(pub);
            pub({"other", "!" + basic::bytedata_utils::RunSerializer<Value>::apply(Value{{42, 7}})});
            pub({"g", "!\xff"});
            pub({"g", "!" + basic::bytedata_utils::RunSerializer<Value>::apply(Value{{1}})});
            // The control observer shares this component's reader thread. An
            // observed marker after the rejected values proves they were read.
            {
                std::lock_guard<std::mutex> lock(oracle.mutex);
                oracle.ready = 0;
            }
            oracle.handshake(pub);
            require(f.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready, "typed predicate/invalid must not satisfy future");
            pub({"g", "!" + basic::bytedata_utils::RunSerializer<Value>::apply(Value{{42, 7}})});
            require(f.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "typed future deadline");
            auto d = f.get();
            require(d.topic == "g" && d.content.value == std::vector<int>{42, 7}, "typed future result");
        } else {
            auto f = multi ? transport::MultiTransportBroadcastFirstUpdateQueryManagingUtils<Env>::fetchFirstUpdateAndDisconnect(env.get(), channel, "g")
                           : Factory::fetchFirstUpdateAndDisconnect(env.get(), loc, std::string("g"));
            oracle.handshake(pub);
            pub({"other", "ignored"});
            {
                std::lock_guard<std::mutex> lock(oracle.mutex);
                oracle.ready = 0;
            }
            oracle.handshake(pub);
            require(f.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready, "raw topic must not satisfy future");
            Bytes expected{"g", std::string("a\0b", 3)};
            pub(Bytes{expected});
            require(f.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "raw future deadline");
            auto d = f.get();
            require(d.topic == expected.topic && d.content == expected.content, "raw future result");
        }
        env->shm_seqlock_removeSubscriptionClient(observer);
        until([&] { return env->shm_seqlock_threadHandles().empty(); }, "fetch automatically unsubscribes");
        env.reset();
        std::cout << "RESULT status=PASS count=1" << std::endl;
    }
    void dynamicListener(transport::ConnectionLocator const &loc)
    {
        Oracle oracle;
        auto env = std::make_unique<Env>();
        R runner(env.get());
        using Input = transport::MultiTransportBroadcastListenerInput;
        using Output = transport::MultiTransportBroadcastListenerOutput;
        using Response = M::KeyedData<Input, Output>;
        std::mutex mutex;
        std::condition_variable cv;
        unsigned responses = 0, delivered = 0;
        uint32_t subscription = 0;
        bool bad = false;
        auto requests = M::triggerImporter<M::Key<Input>, false>();
        auto listener = M::onOrderFacilityWithExternalEffects(new transport::MultiTransportBroadcastListener<Env, Value>(env->InHooks::defaultHook()));
        auto responseSink = M::simpleExporter<Response>([&](M::InnerData<Response> &&d) {
            std::lock_guard<std::mutex> lock(mutex);
            auto const &v = d.timedData.value.data.value;
            if (auto add = std::get_if<transport::MultiTransportBroadcastListenerAddSubscriptionResponse>(&v))
                subscription = add->subscriptionID;
            ++responses;
            cv.notify_all();
        });
        auto dataSink = M::simpleExporter<Typed>([&](M::InnerData<Typed> &&d) {
            std::lock_guard<std::mutex> lock(mutex);
            if (d.timedData.value.content.value != std::vector<int>{static_cast<int>(delivered), 7})
                bad = true;
            ++delivered;
            cv.notify_all();
        });
        runner.registerOnOrderFacilityWithExternalEffects("dynamic", listener);
        runner.placeOrderWithFacilityWithExternalEffects(runner.importItem("requests", std::get<0>(requests)), listener, runner.exporterAsSink("responses", responseSink));
        runner.exportItem("data", dataSink, runner.facilityWithExternalEffectsAsSource(listener));
        auto pub = env->shm_seqlock_getPublisher(loc);
        auto observer = env->shm_seqlock_addSubscriptionClient(loc, std::string("__ready"), [&](Bytes &&d) { oracle.on(d); });
        runner.finalize();
        oracle.handshake(pub);
        unsigned expectedResponses = 0;
        auto command = [&](Input input) {
            std::get<1>(requests)(M::keyify<Input>(std::move(input)));
            ++expectedResponses;
            std::unique_lock<std::mutex> lock(mutex);
            require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return responses == expectedResponses; }), "dynamic subscription response");
        };
        for (unsigned n = 0; n < 20; ++n) {
            command(Input{transport::MultiTransportBroadcastListenerAddSubscription{transport::MultiTransportBroadcastListenerConnectionType::ShmSeqlock, loc, "g"}});
            pub({"g", "!" + basic::bytedata_utils::RunSerializer<Value>::apply(Value{{static_cast<int>(n), 7}})});
            {
                std::unique_lock<std::mutex> lock(mutex);
                require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return delivered >= n + 1 || bad; }), "dynamic listener delivers");
                require(!bad && delivered == n + 1, "dynamic listener exact content/count");
            }
            command(Input{transport::MultiTransportBroadcastListenerRemoveSubscription{transport::MultiTransportBroadcastListenerConnectionType::ShmSeqlock, subscription}});
            pub({"g", "!" + basic::bytedata_utils::RunSerializer<Value>::apply(Value{{-1, 7}})});
            {
                std::lock_guard<std::mutex> lock(oracle.mutex);
                oracle.ready = 0;
            }
            oracle.handshake(pub);
            {
                std::lock_guard<std::mutex> lock(mutex);
                require(delivered == n + 1 && !bad, "removed listener stays silent");
            }
        }
        command(Input{transport::MultiTransportBroadcastListenerRemoveAllSubscriptions{}});
        env->shm_seqlock_removeSubscriptionClient(observer);
        require(env->shm_seqlock_threadHandles().empty(), "dynamic remove-all releases reader");
        env.reset();
        std::cout << "RESULT status=PASS count=20 responses=" << responses << std::endl;
    }
    void synchronous(transport::ConnectionLocator const &loc)
    {
        Oracle oracle;
        auto env = std::make_unique<Env>();
        infra::SynchronousRunner<M> runner(env.get());
        auto source = transport::MultiTransportBroadcastListenerManagingUtils<infra::SynchronousRunner<M>>::oneBroadcastListenerWithTopic<Value>(
            runner,
            transport::MultiTransportBroadcastListenerSpec<Value>{"sync", "shm_seqlock://" + loc.toSerializationFormat(), "g"},
            [](std::string const &) -> std::optional<transport::WireToUserHook> { return std::nullopt; });
        auto result = runner.importItem(source);
        auto pub = env->shm_seqlock_getPublisher(loc);
        auto observer = env->shm_seqlock_addSubscriptionClient(loc, std::string("__ready"), [&](Bytes &&d) { oracle.on(d); });
        oracle.handshake(pub);
        auto iter = result->begin();
        for (int n = 0; n < 20; ++n) {
            pub({"g", "!" + basic::bytedata_utils::RunSerializer<Value>::apply(Value{{n, 7}})});
            auto d = iter.fetch_with_time_out(std::chrono::seconds(5));
            require(d && d->timedData.value.topic == "g" && d->timedData.value.content.value == std::vector<int>{n, 7}, "synchronous manager typed delivery");
            ++iter;
        }
        env->shm_seqlock_removeSubscriptionClient(observer);
        // The result's destructor sends importer control("stop") using its Env.
        // It must therefore be destroyed before Env, and stop must finish while
        // the importer and synchronous runner still own the node.
        result.reset();
        until([&] { return env->shm_seqlock_threadHandles().empty(); }, "synchronous result stops importer");
        env.reset();
        std::cout << "RESULT status=PASS count=20" << std::endl;
    }
} // namespace
int main(int argc, char **argv)
{
    try {
        require(argc >= 3, "usage: graph-test case segment [layout] [read] [wait]");
        std::string mode = argv[1];
        std::string layout = argc > 3 ? argv[3] : "inline";
        std::string read = argc > 4 ? argv[4] : "classic";
        std::string wait = argc > 5 ? argv[5] : "futex";
        auto loc = locator(argv[2], "layout=" + layout + ",read=" + read + ",wait=" + wait + ",spinCount=1,writerLock=true");
        if (mode == "raw_graph" || mode == "multitransport" || mode == "concurrent" || mode == "threaded_exporter" || mode == "shared_exporter")
            rawGraph(mode, loc);
        else if (mode == "typed_graph" || mode == "typed_explicit_hooks")
            typedGraph(loc, mode == "typed_explicit_hooks");
        else if (mode == "slow_sync" || mode == "slow_async")
            slowDownstream(loc, mode == "slow_async");
        else if (mode == "control")
            control(loc);
        else if (mode == "fetch_raw" || mode == "fetch_typed" || mode == "multi_fetch_raw" || mode == "multi_fetch_typed")
            fetch(loc, mode.find("typed") != std::string::npos, mode.find("multi_") == 0);
        else if (mode == "dynamic_listener")
            dynamicListener(loc);
        else if (mode == "synchronous")
            synchronous(loc);
        else
            throw std::runtime_error("unknown case: " + mode);
    } catch (std::exception const &e) {
        std::cerr << "RESULT status=FAIL error=" << e.what() << std::endl;
        return 1;
    }
}
