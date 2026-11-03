#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/basic/TrivialBoostLoggingComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>
#include <tm_kit/transport/MultiTransportBroadcastPublisherManagingUtils.hpp>
#include <tm_kit/transport/MultiTransportBroadcastListenerManagingUtils.hpp>
#include <tm_kit/transport/HeartbeatAndAlertComponent.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

using namespace dev::cd606::tm;
using Env = infra::Environment<
    infra::CheckTimeComponent<false>,
    infra::TrivialExitControlComponent,
    basic::TrivialBoostLoggingComponent,
    basic::real_time_clock::ClockComponent,
    transport::AllNetworkTransportComponents,
    transport::HeartbeatAndAlertComponent>;
using M = infra::RealTimeApp<Env>;
using R = infra::AppRunner<M>;

static std::string body(uint64_t n, std::size_t len)
{
    std::string x(len, '\0');
    uint64_t s = n + 0x9e3779b97f4a7c15ULL;
    for (auto &c : x) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        c = static_cast<char>(s);
    }
    return x;
}
static std::size_t msgSize(uint64_t n)
{
    static std::size_t const sizes[] = {0, 1, 7, 8, 31, 32, 63, 64, 65, 255, 256, 257, 1023, 1024, 4096};
    return sizes[n % (sizeof(sizes) / sizeof(sizes[0]))];
}
static void writeLine(std::string const &p, std::string const &s)
{
    std::ofstream f(p);
    f << s << std::endl;
}
static int pub(std::string const &channel, std::string const &ready, uint64_t count)
{
    Env env;
    R r(&env);
    std::atomic<bool> done{false};
    struct Source : M::AbstractImporter<basic::ByteDataWithTopic> {
        uint64_t count;
        std::string ready;
        std::atomic<bool> *done;
        Source(uint64_t n, std::string r, std::atomic<bool> *d)
            : count(n), ready(std::move(r)), done(d)
        {
        }
        void start(Env *e) override
        {
            std::thread([this, e] {
                //send only once the reader is up, and give subscription-based
                //transports (zeromq) time to connect before the first message
                while (access((ready + ".ready").c_str(), F_OK) != 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                for (uint64_t i = 0; i < count; ++i) {
                    basic::ByteDataWithTopic d{"p." + std::to_string(i), body(i, msgSize(i))};
                    this->publish(M::pureInnerData<basic::ByteDataWithTopic>(e, std::move(d)));
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                done->store(true, std::memory_order_release);
            }).detach();
        }
    };
    auto src = M::importer(new Source(count, ready, &done));
    auto p = transport::MultiTransportBroadcastPublisherManagingUtils<R>::oneByteDataBroadcastPublisher(r, "pub", channel);
    r.connect(r.importItem("source", src), p);
    r.finalize();
    //exit only after every message was handed to the transport, then let the
    //transport drain before the process goes away
    while (!done.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::fflush(nullptr);
    _Exit(0);
}
static int sub(std::string const &channel, std::string const &out, uint64_t count, uint64_t delay)
{
    Env env;
    R r(&env);
    std::mutex m;
    uint64_t received = 0, bad = 0, gaps = 0, ooo = 0, last = 0;
    bool have = false;
    auto sink = M::simpleExporter<basic::ByteDataWithTopic>([&](M::InnerData<basic::ByteDataWithTopic> &&d) {
        auto const &x = d.timedData.value;
        if (x.topic.rfind("p.", 0) != 0)
            return;
        uint64_t q = std::stoull(x.topic.substr(2));
        if (x.content != body(q, msgSize(q)))
            ++bad;
        if (have) {
            if (q <= last)
                ++ooo;
            else
                gaps += q - last - 1;
        } else
            have = true;
        last = q;
        ++received;
        if (delay)
            std::this_thread::sleep_for(std::chrono::microseconds(delay));
    });
    auto s = transport::MultiTransportBroadcastListenerManagingUtils<R>::oneByteDataBroadcastListener(r, "sub", channel);
    r.exportItem("sink", sink, std::move(s));
    r.finalize();
    writeLine(out + ".ready", "ready");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline && received < count)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    writeLine(out, "received=" + std::to_string(received) + " bad=" + std::to_string(bad) + " gaps=" + std::to_string(gaps) + " ooo=" + std::to_string(ooo));
    std::fflush(nullptr);
    _Exit((bad || ooo) ? 1 : 0);
}
int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    std::string mode = argv[1];
    if (mode == "pub" && argc >= 5)
        return pub(argv[2], argv[3], std::stoull(argv[4]));
    if (mode == "sub" && argc >= 5)
        return sub(argv[2], argv[3], std::stoull(argv[4]), argc > 5 ? std::stoull(argv[5]) : 0);
    std::cerr << "usage: pub|sub channel ready-or-result count [delay-us]\n";
    return 2;
}
