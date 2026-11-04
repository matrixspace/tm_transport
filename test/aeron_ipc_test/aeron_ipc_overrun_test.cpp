//Verifies whether an aeron_ipc reader can see the term buffer change under it while
//its fragment handler is still running (untethered reader overrun).
//
//  aeron_ipc_overrun_test <dir> <tether:true|false> [stallMs=100] [rounds=5] [termLength=65536]
//
//The reader's WireToUserHook receives a ByteDataView that points straight into the
//shared term buffer. On selected messages the hook copies the view, sleeps stallMs
//(the reader thread is stalled inside aeron_image_poll), and copies the view again.
//Meanwhile the publisher keeps publishing at full speed (backPressure=drop).
//
//Counters in the RESULT line:
//  changed     stalls after which the view no longer held the bytes it held before
//  badContent  messages handed to the client whose content failed its pattern check
//  malformed   the transport's own malformed counter (topic length out of range)
#include <tm_kit/transport/aeron_ipc/AeronIpcComponent.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace dev::cd606::tm;
using namespace dev::cd606::tm::transport;

namespace {
    constexpr std::size_t kPayload = 200;
    void fill(std::string &b, uint64_t seq) {
        std::memcpy(b.data(), &seq, 8);
        for (std::size_t ii=8; ii<b.size(); ++ii) {
            b[ii] = static_cast<char>((seq*31+ii) & 0xff);
        }
    }
    bool check(std::string_view b) {
        if (b.size() != kPayload) {
            return false;
        }
        uint64_t seq;
        std::memcpy(&seq, b.data(), 8);
        for (std::size_t ii=8; ii<b.size(); ++ii) {
            if (b[ii] != static_cast<char>((seq*31+ii) & 0xff)) {
                return false;
            }
        }
        return true;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: aeron_ipc_overrun_test <dir> <tether:true|false> [stallMs] [rounds] [termLength]\n";
        return 2;
    }
    std::string dir = argv[1];
    std::string tether = argv[2];
    int stallMs = (argc > 3) ? std::stoi(argv[3]) : 100;
    int rounds = (argc > 4) ? std::stoi(argv[4]) : 5;
    std::string termLength = (argc > 5) ? argv[5] : "65536";

    auto l = ConnectionLocator::parse(
        "::::overrun[dir="+dir+",slotSize=256,termLength="+termLength+",tether="+tether+"]");

    aeron_ipc::AeronIpcComponent c;
    auto pub = c.aeron_ipc_getPublisher(l);

    std::atomic<uint64_t> received {0}, badContent {0}, changed {0}, stalls {0};
    std::atomic<bool> stalling {false};
    std::atomic<bool> stop {false};
    uint64_t nextStallAt = 2000;

    WireToUserHook hook {[&](basic::ByteDataView const &v) -> std::optional<basic::ByteData> {
        //runs on the reader thread, inside aeron_image_poll; v points into the term buffer
        auto n = received.load(std::memory_order_relaxed);
        if (static_cast<int>(stalls.load()) < rounds && n >= nextStallAt) {
            std::string before(v.content);
            stalling = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
            stalling = false;
            std::string after(v.content);
            if (before != after) {
                ++changed;
            }
            ++stalls;
            nextStallAt = n+2000;
            return basic::ByteData {std::move(after)};
        }
        return basic::ByteData {std::string(v.content)};
    }};

    c.aeron_ipc_addSubscriptionClient(l, aeron_ipc::AeronIpcComponent::NoTopicSelection {}
        , [&](basic::ByteDataWithTopic &&d) {
            ++received;
            if (!check(d.content)) {
                ++badContent;
            }
        }, hook);

    //optional fast reader in a separate component (= separate Aeron subscription): it keeps
    //min_sub_pos moving, so the publisher can pass the stalled reader once it is ejected
    bool fastReader = (argc > 6) ? (std::string(argv[6]) == "true") : false;
    std::atomic<uint64_t> fastReceived {0}, fastBad {0};
    std::unique_ptr<aeron_ipc::AeronIpcComponent> c2;
    if (fastReader) {
        c2 = std::make_unique<aeron_ipc::AeronIpcComponent>();
        c2->aeron_ipc_addSubscriptionClient(l, aeron_ipc::AeronIpcComponent::NoTopicSelection {}
            , [&](basic::ByteDataWithTopic &&d) {
                ++fastReceived;
                if (!check(d.content)) {
                    ++fastBad;
                }
            });
    }

    std::thread writer([&]() {
        std::string buf(kPayload, '\0');
        uint64_t seq = 0;
        while (!stop) {
            fill(buf, seq++);
            pub({"t", buf});
        }
    });

    auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10+rounds*(stallMs/100+1));
    while (static_cast<int>(stalls.load()) < rounds && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop = true;
    writer.join();

    auto ps = c.aeron_ipc_publisherStats(l);
    auto ss = c.aeron_ipc_subscriberStats(l);
    std::cout << "RESULT tether=" << tether << " stallMs=" << stallMs << " termLength=" << termLength
        << " stalls=" << stalls << " changed=" << changed << " received=" << received
        << " badContent=" << badContent
        << " overrun=" << (ss ? ss->overrun : 0)
        << " malformed=" << (ss ? ss->malformed : 0)
        << " imagesJoined=" << (ss ? ss->imagesJoined : 0)
        << " published=" << (ps ? ps->published : 0)
        << " backPressured=" << (ps ? ps->backPressured : 0)
        << " fastReader=" << (fastReader ? "true" : "false")
        << " fastReceived=" << fastReceived << " fastBad=" << fastBad
        << std::endl;
    return 0;
}
