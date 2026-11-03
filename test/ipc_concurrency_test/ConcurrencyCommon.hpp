#ifndef TM_KIT_TRANSPORT_TEST_IPC_CONCURRENCY_COMMON_HPP_
#define TM_KIT_TRANSPORT_TEST_IPC_CONCURRENCY_COMMON_HPP_

//Payload format, option parsing and the per-reader checker shared by the
//ipc_concurrency_test modes.
//
//Every message is [PayloadHeader][body]. The header carries the writer id, the
//per-writer sequence number, the total length and an FNV-1a checksum over the
//first header fields plus the body; the body is a pattern derived from
//(writer, seq, offset). The topic is "w<writer>". A reader can therefore detect:
//  corrupt  : torn / mixed message (bad magic, length or checksum)
//  topicBad : intact content delivered under another writer's topic
//  ooo      : per-writer sequence went backwards
//  dup      : the same (writer, seq) delivered twice
//Lossy transports may skip messages; gaps are reported but are not an error.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ipc_concurrency {

    using Options = std::unordered_map<std::string, std::string>;

    inline Options parseOptions(int argc, char **argv) {
        Options o;
        for (int ii=1; ii<argc; ++ii) {
            std::string a = argv[ii];
            if (a.rfind("--", 0) != 0) {
                continue;
            }
            auto eq = a.find('=');
            if (eq == std::string::npos) {
                o[a.substr(2)] = "true";
            } else {
                o[a.substr(2, eq-2)] = a.substr(eq+1);
            }
        }
        return o;
    }
    inline std::string opt(Options const &o, std::string const &k, std::string const &def) {
        auto iter = o.find(k);
        return (iter == o.end()) ? def : iter->second;
    }
    inline uint64_t optNum(Options const &o, std::string const &k, uint64_t def) {
        auto iter = o.find(k);
        return (iter == o.end()) ? def : std::stoull(iter->second);
    }
    inline int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }

    constexpr uint32_t kMagic = 0x43435431; //"CCT1"
    struct PayloadHeader {
        uint32_t magic;
        uint32_t writer;
        uint64_t seq;
        uint32_t len;      //total length including the header
        uint32_t checksum; //FNV-1a over magic,writer,seq,len and the body
    };
    static_assert(sizeof(PayloadHeader) == 24, "unexpected PayloadHeader layout");

    inline uint32_t fnv1a(uint32_t h, void const *p, std::size_t n) {
        auto const *b = static_cast<unsigned char const *>(p);
        for (std::size_t ii=0; ii<n; ++ii) {
            h ^= b[ii];
            h *= 16777619u;
        }
        return h;
    }
    inline uint32_t checksumOf(PayloadHeader const &h, char const *body, std::size_t bodyLen) {
        uint32_t c = 2166136261u;
        c = fnv1a(c, &h, offsetof(PayloadHeader, checksum));
        return fnv1a(c, body, bodyLen);
    }
    inline std::string topicFor(uint32_t writer) {
        return "w"+std::to_string(writer);
    }
    //len is clamped to at least the header size
    inline void makePayload(std::string &out, uint32_t writer, uint64_t seq, std::size_t len) {
        if (len < sizeof(PayloadHeader)) {
            len = sizeof(PayloadHeader);
        }
        out.resize(len);
        char *body = out.data()+sizeof(PayloadHeader);
        std::size_t bodyLen = len-sizeof(PayloadHeader);
        for (std::size_t ii=0; ii<bodyLen; ++ii) {
            body[ii] = static_cast<char>((writer*131u+seq*31u+ii) & 0xff);
        }
        PayloadHeader h {kMagic, writer, seq, static_cast<uint32_t>(len), 0};
        h.checksum = checksumOf(h, body, bodyLen);
        std::memcpy(out.data(), &h, sizeof(h));
    }
    //payload length for (writer, seq), varied so that torn writes of different
    //sizes overlap inside one slot; maxLen is topic+payload capacity minus topic
    inline std::size_t lengthFor(uint32_t writer, uint64_t seq, std::size_t maxLen) {
        std::size_t span = (maxLen > sizeof(PayloadHeader)) ? (maxLen-sizeof(PayloadHeader)+1) : 1;
        return sizeof(PayloadHeader)+((seq*2654435761ULL+writer*40503ULL) % span);
    }

    //header, length and checksum check only (no per-writer ordering)
    inline bool validPayload(std::string const &content) {
        if (content.size() < sizeof(PayloadHeader)) {
            return false;
        }
        PayloadHeader h;
        std::memcpy(&h, content.data(), sizeof(h));
        return h.magic == kMagic && h.len == content.size()
            && h.checksum == checksumOf(h, content.data()+sizeof(h), content.size()-sizeof(h));
    }

    struct Totals {
        uint64_t received = 0;
        uint64_t corrupt = 0;
        uint64_t topicBad = 0;
        uint64_t ooo = 0;
        uint64_t dup = 0;
        uint64_t unknownWriter = 0;
        uint64_t gaps = 0;
        bool clean() const {
            return corrupt == 0 && topicBad == 0 && ooo == 0 && dup == 0 && unknownWriter == 0;
        }
        std::string str() const {
            std::ostringstream oss;
            oss << "received=" << received << " corrupt=" << corrupt << " topicBad=" << topicBad
                << " ooo=" << ooo << " dup=" << dup << " unknownWriter=" << unknownWriter
                << " gaps=" << gaps;
            return oss.str();
        }
    };

    //One checker per subscription client. Callbacks of one client come from one
    //transport thread, but the mutex keeps the checker safe if they do not.
    class Checker {
    private:
        struct PerWriter {
            bool have = false;
            uint64_t last = 0;
            uint64_t received = 0;
            uint64_t gaps = 0;
        };
        std::mutex mutex_;
        std::vector<PerWriter> writers_;
    public:
        uint64_t received = 0;
        uint64_t corrupt = 0;
        uint64_t topicBad = 0;
        uint64_t ooo = 0;
        uint64_t dup = 0;
        uint64_t unknownWriter = 0;

        explicit Checker(std::size_t writerCount) : writers_(writerCount) {}

        void on(std::string const &topic, std::string const &content) {
            std::lock_guard<std::mutex> _(mutex_);
            ++received;
            if (content.size() < sizeof(PayloadHeader)) {
                ++corrupt;
                return;
            }
            PayloadHeader h;
            std::memcpy(&h, content.data(), sizeof(h));
            if (h.magic != kMagic || h.len != content.size()
                || h.checksum != checksumOf(h, content.data()+sizeof(h), content.size()-sizeof(h))) {
                ++corrupt;
                return;
            }
            if (h.writer >= writers_.size()) {
                ++unknownWriter;
                return;
            }
            if (topic != topicFor(h.writer)) {
                ++topicBad;
            }
            auto &w = writers_[h.writer];
            ++w.received;
            if (w.have) {
                if (h.seq == w.last) {
                    ++dup;
                } else if (h.seq < w.last) {
                    ++ooo;
                } else if (h.seq != w.last+1) {
                    ++w.gaps;
                }
            }
            if (!w.have || h.seq > w.last) {
                w.last = h.seq;
            }
            w.have = true;
        }
        bool clean() {
            std::lock_guard<std::mutex> _(mutex_);
            return corrupt == 0 && topicBad == 0 && ooo == 0 && dup == 0 && unknownWriter == 0;
        }
        std::string summary() {
            std::lock_guard<std::mutex> _(mutex_);
            std::ostringstream oss;
            uint64_t gaps = 0;
            for (auto const &w : writers_) {
                gaps += w.gaps;
            }
            oss << "received=" << received << " corrupt=" << corrupt << " topicBad=" << topicBad
                << " ooo=" << ooo << " dup=" << dup << " unknownWriter=" << unknownWriter
                << " gaps=" << gaps << " perWriter=";
            for (std::size_t ii=0; ii<writers_.size(); ++ii) {
                oss << (ii ? "," : "") << writers_[ii].received;
            }
            return oss.str();
        }
        uint64_t receivedFrom(std::size_t writer) {
            std::lock_guard<std::mutex> _(mutex_);
            return writers_[writer].received;
        }
        uint64_t receivedCount() {
            std::lock_guard<std::mutex> _(mutex_);
            return received;
        }
        void addTo(Totals &t) {
            std::lock_guard<std::mutex> _(mutex_);
            t.received += received;
            t.corrupt += corrupt;
            t.topicBad += topicBad;
            t.ooo += ooo;
            t.dup += dup;
            t.unknownWriter += unknownWriter;
            for (auto const &w : writers_) {
                t.gaps += w.gaps;
            }
        }
    };

}

#endif
