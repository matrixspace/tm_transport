//Test 8: drives aeron_ipc through the MultiTransport channel-string utilities
//inside a real TM Kit graph, using a component-only environment.
//
//  aeron_ipc_graph_test pub <channel> <count> <topic[,topic...]>   (topics used round-robin)
//  aeron_ipc_graph_test sub <channel> <topic spec> <seconds>
//      topic spec is passed to oneByteDataBroadcastListener as is: an exact topic,
//      r/<regex>/ for a regex, or "" for no topic selection
//      prints GRAPHSUB received=<n> topics=<topic>:<count>,...
//
//channel is e.g. aeron_ipc://::::graph_test[slotSize=256]
#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/TerminationController.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/basic/SpdLoggingComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>
#include <tm_kit/transport/MultiTransportBroadcastPublisherManagingUtils.hpp>
#include <tm_kit/transport/MultiTransportBroadcastListenerManagingUtils.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

using namespace dev::cd606::tm;

namespace {
    // clang-format off
    using Env = infra::Environment<
        infra::CheckTimeComponent<false>
        , infra::TrivialExitControlComponent
        , basic::TimeComponentEnhancedWithSpdLogging<basic::real_time_clock::ClockComponent, false>
        , transport::aeron_ipc::AeronIpcComponent
    >;
    // clang-format on
    using M = infra::RealTimeApp<Env>;
    using R = infra::AppRunner<M>;

    class CountingSource final : public M::AbstractImporter<basic::ByteDataWithTopic> {
    private:
        uint64_t count_;
        std::vector<std::string> topics_;
        std::thread th_;
    public:
        CountingSource(uint64_t count, std::string const &topics) : count_(count) {
            std::stringstream ss(topics);
            std::string t;
            while (std::getline(ss, t, ',')) {
                topics_.push_back(t);
            }
            if (topics_.empty()) {
                topics_.push_back("t");
            }
        }
        ~CountingSource() {
            if (th_.joinable()) {
                th_.join();
            }
        }
        virtual void start(Env *env) override final {
            th_ = std::thread([this,env]() {
                //readers join from latest, give them time to attach first
                std::this_thread::sleep_for(std::chrono::seconds(1));
                for (uint64_t ii=0; ii<count_; ++ii) {
                    this->publish(M::pureInnerData<basic::ByteDataWithTopic>(
                        env, basic::ByteDataWithTopic {topics_[ii%topics_.size()], std::to_string(ii)}
                    ));
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                std::cout << "GRAPHPUB done count=" << count_ << std::endl;
            });
        }
    };
}

int main(int argc, char **argv) {
    if (argc < 5) {
        std::cerr << "usage: aeron_ipc_graph_test pub <channel> <count> <topic> | sub <channel> <topic> <seconds>\n";
        return 2;
    }
    std::string mode = argv[1];
    std::string channel = argv[2];
    Env env;
    R r(&env);
    if (mode == "pub") {
        uint64_t count = std::stoull(argv[3]);
        auto source = M::importer(new CountingSource(count, argv[4]));
        auto sink = transport::MultiTransportBroadcastPublisherManagingUtils<R>::oneByteDataBroadcastPublisher(
            r, "publisher", channel);
        r.connect(r.importItem("source", source), sink);
        r.finalize();
        std::this_thread::sleep_for(std::chrono::seconds(2)+std::chrono::microseconds(150*count));
        return 0;
    }
    if (mode == "sub") {
        std::string topic = argv[3];
        double seconds = std::stod(argv[4]);
        std::atomic<uint64_t> received {0};
        std::mutex mutex;
        std::map<std::string, uint64_t> perTopic;
        auto source = transport::MultiTransportBroadcastListenerManagingUtils<R>::oneByteDataBroadcastListener(
            r, "listener", channel, topic);
        auto sink = M::simpleExporter<basic::ByteDataWithTopic>(
            [&received,&mutex,&perTopic](M::InnerData<basic::ByteDataWithTopic> &&d) {
                std::lock_guard<std::mutex> _(mutex);
                ++perTopic[d.timedData.value.topic];
                ++received;
            }
        );
        r.connect(std::move(source), r.exporterAsSink("sink", sink));
        r.finalize();
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        std::lock_guard<std::mutex> _(mutex);
        std::cout << "GRAPHSUB received=" << received.load() << " topics=";
        bool first = true;
        for (auto const &item : perTopic) {
            std::cout << (first ? "" : ",") << item.first << ":" << item.second;
            first = false;
        }
        std::cout << std::endl;
        std::_Exit(0);
    }
    std::cerr << "bad mode\n";
    return 2;
}
