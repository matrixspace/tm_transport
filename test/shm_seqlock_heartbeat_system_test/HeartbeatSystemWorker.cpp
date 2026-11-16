#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/basic/TrivialBoostLoggingComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>
#include <tm_kit/transport/BoostUUIDComponent.hpp>
#include <tm_kit/transport/HeartbeatAndAlertComponent.hpp>
#include <tm_kit/transport/HeartbeatMessageToRemoteFacilityCommand.hpp>
#include <tm_kit/transport/MultiTransportBroadcastListenerManagingUtils.hpp>

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>

using namespace dev::cd606::tm;
using namespace std::chrono_literals;
namespace
{
    using Env = infra::Environment<
        infra::CheckTimeComponent<false>,
        infra::TrivialExitControlComponent,
        basic::TrivialBoostLoggingComponent,
        basic::real_time_clock::ClockComponent,
        transport::BoostUUIDComponent,
        transport::HeartbeatAndAlertComponent,
        transport::shm_seqlock::ShmSeqlockComponent>;
    using M = infra::RealTimeApp<Env>;
    using R = infra::AppRunner<M>;
    using HB = transport::HeartbeatMessage;
    using Converter = transport::HeartbeatMessageToRemoteFacilityCommandConverter<std::tuple<>, std::tuple<int>>;
    std::mutex outMutex;
    void emit(std::string const &line)
    {
        std::lock_guard<std::mutex> l(outMutex);
        std::cout << line << std::endl;
    }
    std::string hex(std::string const &s)
    {
        std::string r;
        char const *digits = "0123456789abcdef";
        for (unsigned char c : s) {
            r += digits[c >> 4];
            r += digits[c & 15];
        }
        return r.empty() ? "-" : r;
    }
    std::string unhex(std::string const &s)
    {
        if (s == "-")
            return {};
        if (s.size() % 2)
            throw std::runtime_error("odd hex length");
        std::string r;
        for (std::size_t k = 0; k < s.size(); k += 2)
            r += static_cast<char>(std::stoul(s.substr(k, 2), nullptr, 16));
        return r;
    }
    transport::ConnectionLocator locator(std::string const &channel)
    {
        auto c = transport::parseMultiTransportBroadcastChannel(channel);
        if (!c || std::get<0>(*c) != transport::MultiTransportBroadcastListenerConnectionType::ShmSeqlock)
            throw std::runtime_error("shm_seqlock channel required");
        return std::get<1>(*c);
    }
    void actions(Converter &converter, std::variant<HB, basic::VoidStruct> data)
    {
        auto result = converter(std::make_tuple(std::chrono::system_clock::now(), std::move(data)));
        for (auto const &a : result) {
            auto const &action = std::get<1>(a)[0];
            emit(action.actionType == transport::MultiTransportRemoteFacilityActionType::Register ? "REGISTER" : "DEREGISTER");
        }
    }
} // namespace
int main(int argc, char **argv)
{
    try {
        if (argc < 4)
            throw std::runtime_error("worker service|client hb-channel data-channel [timer-ms] [first-unlocked]");
        auto role = std::string(argv[1]);
        auto hbChannel = std::string(argv[2]);
        auto dataChannel = std::string(argv[3]);
        auto hbLocator = locator(hbChannel);
        struct CallbackGate {
            std::mutex mutex;
            std::condition_variable condition;
            bool blocked = false;
            void release()
            {
                std::lock_guard<std::mutex> lock(mutex);
                blocked = false;
                condition.notify_all();
            }
        } gate;
        Converter converter(std::regex("system-service"), {}, {"rpc"}, 1s);
        auto envOwner = std::make_unique<Env>();
        auto &env = *envOwner;
        R runner(&env);
        struct StopBeforeRunner {
            std::unique_ptr<Env> &owner;
            CallbackGate &gate;
            ~StopBeforeRunner()
            {
                gate.release();
                owner.reset();
            }
        } stopBeforeRunner{envOwner, gate};
        std::function<void(basic::ByteDataWithTopic &&)> publisher;
        std::function<void(basic::ByteDataWithTopic &&)> originalPublisher;
        auto finishTime = std::chrono::steady_clock::now();
        bool unlockedRisk = argc > 5 && std::string(argv[5]) == "first-unlocked";
        if (role == "service") {
            // Optional risk mode creates the cached sender before heartbeat's
            // later writerLock=true locator; this cannot repair first creation.
            if (unlockedRisk) {
                publisher = env.shm_seqlock_getPublisher(hbLocator.removeProperty("writerLock"));
                emit("FIRST_WRITER_LOCK false");
            }
            transport::initializeHeartbeatAndAlertComponent(&env, "system-service", hbChannel, transport::UserToWireHook{[](basic::ByteData &&data) {
                emit("WIRE_SIZE " + std::to_string(data.content.size() + std::string("heartbeat").size()));
                return std::move(data);
            }});
            if (unlockedRisk)
                emit("LATE_WRITER_LOCK " + hbLocator.query("writerLock", "false"));
            env.setStatus("program", HB::Status::Good, "ready");
            env.addBroadcastChannel("business", dataChannel);
            env.addFacilityChannel("rpc", "socket_rpc://localhost:12345");
            publisher = env.shm_seqlock_getPublisher(locator(dataChannel));
            originalPublisher = publisher;
            auto timerMs = argc > 4 ? std::stoul(argv[4]) : 0;
            if (timerMs) {
                transport::attachHeartbeatAndAlertComponent(runner, &env, "heartbeat", 100ms, std::chrono::milliseconds(timerMs));
                finishTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(timerMs) + 200ms;
            }
        } else if (role == "client") {
            auto heartbeat = transport::MultiTransportBroadcastListenerManagingUtils<R>::oneBroadcastListener<HB>(runner, "hb", hbChannel, "heartbeat");
            auto monitor = M::simpleExporter<HB>([&](M::InnerData<HB> &&d) {
                {
                    std::unique_lock<std::mutex> lock(gate.mutex);
                    if (gate.blocked) {
                        emit("BLOCKED_HB");
                        gate.condition.wait(lock, [&] { return !gate.blocked; });
                    }
                }
                auto const &h = d.timedData.value;
                auto status = h.status("program");
                auto channels = h.broadcastChannels().find("business");
                emit(
                    "HB " + h.uuidStr() + " " + std::to_string(h.pid()) + " " + h.senderDescription() + " " +
                    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(h.heartbeatTime().time_since_epoch()).count()) + " " +
                    (!status ? "missing" : HB::statusString(status->status)) + " " + std::to_string(channels == h.broadcastChannels().end() ? 0 : channels->second.size()) + " " + hex(h.host()));
                actions(converter, HB(h));
            });
            runner.registerExporter("monitor", monitor);
            runner.exportItem(monitor, heartbeat.clone());
            auto business = transport::MultiTransportBroadcastListenerManagingUtils<R>::setupBroadcastListenerThroughHeartbeat<std::string>(
                runner, std::move(heartbeat), std::regex("system-service"), "business", "business", "discover");
            auto sink = M::simpleExporter<std::string>([](M::InnerData<std::string> &&d) { emit("DATA " + hex(d.timedData.value)); });
            runner.registerExporter("business-sink", sink);
            runner.exportItem(sink, std::move(business));
            env.shm_seqlock_addSubscriptionClient(hbLocator, std::string("heartbeat"), [](basic::ByteDataWithTopic &&d) {
                HB h;
                if (!h.ParseFromString(d.content))
                    emit("BAD_HB");
            });
            env.shm_seqlock_addSubscriptionClient(hbLocator, std::string("alert"), [](basic::ByteDataWithTopic &&d) {
                transport::AlertMessage a;
                if (!a.ParseFromString(d.content)) {
                    emit("BAD_ALERT");
                    return;
                }
                emit("ALERT " + a.senderDescription() + " " + std::to_string(a.pid()) + " " + hex(a.message()));
            });
            env.shm_seqlock_addSubscriptionClient(hbLocator, std::string("custom"), [](basic::ByteDataWithTopic &&d) {
                using Custom = transport::CustomDataMessageThroughAlertChannel<std::string>;
                auto c = basic::bytedata_utils::RunCBORDeserializer<Custom>::apply(d.content, 0);
                if (!c || std::get<1>(*c) != d.content.size()) {
                    emit("BAD_CUSTOM");
                    return;
                }
                auto const &v = std::get<0>(*c);
                emit("CUSTOM " + v.senderDescription + " " + std::to_string(v.pid) + " " + hex(v.data));
            });
        } else
            throw std::runtime_error("unknown role");
        runner.finalize();
        emit("READY");
        for (std::string line; std::getline(std::cin, line);) {
            std::istringstream input(line);
            std::string command, value;
            input >> command >> value;
            if (command == "HB") {
                env.publishHeartbeat("heartbeat");
                emit("SENT_HB");
            } else if (command == "ALERT") {
                env.sendAlert("alert", infra::LogLevel::Warning, unhex(value));
                emit("SENT_ALERT");
            } else if (command == "CUSTOM") {
                env.sendCustomMessage("custom", unhex(value));
                emit("SENT_CUSTOM");
            } else if (command == "DATA") {
                publisher({"business", unhex(value)});
                emit("SENT_DATA");
            } else if (command == "STATUS") {
                env.setStatus("program", HB::parseStatus(value), value);
                emit("STATUS_SET");
            } else if (command == "CHANNEL") {
                env.addBroadcastChannel("business", value);
                publisher = env.shm_seqlock_getPublisher(locator(value));
                emit("CHANNEL_ADDED");
            } else if (command == "OLD_DATA") {
                originalPublisher({"business", unhex(value)});
                emit("SENT_OLD_DATA");
            } else if (command == "GROW") {
                env.setStatus("growth", HB::Status::Good, std::string(std::stoul(value), 'x'));
                emit("GROWN");
            } else if (command == "CHECK") {
                actions(converter, basic::VoidStruct{});
                emit("CHECKED");
            } else if (command == "BLOCK") {
                std::lock_guard<std::mutex> lock(gate.mutex);
                gate.blocked = true;
                emit("BLOCK_ARMED");
            } else if (command == "RELEASE") {
                gate.release();
                emit("RELEASED");
            } else if (command == "RISK_BURST") {
                if (!unlockedRisk)
                    throw std::runtime_error("risk mode required");
                auto count = std::stoul(value);
                std::atomic<bool> go{false};
                std::thread dataThread([&] {
                    while (!go.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    for (std::size_t k = 0; k < count; ++k) {
                        std::string payload = "risk/" + std::to_string(k) + "/";
                        for (unsigned b = 0; b < 512; ++b)
                            payload += static_cast<char>((k * 13 + b * 7) & 255);
                        publisher({"business", std::move(payload)});
                    }
                });
                std::thread heartbeatThread([&] {
                    while (!go.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    for (std::size_t k = 0; k < count / 64 + 1; ++k)
                        env.publishHeartbeat("heartbeat");
                });
                go.store(true, std::memory_order_release);
                dataThread.join();
                heartbeatThread.join();
                emit("RISK_SENT " + std::to_string(count));
            } else if (command == "STATS") {
                for (auto const &p : env.shm_seqlock_publisherStats())
                    emit("PUB " + p.first.identifier() + " " + std::to_string(p.second.published) + " " + std::to_string(p.second.rejected));
                emit("STATS_END");
            } else if (command == "QUIT")
                break;
            else
                throw std::runtime_error("unknown command");
        }
        // All callback owners remain alive until transport callbacks stop. A
        // finite graph timer is allowed to finish before its runner is destroyed.
        if (finishTime > std::chrono::steady_clock::now())
            std::this_thread::sleep_until(finishTime);
        if (role == "service")
            runner.controlAll("__heartbeat_clock_importer", "stop", {});
        emit("DONE");
    } catch (std::exception const &e) {
        emit(std::string("ERROR ") + e.what());
        return 1;
    }
}
