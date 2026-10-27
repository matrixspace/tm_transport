//Functional test driver for the aeron_ipc transport.
//
//  aeron_ipc_test pub <locator> <count> <rateHz> [payloadBytes] [topic[,topic...]] [lingerMs]
//      publishes <count> messages, topics used round-robin; rateHz 0 = as fast as possible
//      content = [uint64 seq][int64 send ns][pattern bytes (seq+i)&0xff]
//  aeron_ipc_test sub <locator> <seconds> [topic spec] [slowUs]
//      topic spec: "-" no selection, "r/<regex>/" regex, anything else exact
//      slowUs > 0 sleeps that long in every callback
//  aeron_ipc_test oversized <locator>
//      waits for readers, publishes one slotSize+1 byte message, then 1000 normal ones
//
//Each summary is one line of key=value pairs (PUBDONE / PUBSTATS / SUBDONE / OVERSIZED)
//so that run_tests.sh can grep it. All times use CLOCK_MONOTONIC, shared by all processes.
#include <tm_kit/transport/aeron_ipc/AeronIpcComponent.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace dev::cd606::tm;
using namespace dev::cd606::tm::transport;

namespace {
    std::atomic<bool> g_stop {false};
    void onSignal(int) {
        g_stop = true;
    }
    int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }
    int64_t cpuNs() {
        timespec ts;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        return static_cast<int64_t>(ts.tv_sec)*1000000000LL+ts.tv_nsec;
    }
    std::vector<std::string> splitTopics(std::string const &s) {
        std::vector<std::string> ret;
        std::stringstream ss(s);
        std::string t;
        while (std::getline(ss, t, ',')) {
            ret.push_back(t);
        }
        if (ret.empty()) {
            ret.push_back("t");
        }
        return ret;
    }
    void fillContent(std::string &buf, uint64_t seq, int64_t t) {
        std::memcpy(buf.data(), &seq, 8);
        std::memcpy(buf.data()+8, &t, 8);
        for (std::size_t ii=16; ii<buf.size(); ++ii) {
            buf[ii] = static_cast<char>((seq+ii) & 0xff);
        }
    }
    bool checkContent(std::string const &buf, uint64_t seq) {
        for (std::size_t ii=16; ii<buf.size(); ++ii) {
            if (buf[ii] != static_cast<char>((seq+ii) & 0xff)) {
                return false;
            }
        }
        return true;
    }
    void printPubStats(aeron_ipc::AeronIpcComponent &c, ConnectionLocator const &l) {
        auto s = c.aeron_ipc_publisherStats(l);
        if (!s) {
            std::cout << "PUBSTATS none" << std::endl;
            return;
        }
        std::cout << "PUBSTATS published=" << s->published
            << " notConnected=" << s->notConnected
            << " backPressured=" << s->backPressured
            << " adminAction=" << s->adminAction
            << " oversized=" << s->oversized
            << " failed=" << s->failed << std::endl;
    }

    int runPub(ConnectionLocator const &l, uint64_t count, uint64_t rateHz, std::size_t payload
        , std::string const &topicList, uint64_t lingerMs) {
        aeron_ipc::AeronIpcComponent c;
        auto pub = c.aeron_ipc_getPublisher(l);
        auto topics = splitTopics(topicList);
        payload = std::max<std::size_t>(payload, 16);
        std::string buf(payload, '\0');
        int64_t periodNs = (rateHz == 0) ? 0 : static_cast<int64_t>(1000000000ull/rateHz);
        int64_t start = nowNs();
        std::cout << "PUBSTART startNs=" << start << " rateHz=" << rateHz << std::endl;
        int64_t next = start;
        int64_t maxCallNs = 0;
        uint64_t slowCalls = 0;  //calls longer than 10us
        uint64_t lateSlots = 0;  //send slots missed by more than one period
        uint64_t sent = 0;
        for (uint64_t seq=0; seq<count && !g_stop; ++seq) {
            if (periodNs > 0) {
                int64_t n = nowNs();
                if (n > next+periodNs) {
                    ++lateSlots;
                }
                while (nowNs() < next) {
                }
                next += periodNs;
            }
            int64_t t = nowNs();
            fillContent(buf, seq, t);
            pub(basic::ByteDataWithTopic {topics[seq%topics.size()], buf});
            int64_t d = nowNs()-t;
            maxCallNs = std::max(maxCallNs, d);
            if (d > 10000) {
                ++slowCalls;
            }
            ++sent;
        }
        int64_t elapsed = nowNs()-start;
        std::cout << "PUBDONE count=" << sent << " elapsedMs=" << (elapsed/1000000)
            << " achievedHz=" << static_cast<uint64_t>(sent*1e9/std::max<int64_t>(elapsed,1))
            << " maxCallUs=" << (maxCallNs/1000) << " slowCalls=" << slowCalls
            << " lateSlots=" << lateSlots << std::endl;
        printPubStats(c, l);
        //give readers time to drain before the publication (and maybe the driver) goes away
        std::this_thread::sleep_for(std::chrono::milliseconds(lingerMs));
        return 0;
    }

    int runSub(ConnectionLocator const &l, double seconds, std::string const &topic, uint64_t slowUs) {
        aeron_ipc::AeronIpcComponent c;
        std::mutex mutex;
        uint64_t received = 0;
        uint64_t firstSeq = UINT64_MAX, lastSeq = 0, gaps = 0, missing = 0, reorders = 0;
        uint64_t badContent = 0, badTopic = 0;
        int64_t firstLatNs = -1, maxLatNs = 0, firstRecvNs = -1;
        bool haveLast = false;
        std::map<std::string, uint64_t> perTopic;
        std::variant<aeron_ipc::AeronIpcComponent::NoTopicSelection, std::string, std::regex> sel;
        std::optional<std::regex> re;
        if (topic == "-") {
            sel = aeron_ipc::AeronIpcComponent::NoTopicSelection {};
        } else if (topic.size() > 3 && topic.compare(0, 2, "r/") == 0 && topic.back() == '/') {
            re = std::regex(topic.substr(2, topic.size()-3));
            sel = *re;
        } else {
            sel = topic;
        }
        c.aeron_ipc_addSubscriptionClient(l, sel, [&](basic::ByteDataWithTopic &&d) {
            int64_t now = nowNs();
            std::lock_guard<std::mutex> _(mutex);
            ++received;
            ++perTopic[d.topic];
            if (topic != "-") {
                bool ok = re ? std::regex_match(d.topic, *re) : (d.topic == topic);
                if (!ok) {
                    ++badTopic;
                }
            }
            if (d.content.size() < 16) {
                ++badContent;
                return;
            }
            uint64_t seq;
            int64_t t;
            std::memcpy(&seq, d.content.data(), 8);
            std::memcpy(&t, d.content.data()+8, 8);
            if (!checkContent(d.content, seq)) {
                ++badContent;
            }
            int64_t lat = now-t;
            maxLatNs = std::max(maxLatNs, lat);
            if (firstSeq == UINT64_MAX) {
                firstSeq = seq;
                firstLatNs = lat;
                firstRecvNs = now;
            }
            if (haveLast) {
                if (seq <= lastSeq) {
                    //a restarted publisher begins again from 0
                    ++reorders;
                } else if (seq != lastSeq+1) {
                    ++gaps;
                    missing += seq-lastSeq-1;
                }
            }
            lastSeq = seq;
            haveLast = true;
            if (slowUs > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(slowUs));
            }
        });
        int64_t wallStart = nowNs();
        int64_t cpuStart = cpuNs();
        auto end = std::chrono::steady_clock::now()+std::chrono::duration<double>(seconds);
        while (!g_stop && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        double cpuPct = 100.0*(cpuNs()-cpuStart)/std::max<int64_t>(nowNs()-wallStart, 1);
        auto s = c.aeron_ipc_subscriberStats(l);
        std::lock_guard<std::mutex> _(mutex);
        std::cout << "SUBDONE received=" << received
            << " firstSeq=" << (firstSeq == UINT64_MAX ? -1 : static_cast<int64_t>(firstSeq))
            << " lastSeq=" << lastSeq
            << " gaps=" << gaps << " missing=" << missing << " reorders=" << reorders
            << " badContent=" << badContent << " badTopic=" << badTopic
            << " firstLatUs=" << (firstLatNs < 0 ? -1 : firstLatNs/1000)
            << " firstRecvNs=" << firstRecvNs
            << " maxLatUs=" << (maxLatNs/1000)
            << " cpuPct=" << static_cast<int>(cpuPct);
        if (s) {
            std::cout << " imagesJoined=" << s->imagesJoined << " skippedOnJoin=" << s->skippedOnJoin
                << " clientRebuilds=" << s->clientRebuilds << " malformed=" << s->malformed;
        }
        std::cout << " topics=";
        bool first = true;
        for (auto const &item : perTopic) {
            std::cout << (first ? "" : ",") << item.first << ":" << item.second;
            first = false;
        }
        std::cout << std::endl;
        //exit without running destructors so that a slow callback cannot block shutdown
        std::_Exit(0);
    }

    int runOversized(ConnectionLocator const &l) {
        aeron_ipc::AeronIpcComponent c;
        auto pub = c.aeron_ipc_getPublisher(l);
        //readers join from latest, let them attach first
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::size_t slot = std::stoul(l.query("slotSize", "1024"));
        std::string big(slot, 'x');
        pub(basic::ByteDataWithTopic {"t", big});
        std::string buf(64, '\0');
        for (uint64_t seq=0; seq<1000; ++seq) {
            fillContent(buf, seq, nowNs());
            pub(basic::ByteDataWithTopic {"t", buf});
        }
        printPubStats(c, l);
        auto s = c.aeron_ipc_publisherStats(l);
        bool ok = (s && s->oversized == 1 && s->published == 1000);
        std::cout << (ok ? "OVERSIZED ok" : "OVERSIZED fail") << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return ok ? 0 : 1;
    }
}

int main(int argc, char **argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    if (argc < 3) {
        std::cerr << "usage: see the comment at the top of aeron_ipc_test.cpp\n";
        return 2;
    }
    std::string mode = argv[1];
    try {
        //accepts the full channel string as well as a bare locator
        std::string spec = argv[2];
        std::string const prefix = "aeron_ipc://";
        if (spec.compare(0, prefix.length(), prefix) == 0) {
            spec = spec.substr(prefix.length());
        }
        auto l = ConnectionLocator::parse(spec);
        if (mode == "pub" && argc >= 5) {
            return runPub(l, std::stoull(argv[3]), std::stoull(argv[4])
                , (argc > 5) ? std::stoul(argv[5]) : 64
                , (argc > 6) ? argv[6] : "t"
                , (argc > 7) ? std::stoull(argv[7]) : 500);
        } else if (mode == "sub" && argc >= 4) {
            return runSub(l, std::stod(argv[3])
                , (argc > 4) ? argv[4] : "-"
                , (argc > 5) ? std::stoull(argv[5]) : 0);
        } else if (mode == "oversized") {
            return runOversized(l);
        }
    } catch (std::exception const &ex) {
        std::cerr << "ERROR " << ex.what() << std::endl;
        return 1;
    }
    std::cerr << "bad arguments\n";
    return 2;
}
