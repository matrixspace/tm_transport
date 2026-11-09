#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/basic/RealTimeSharedPoolExecutor.hpp>

#include <tm_kit/basic/SerializationHelperMacros.hpp>
#include <tm_kit/basic/TrivialBoostLoggingComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>

#include <tm_kit/transport/MultiTransportBroadcastListener.hpp>
#include <tm_kit/transport/CrossGuidComponent.hpp>
#include <tm_kit/transport/json_rest/JsonRESTClientFacility.hpp>
#include <tm_kit/transport/json_rest/JsonRESTFacilityWrapper.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

//Transport-level regression test for the RealTimeApp threaded handler
//executors (tm_infra RealTimeThreadedExecutor.hpp). JsonRESTClientFacility
//derives from ThreadedHandler, so every client request goes through the
//executor. Run for the default DedicatedThreadExecutor and the opt-in
//shared pool:
//  1. many concurrent requests: every response arrives exactly once, with
//     the right content
//  2. sequential round trips: latency summary (informational; the server
//     side and HTTP dominate, the check only guards against a regression to
//     the old 1ms polling floor)
//Uses a port given by argv[1] (default 39871) on localhost.

using namespace dev::cd606::tm;

#define TH_REQ_FIELDS \
    ((int64_t, seq)) \
    ((std::string, payload))
#define TH_RESP_FIELDS \
    ((int64_t, seq)) \
    ((std::string, echo))
TM_BASIC_CBOR_CAPABLE_STRUCT(ThReq, TH_REQ_FIELDS);
TM_BASIC_CBOR_CAPABLE_STRUCT_SERIALIZE_NO_FIELD_NAMES(ThReq, TH_REQ_FIELDS);
TM_BASIC_CBOR_CAPABLE_STRUCT(ThResp, TH_RESP_FIELDS);
TM_BASIC_CBOR_CAPABLE_STRUCT_SERIALIZE_NO_FIELD_NAMES(ThResp, TH_RESP_FIELDS);
#undef TH_REQ_FIELDS
#undef TH_RESP_FIELDS

namespace {

    template <bool Pool>
    struct EnvFor {
        using type = infra::Environment<
            infra::CheckTimeComponent<false>
            , infra::TrivialExitControlComponent
            , basic::TrivialBoostLoggingComponent
            , basic::real_time_clock::ClockComponent
            , transport::CrossGuidComponent
            , transport::AllNetworkTransportComponents
        >;
    };
    template <>
    struct EnvFor<true> {
        using type = infra::Environment<
            infra::CheckTimeComponent<false>
            , infra::TrivialExitControlComponent
            , basic::TrivialBoostLoggingComponent
            , basic::real_time_clock::ClockComponent
            , transport::CrossGuidComponent
            , transport::AllNetworkTransportComponents
            , basic::RealTimeSharedPoolExecutorComponent
        >;
    };

    int g_failures = 0;
    void check(bool cond, std::string const &what) {
        if (!cond) {
            ++g_failures;
            std::cout << "FAIL: " << what << std::endl;
        } else {
            std::cout << "ok:   " << what << std::endl;
        }
    }

    template <class F>
    bool waitUntil(F &&pred, std::chrono::milliseconds limit) {
        auto deadline = std::chrono::steady_clock::now()+limit;
        while (!pred()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return pred();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        return true;
    }

    template <bool Pool>
    void runFor(int port) {
        using Env = typename EnvFor<Pool>::type;
        using M = infra::RealTimeApp<Env>;
        using R = infra::AppRunner<M>;
        std::string const tag = Pool ? " [shared pool]" : " [dedicated]";
        std::string const path = Pool ? "/th_pool" : "/th_dedicated";

        //environments and graphs are leaked on purpose: transport threads
        //are detached and keep references to them
        auto *env = new Env();

        //---- server
        //one runner for server and client: every finalize() starts the
        //environment's JSON REST component, so two runners on one env would
        //bind the port twice
        auto *rs = new R(env);
        auto *rc = rs;
        auto facility = M::template liftPureOnOrderFacility<ThReq>([](ThReq &&r) -> ThResp {
            return ThResp {r.seq, r.payload + ":" + std::to_string(r.seq)};
        });
        rs->registerOnOrderFacility("server_facility", facility);
        transport::json_rest::JsonRESTFacilityWrapper<M>::template wrapOnOrderFacility<ThReq, ThResp>(
            *rs, facility
            , transport::ConnectionLocator::parse("localhost:" + std::to_string(port) + ":::" + path)
            , "server_wrapper"
        );

        //---- client
        auto client = transport::json_rest::JsonRESTClientFacilityFactory<M>::template createClientFacility<ThReq, ThResp>(
            transport::ConnectionLocator::parse("localhost:" + std::to_string(port) + ":::" + path)
        );
        rc->registerOnOrderFacility("client_facility", client);
        auto trigger = M::template triggerImporter<typename M::template Key<ThReq>>();
        auto src = rc->importItem("trigger", std::get<0>(trigger));

        constexpr int64_t kConcurrent = 2000;
        std::mutex mutex;
        std::vector<int> seen(kConcurrent, 0);
        std::atomic<int64_t> received {0};
        std::atomic<int64_t> wrong {0};
        std::atomic<int64_t> lastSeq {-1};
        auto sink = M::template pureExporter<typename M::template KeyedData<ThReq, ThResp>>(
            [&](typename M::template KeyedData<ThReq, ThResp> &&d) {
                auto const &resp = d.data;
                if (resp.seq < 0 || resp.seq >= kConcurrent + 1000
                    || resp.echo != d.key.key().payload + ":" + std::to_string(resp.seq)) {
                    ++wrong;
                }
                if (resp.seq >= 0 && resp.seq < kConcurrent) {
                    std::lock_guard<std::mutex> _(mutex);
                    ++seen[resp.seq];
                }
                lastSeq.store(resp.seq, std::memory_order_release);
                received.fetch_add(1, std::memory_order_acq_rel);
            }
        );
        rc->placeOrderWithFacility(std::move(src), client, rc->exporterAsSink("sink", sink));
        rc->finalize();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto &fire = std::get<1>(trigger);

        //---- 1. concurrent requests from several threads
        {
            std::vector<std::thread> ths;
            constexpr int kThreads = 4;
            for (int t = 0; t < kThreads; ++t) {
                ths.emplace_back([&, t]() {
                    for (int64_t i = t; i < kConcurrent; i += kThreads) {
                        fire(typename M::template Key<ThReq> {ThReq {i, "p" + std::to_string(t)}});
                    }
                });
            }
            for (auto &th : ths) {
                th.join();
            }
            bool all = waitUntil([&]() {return received.load() >= kConcurrent;}, std::chrono::seconds(60));
            int missing = 0, dup = 0;
            {
                std::lock_guard<std::mutex> _(mutex);
                for (int x : seen) {
                    if (x == 0) {
                        ++missing;
                    } else if (x > 1) {
                        ++dup;
                    }
                }
            }
            check(all && missing == 0, "json_rest client: all " + std::to_string(kConcurrent) + " concurrent responses arrived (missing " + std::to_string(missing) + ")" + tag);
            check(dup == 0, "json_rest client: no duplicate response" + tag);
            check(wrong.load() == 0, "json_rest client: response content matches request" + tag);
        }

        //---- 2. sequential round trips
        {
            constexpr int kRounds = 300;
            std::vector<double> us;
            us.reserve(kRounds);
            bool ok = true;
            for (int i = 0; i < kRounds && ok; ++i) {
                int64_t seq = kConcurrent + i;
                int64_t before = received.load();
                auto t0 = std::chrono::steady_clock::now();
                fire(typename M::template Key<ThReq> {ThReq {seq, "s"}});
                ok = waitUntil([&]() {return received.load() > before && lastSeq.load(std::memory_order_acquire) == seq;}, std::chrono::seconds(5));
                us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()-t0).count());
            }
            check(ok, "json_rest client: sequential round trips all answered" + tag);
            std::sort(us.begin(), us.end());
            auto pct = [&](double p) {return us[std::min(us.size()-1, static_cast<std::size_t>(p*us.size()))];};
            std::cout << "      round trip us: p50=" << pct(0.5) << " p99=" << pct(0.99) << " max=" << us.back() << tag << std::endl;
            //informational bound only: generous for loaded / sanitizer builds
            check(pct(0.5) < 50000.0, "json_rest client: p50 round trip sane" + tag);
        }
    }

}

int main(int argc, char **argv) {
    int port = (argc >= 2) ? std::atoi(argv[1]) : 39871;
    runFor<false>(port);
    runFor<true>(port + 1);
    if (g_failures == 0) {
        std::cout << "ALL PASSED" << std::endl;
        std::_Exit(0);
    }
    std::cout << g_failures << " FAILURE(S)" << std::endl;
    std::_Exit(1);
}
