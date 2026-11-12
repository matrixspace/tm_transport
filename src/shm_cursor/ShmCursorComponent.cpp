#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <linux/futex.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include <tm_kit/transport/shm_cursor/ShmCursorComponent.hpp>

#include "../ThreadCpuLeaseSupport.hpp"

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace shm_cursor {

    namespace {
        constexpr uint64_t SHM_CURSOR_MAGIC = 0x5255434d48534d54ULL; //"TMSHMCUR" in little endian
        constexpr uint32_t SHM_CURSOR_LAYOUT_VERSION = 2;
        constexpr uint32_t SLOT_FLAG_SKIP = 1;

        // Segment layout: one 640-byte header followed by slotCount slots of slotSize bytes.
        // Every group of fields lives on its own 128-byte block so that the
        // adjacent-line prefetcher does not couple them.
        struct ShmCursorHeader {
            alignas(128) std::atomic<uint64_t> magic;
            uint32_t layoutVersion;
            uint32_t slotSize;
            uint64_t slotCount;
            uint64_t headerSize;
            std::atomic<uint32_t> publisherWakes;
            // claim and commit are the indices (starting at 1, 0 means nothing yet)
            // of the latest claimed and the latest fully written message; they
            // share one cache line so a reader gets both with one miss
            alignas(128) std::atomic<uint64_t> claim;
            std::atomic<uint64_t> commit;
            // epoch is bumped on every publisher (re)start; epochStart is the first
            // index the current publisher incarnation writes
            alignas(128) std::atomic<uint64_t> epoch;
            std::atomic<uint64_t> epochStart;
            alignas(128) std::atomic<uint32_t> futexWord;
            alignas(128) std::atomic<uint32_t> waiters;
        };
        static_assert(sizeof(ShmCursorHeader) == 640, "unexpected shm cursor header size");
        static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "futex word must be a plain 32-bit word");
        static_assert(std::atomic<uint64_t>::is_always_lock_free, "64-bit atomics must be lock free for shared memory use");

        struct SlotHeader {
            uint32_t contentLen;
            uint32_t topicLen;
            uint32_t topicHash;
            uint32_t flags;
        };
        static_assert(sizeof(SlotHeader) == 16, "unexpected slot header size");

        inline uint32_t topicHash(std::string_view s) {
            uint32_t h = 2166136261u;
            for (char c : s) {
                h ^= static_cast<uint8_t>(c);
                h *= 16777619u;
            }
            return h;
        }

        inline void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
            _mm_pause();
#endif
        }

        inline long futexCall(std::atomic<uint32_t> *word, int op, uint32_t val, struct timespec const *ts) {
            // shared (not FUTEX_PRIVATE_FLAG) because waiters and wakers are in different processes
            return ::syscall(SYS_futex, reinterpret_cast<uint32_t *>(word), op, val, ts, nullptr, 0);
        }

        void validateIdentifier(std::string const &id) {
            if (id.empty() || id.length() > 200) {
                throw std::runtime_error("[ShmCursorComponent] segment name '" + id + "' must be 1-200 characters long");
            }
            for (char c : id) {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) {
                    throw std::runtime_error("[ShmCursorComponent] segment name '" + id + "' may only contain letters, digits, '_', '-' and '.'");
                }
            }
        }

        bool isPowerOfTwo(uint64_t x) {
            return x != 0 && (x & (x - 1)) == 0;
        }

        struct ShmCursorOptions {
            uint32_t slotSize;
            uint64_t slotCount;
            bool futex;
            uint64_t spinCount;
            uint64_t pauseAfter;
            uint32_t futexTimeoutMs;
            bool joinLatest;
            bool publisherLock;

            static ShmCursorOptions fromLocator(ConnectionLocator const &l) {
                ShmCursorOptions o;
                o.slotSize = static_cast<uint32_t>(std::stoul(l.query("slotSize", "1024")));
                o.slotCount = std::stoull(l.query("slotCount", "4096"));
                auto w = l.query("wait", "spin");
                if (w == "spin") {
                    o.futex = false;
                } else if (w == "futex") {
                    o.futex = true;
                } else {
                    throw std::runtime_error("[ShmCursorComponent] wait must be 'spin' or 'futex', got '" + w + "'");
                }
                o.spinCount = std::stoull(l.query("spinCount", "20000"));
                o.pauseAfter = std::stoull(l.query("pauseAfter", "100000"));
                o.futexTimeoutMs = static_cast<uint32_t>(std::stoul(l.query("futexTimeoutMs", "10")));
                if (o.futexTimeoutMs == 0) {
                    o.futexTimeoutMs = 1;
                }
                auto j = l.query("join", "latest");
                if (j == "latest") {
                    o.joinLatest = true;
                } else if (j == "next") {
                    o.joinLatest = false;
                } else {
                    throw std::runtime_error("[ShmCursorComponent] join must be 'latest' or 'next', got '" + j + "'");
                }
                auto pl = l.query("publisherLock", "false");
                if (pl == "true") {
                    o.publisherLock = true;
                } else if (pl == "false") {
                    o.publisherLock = false;
                } else {
                    throw std::runtime_error("[ShmCursorComponent] publisherLock must be 'true' or 'false', got '" + pl + "'");
                }
                if (o.slotSize < 64 || o.slotSize % 64 != 0 || o.slotSize > (1u << 30)) {
                    throw std::runtime_error("[ShmCursorComponent] slotSize must be a multiple of 64 between 64 and 2^30");
                }
                if (o.slotCount < 2 || !isPowerOfTwo(o.slotCount) || o.slotCount > (1ull << 30)) {
                    throw std::runtime_error("[ShmCursorComponent] slotCount must be a power of two between 2 and 2^30");
                }
                return o;
            }
        };

        std::string shmNameFor(std::string const &id) {
            return "/tm_shm_cursor." + id;
        }
        std::string shmPathFor(std::string const &id) {
            return "/dev/shm/tm_shm_cursor." + id;
        }
        std::string lockPathFor(std::string const &id) {
            return "/dev/shm/tm_shm_cursor." + id + ".lock";
        }
    }

    class ShmCursorSender {
    private:
        std::string id_;
        int lockFd_;
        int shmFd_;
        void *mem_;
        std::size_t memSize_;
        ShmCursorHeader *hdr_;
        char *slots_;
        uint32_t slotSize_;
        uint64_t mask_;
        uint32_t capacity_;
        uint64_t next_;
        bool wake_;
        bool lock_;
        std::atomic<bool> locked_;
        std::atomic<uint64_t> rejected_;

        void cleanup() {
            if (mem_ != nullptr) {
                ::munmap(mem_, memSize_);
                mem_ = nullptr;
            }
            if (shmFd_ >= 0) {
                ::close(shmFd_);
                shmFd_ = -1;
            }
            if (lockFd_ >= 0) {
                // closing the descriptor releases the flock
                ::close(lockFd_);
                lockFd_ = -1;
            }
        }
        [[noreturn]] void fail(std::string const &what) {
            int e = errno;
            cleanup();
            throw std::runtime_error("[ShmCursorComponent] publisher for '" + id_ + "': " + what + (e != 0 ? (std::string(": ") + std::strerror(e)) : std::string()));
        }
        void reject(std::size_t topicLen, std::size_t contentLen) {
            auto n = rejected_.fetch_add(1, std::memory_order_relaxed) + 1;
            // log the first rejection and then at powers of two to avoid flooding
            if (isPowerOfTwo(n)) {
                std::cerr << "[ShmCursorComponent] publisher for '" << id_ << "' dropped oversized message (topic "
                          << topicLen << " + content " << contentLen << " bytes > slot capacity " << capacity_
                          << "), total rejected so far: " << n << std::endl;
            }
        }

    public:
        ShmCursorSender(std::string const &id, ShmCursorOptions const &opt)
            : id_(id)
            , lockFd_(-1)
            , shmFd_(-1)
            , mem_(nullptr)
            , memSize_(0)
            , hdr_(nullptr)
            , slots_(nullptr)
            , slotSize_(opt.slotSize)
            , mask_(opt.slotCount - 1)
            , capacity_(opt.slotSize - static_cast<uint32_t>(sizeof(SlotHeader)))
            , next_(1)
            , wake_(opt.futex)
            , lock_(opt.publisherLock)
            , locked_(false)
            , rejected_(0) {
            validateIdentifier(id);
            errno = 0;
            lockFd_ = ::open(lockPathFor(id).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
            if (lockFd_ < 0) {
                fail("cannot open lock file " + lockPathFor(id));
            }
            // best effort: let readers running under other users probe the lock
            (void)::fchmod(lockFd_, 0666);
            // readers probe the lock with short LOCK_SH attempts, so retry for a while
            bool locked = false;
            for (int ii = 0; ii < 2000; ++ii) {
                if (::flock(lockFd_, LOCK_EX | LOCK_NB) == 0) {
                    locked = true;
                    break;
                }
                if (errno != EWOULDBLOCK) {
                    fail("flock failed");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!locked) {
                errno = 0;
                fail("another publisher process holds " + lockPathFor(id));
            }

            std::size_t const headerSize = sizeof(ShmCursorHeader);
            std::size_t const total = headerSize + static_cast<std::size_t>(opt.slotSize) * opt.slotCount;
            std::string const name = shmNameFor(id);

            bool reuse = false;
            shmFd_ = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
            if (shmFd_ < 0) {
                fail("shm_open failed");
            }
            struct stat st;
            if (::fstat(shmFd_, &st) != 0) {
                fail("fstat failed");
            }
            if (static_cast<std::size_t>(st.st_size) == total) {
                void *m = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, shmFd_, 0);
                if (m == MAP_FAILED) {
                    fail("mmap failed");
                }
                auto *h = reinterpret_cast<ShmCursorHeader *>(m);
                if (h->magic.load(std::memory_order_acquire) == SHM_CURSOR_MAGIC && h->layoutVersion == SHM_CURSOR_LAYOUT_VERSION && h->slotSize == opt.slotSize && h->slotCount == opt.slotCount && h->headerSize == headerSize) {
                    reuse = true;
                    mem_ = m;
                    memSize_ = total;
                } else {
                    ::munmap(m, total);
                }
            }
            if (!reuse) {
                // Layout differs (or segment is new): replace the object. Readers still
                // mapping the old object notice the inode change and re-attach.
                ::close(shmFd_);
                shmFd_ = -1;
                ::shm_unlink(name.c_str());
                errno = 0;
                shmFd_ = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
                if (shmFd_ < 0) {
                    fail("shm_open(O_EXCL) failed");
                }
                // best effort: readers map read-write (waiter count in futex mode)
                (void)::fchmod(shmFd_, 0666);
                if (::ftruncate(shmFd_, static_cast<off_t>(total)) != 0) {
                    fail("ftruncate failed");
                }
                void *m = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, shmFd_, 0);
                if (m == MAP_FAILED) {
                    fail("mmap failed");
                }
                mem_ = m;
                memSize_ = total;
                // prefault every page for writing
                std::memset(mem_, 0, total);
                auto *h = reinterpret_cast<ShmCursorHeader *>(mem_);
                h->layoutVersion = SHM_CURSOR_LAYOUT_VERSION;
                h->slotSize = opt.slotSize;
                h->slotCount = opt.slotCount;
                h->headerSize = headerSize;
                h->claim.store(0, std::memory_order_relaxed);
                h->commit.store(0, std::memory_order_relaxed);
                h->epoch.store(0, std::memory_order_relaxed);
                h->epochStart.store(1, std::memory_order_relaxed);
                h->futexWord.store(0, std::memory_order_relaxed);
                h->waiters.store(0, std::memory_order_relaxed);
                h->magic.store(SHM_CURSOR_MAGIC, std::memory_order_release);
            }
            // best effort: RLIMIT_MEMLOCK may forbid it, which is fine
            (void)::mlock(mem_, memSize_);

            hdr_ = reinterpret_cast<ShmCursorHeader *>(mem_);
            slots_ = reinterpret_cast<char *>(mem_) + headerSize;
            hdr_->publisherWakes.store(wake_ ? 1 : 0, std::memory_order_relaxed);

            uint64_t claim = hdr_->claim.load(std::memory_order_acquire);
            uint64_t commit = hdr_->commit.load(std::memory_order_acquire);
            if (claim > commit) {
                // Previous publisher died between claim and commit, so slot "claim" may be
                // torn. Commit it as a skip marker so readers waiting on it move on.
                SlotHeader skip{0, 0, 0, SLOT_FLAG_SKIP};
                std::memcpy(slots_ + (claim & mask_) * slotSize_, &skip, sizeof(skip));
                hdr_->commit.store(claim, std::memory_order_release);
                commit = claim;
            }
            next_ = std::max(claim, commit) + 1;
            hdr_->epochStart.store(next_, std::memory_order_relaxed);
            hdr_->epoch.fetch_add(1, std::memory_order_acq_rel);
        }
        ~ShmCursorSender() {
            cleanup();
        }
        ShmCursorSender(ShmCursorSender const &) = delete;
        ShmCursorSender &operator=(ShmCursorSender const &) = delete;

        bool wake() const {
            return wake_;
        }
        bool lock() const {
            return lock_;
        }
        uint64_t rejectedCount() const {
            return rejected_.load(std::memory_order_relaxed);
        }

        template <bool Wake, bool Lock>
        inline void publish(std::string_view topic, std::string_view content) {
            std::size_t const total = topic.size() + content.size();
            if (total > capacity_) {
                reject(topic.size(), content.size());
                return;
            }
            if constexpr (Lock) {
                // only for processes that publish to one segment from several threads
                //(e.g. data plus heartbeats); uncontended cost is one atomic exchange
                while (locked_.exchange(true, std::memory_order_acquire)) {
                    while (locked_.load(std::memory_order_relaxed)) {
                        cpuRelax();
                    }
                }
            }
            uint64_t const i = next_++;
            hdr_->claim.store(i, std::memory_order_relaxed);
            // orders the claim store before the slot writes (compiler barrier only on x86)
            std::atomic_thread_fence(std::memory_order_release);
            char *s = slots_ + (i & mask_) * slotSize_;
            SlotHeader h{static_cast<uint32_t>(content.size()), static_cast<uint32_t>(topic.size()), topicHash(topic), 0};
            std::memcpy(s, &h, sizeof(h));
            std::memcpy(s + sizeof(h), topic.data(), topic.size());
            std::memcpy(s + sizeof(h) + topic.size(), content.data(), content.size());
            hdr_->commit.store(i, std::memory_order_release);
            if constexpr (Wake) {
                // pairs with the reader's waiters increment followed by its commit recheck
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (hdr_->waiters.load(std::memory_order_relaxed) != 0) {
                    hdr_->futexWord.fetch_add(1, std::memory_order_release);
                    futexCall(&hdr_->futexWord, FUTEX_WAKE, INT_MAX, nullptr);
                }
            }
            if constexpr (Lock) {
                locked_.store(false, std::memory_order_release);
            }
        }
    };

    class ShmCursorSubscription {
    private:
        struct ClientCB {
            uint32_t id;
            std::function<void(basic::ByteDataWithTopic &&)> cb;
            std::optional<WireToUserHook> hook;
        };
        struct StringClient {
            std::string topic;
            uint32_t hash;
            ClientCB client;
        };
        struct RegexClient {
            std::regex re;
            ClientCB client;
            //(hash, topic, matched) memo so that std::regex only runs once per distinct topic
            std::vector<std::tuple<uint32_t, std::string, bool>> cache;
        };
        struct ClientSet {
            std::vector<ClientCB> noFilter;
            std::vector<StringClient> stringMatch;
            std::vector<RegexClient> regexMatch;
            std::size_t size() const {
                return noFilter.size() + stringMatch.size() + regexMatch.size();
            }
        };
        static constexpr std::size_t REGEX_CACHE_LIMIT = 256;

        std::string id_;
        std::string shmName_;
        std::string shmPath_;
        std::string lockPath_;
        ShmCursorOptions opt_;

        // written by add/remove under clientsMutex_, picked up by the reader
        // thread when version_ changes; the hot path never takes the mutex
        std::mutex clientsMutex_;
        ClientSet pending_;
        std::atomic<uint64_t> version_;
        std::atomic<uint64_t> ack_;

        // owned by the reader thread
        ClientSet active_;
        std::vector<ClientCB const *> matched_;
        uint64_t localVersion_;

        int shmFd_;
        int lockFd_;
        void *mem_;
        std::size_t memSize_;
        ShmCursorHeader *hdr_;
        char *slots_;
        uint32_t slotSize_;
        uint32_t capacity_;
        uint64_t slotCount_;
        uint64_t mask_;
        ino_t ino_;
        uint64_t lastEpoch_;
        bool publisherAlive_;
        bool warnedNoWake_;
        std::vector<char> buf_;
        uint64_t next_;

        std::atomic<uint64_t> lost_;
        std::atomic<uint64_t> overwritten_;
        std::atomic<bool> running_;
        std::atomic<bool> exited_;
        std::thread th_;
        //built in ShmCursorComponentImpl::addSubscriptionClient (which also
        //validates it in the caller's thread); the reader thread binds itself
        thread_cpu_lease_detail::BindRequest bindRequest_;
        bool warnedCpuMismatch_ = false;

        void refreshClients() {
            std::lock_guard<std::mutex> _(clientsMutex_);
            uint64_t v = version_.load(std::memory_order_relaxed);
            active_ = pending_;
            matched_.clear();
            matched_.reserve(active_.size());
            localVersion_ = v;
            ack_.store(v, std::memory_order_release);
        }

        bool attach() {
            int fd = ::shm_open(shmName_.c_str(), O_RDWR | O_CLOEXEC, 0);
            if (fd < 0) {
                return false;
            }
            struct stat st;
            if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) < sizeof(ShmCursorHeader)) {
                ::close(fd);
                return false;
            }
            std::size_t size = static_cast<std::size_t>(st.st_size);
            // read-write because readers update the waiter count in futex mode
            void *m = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
            if (m == MAP_FAILED) {
                ::close(fd);
                return false;
            }
            auto *h = reinterpret_cast<ShmCursorHeader *>(m);
            bool ok = (h->magic.load(std::memory_order_acquire) == SHM_CURSOR_MAGIC && h->layoutVersion == SHM_CURSOR_LAYOUT_VERSION && h->headerSize == sizeof(ShmCursorHeader) && h->slotSize >= 64 && h->slotSize % 64 == 0 && isPowerOfTwo(h->slotCount) && size >= h->headerSize + static_cast<std::size_t>(h->slotSize) * h->slotCount);
            if (!ok) {
                // not initialized yet (or foreign data); try again later
                ::munmap(m, size);
                ::close(fd);
                return false;
            }
            (void)::mlock(m, size);
            shmFd_ = fd;
            mem_ = m;
            memSize_ = size;
            hdr_ = h;
            slots_ = reinterpret_cast<char *>(m) + h->headerSize;
            slotSize_ = h->slotSize;
            capacity_ = h->slotSize - static_cast<uint32_t>(sizeof(SlotHeader));
            slotCount_ = h->slotCount;
            mask_ = h->slotCount - 1;
            ino_ = st.st_ino;
            lastEpoch_ = h->epoch.load(std::memory_order_acquire);
            buf_.resize(slotSize_);
            uint64_t c = h->commit.load(std::memory_order_acquire);
            // join: "latest" redelivers the last committed message (if any) as long as it
            // was written by the currently running publisher incarnation, "next" waits
            // for a new one; a message left behind by a dead or previous publisher is
            // never delivered on join
            publisherAlive_ = probePublisherAlive();
            uint64_t const es = h->epochStart.load(std::memory_order_acquire);
            next_ = (opt_.joinLatest && c > 0 && c >= es && publisherAlive_) ? c : c + 1;
            if (opt_.futex && h->publisherWakes.load(std::memory_order_relaxed) == 0 && !warnedNoWake_) {
                warnedNoWake_ = true;
                std::cerr << "[ShmCursorComponent] reader for '" << id_ << "' uses wait=futex but the publisher does not issue wakeups; "
                          << "readers fall back to futexTimeoutMs=" << opt_.futexTimeoutMs << " polling" << std::endl;
            }
            return true;
        }
        void detach() {
            if (mem_ != nullptr) {
                ::munmap(mem_, memSize_);
                mem_ = nullptr;
            }
            hdr_ = nullptr;
            slots_ = nullptr;
            if (shmFd_ >= 0) {
                ::close(shmFd_);
                shmFd_ = -1;
            }
            if (lockFd_ >= 0) {
                ::close(lockFd_);
                lockFd_ = -1;
            }
        }
        // runs only while idle; returns false when the segment must be re-attached
        bool periodicCheck() {
            struct stat st;
            if (::stat(shmPath_.c_str(), &st) == 0 && st.st_ino != ino_) {
                std::cerr << "[ShmCursorComponent] reader for '" << id_ << "': segment was re-created, re-attaching" << std::endl;
                return false;
            }
            uint64_t e = hdr_->epoch.load(std::memory_order_acquire);
            if (e != lastEpoch_) {
                std::cerr << "[ShmCursorComponent] reader for '" << id_ << "': publisher restarted (epoch " << lastEpoch_ << " -> " << e << ")" << std::endl;
                lastEpoch_ = e;
                publisherAlive_ = true;
            }
            bool alive = probePublisherAlive();
            if (alive != publisherAlive_) {
                std::cerr << "[ShmCursorComponent] reader for '" << id_ << "': publisher " << (alive ? "is running again" : "is not running") << std::endl;
                publisherAlive_ = alive;
            }
            return true;
        }
        // the publisher holds an exclusive flock on the lock file for its lifetime
        bool probePublisherAlive() {
            if (lockFd_ < 0) {
                lockFd_ = ::open(lockPath_.c_str(), O_RDONLY | O_CLOEXEC);
                if (lockFd_ < 0) {
                    return false;
                }
            }
            if (::flock(lockFd_, LOCK_SH | LOCK_NB) == 0) {
                ::flock(lockFd_, LOCK_UN);
                return false;
            }
            return (errno == EWOULDBLOCK);
        }

        bool regexMatches(RegexClient &c, std::string_view topic, uint32_t hash) {
            for (auto const &e : c.cache) {
                if (std::get<0>(e) == hash && std::get<1>(e) == topic) {
                    return std::get<2>(e);
                }
            }
            bool res = std::regex_match(topic.begin(), topic.end(), c.re);
            if (c.cache.size() < REGEX_CACHE_LIMIT) {
                c.cache.emplace_back(hash, std::string(topic), res);
            }
            return res;
        }
        inline void callClient(ClientCB const &c, std::string_view topic, std::string_view content) {
            if (c.hook) {
                auto b = (c.hook->hook)(basic::ByteDataView{content});
                if (b) {
                    c.cb({std::string(topic), std::move(b->content)});
                }
            } else {
                c.cb({std::string(topic), std::string(content)});
            }
        }
        inline void dispatch(std::string_view topic, uint32_t hash, std::string_view content) {
            matched_.clear();
            for (auto const &c : active_.noFilter) {
                matched_.push_back(&c);
            }
            for (auto const &c : active_.stringMatch) {
                if (c.hash == hash && c.topic == topic) {
                    matched_.push_back(&c.client);
                }
            }
            for (auto &c : active_.regexMatch) {
                if (regexMatches(c, topic, hash)) {
                    matched_.push_back(&c.client);
                }
            }
            for (auto const *c : matched_) {
                callClient(*c, topic, content);
            }
        }

        struct SpinWait {
            static inline bool idle(ShmCursorOptions const &opt, ShmCursorHeader *, uint64_t, uint64_t emptyPolls) {
                // no PAUSE on the hot loop; only after a long idle stretch
                if (emptyPolls > opt.pauseAfter) {
                    cpuRelax();
                }
                return false;
            }
        };
        struct FutexWait {
            static inline bool idle(ShmCursorOptions const &opt, ShmCursorHeader *hdr, uint64_t next, uint64_t emptyPolls) {
                if (emptyPolls <= opt.spinCount) {
                    return false;
                }
                hdr->waiters.fetch_add(1, std::memory_order_seq_cst);
                uint32_t v = hdr->futexWord.load(std::memory_order_acquire);
                if (hdr->commit.load(std::memory_order_seq_cst) < next) {
                    struct timespec ts;
                    ts.tv_sec = opt.futexTimeoutMs / 1000;
                    ts.tv_nsec = static_cast<long>(opt.futexTimeoutMs % 1000) * 1000000L;
                    futexCall(&hdr->futexWord, FUTEX_WAIT, v, &ts);
                }
                hdr->waiters.fetch_sub(1, std::memory_order_seq_cst);
                return true;
            }
        };

        // returns when stopping or when the segment must be re-attached
        template <class WaitPolicy>
        void readLoop() {
            ShmCursorHeader *const hdr = hdr_;
            char const *const slots = slots_;
            uint32_t const slotSize = slotSize_;
            uint32_t const capacity = capacity_;
            uint64_t const slotCount = slotCount_;
            uint64_t const mask = mask_;
            char *const buf = buf_.data();
            uint64_t j = next_;
            uint64_t emptyPolls = 0;
            auto lastCheck = std::chrono::steady_clock::now();

            while (running_.load(std::memory_order_relaxed)) {
                if (version_.load(std::memory_order_relaxed) != localVersion_) {
                    refreshClients();
                }
                uint64_t const c = hdr->commit.load(std::memory_order_acquire);
                if (c < j) {
                    ++emptyPolls;
                    bool slept = WaitPolicy::idle(opt_, hdr, j, emptyPolls);
                    if (slept || (emptyPolls & 4095) == 0) {
                        auto now = std::chrono::steady_clock::now();
                        if (now - lastCheck >= std::chrono::milliseconds(100)) {
                            lastCheck = now;
                            if (!periodicCheck()) {
                                next_ = j;
                                detach();
                                return;
                            }
                        }
                    }
                    continue;
                }
                emptyPolls = 0;
                if (c - j >= slotCount) {
                    // lapped: everything before the latest committed message is gone
                    lost_.fetch_add(c - j, std::memory_order_relaxed);
                    j = c;
                }
                char const *s = slots + (j & mask) * slotSize;
                // racy reads of the lengths only size the copy; they are clamped and
                // the copy is validated against claim below
                auto const *vs = reinterpret_cast<SlotHeader const volatile *>(s);
                uint32_t const tl0 = vs->topicLen;
                uint32_t const cl0 = vs->contentLen;
                std::size_t const n = (tl0 <= capacity && cl0 <= capacity - tl0) ? (tl0 + cl0) : capacity;
                std::memcpy(buf, s, sizeof(SlotHeader) + n);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (hdr->claim.load(std::memory_order_relaxed) >= j + slotCount) {
                    // the writer started overwriting slot j while we copied it
                    uint64_t const c2 = hdr->commit.load(std::memory_order_acquire);
                    uint64_t const nj = (c2 > j) ? c2 : (j + 1);
                    lost_.fetch_add(nj - j, std::memory_order_relaxed);
                    overwritten_.fetch_add(1, std::memory_order_relaxed);
                    j = nj;
                    continue;
                }
                ++j;
                SlotHeader h;
                std::memcpy(&h, buf, sizeof(h));
                if (h.flags & SLOT_FLAG_SKIP) {
                    continue;
                }
                if (h.topicLen > capacity || h.contentLen > capacity - h.topicLen) {
                    // cannot happen with a well-behaved publisher; never trust it blindly
                    lost_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                dispatch(
                    std::string_view(buf + sizeof(SlotHeader), h.topicLen), h.topicHash, std::string_view(buf + sizeof(SlotHeader) + h.topicLen, h.contentLen));
            }
            next_ = j;
        }

        void run(thread_cpu_lease_detail::BindHandshake *handshake) {
            //first action: pin this thread, reports the result to the caller
            auto lease = thread_cpu_lease_detail::bindWorkerThread(&bindRequest_, handshake);
            if (!lease) {
                return;
            }
            try {
                while (running_.load(std::memory_order_acquire)) {
                    if (version_.load(std::memory_order_relaxed) != localVersion_) {
                        refreshClients();
                    }
                    if (hdr_ == nullptr) {
                        if (!attach()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            continue;
                        }
                    }
                    if (opt_.futex) {
                        readLoop<FutexWait>();
                    } else {
                        readLoop<SpinWait>();
                    }
                }
            } catch (std::exception const &ex) {
                std::cerr << "[ShmCursorComponent] reader for '" << id_ << "' stopped on exception: " << ex.what() << std::endl;
            }
            detach();
            // release anybody waiting for an acknowledgement
            ack_.store(UINT64_MAX, std::memory_order_release);
            exited_.store(true, std::memory_order_release);
        }

    public:
        ShmCursorSubscription(std::string const &id, ShmCursorOptions const &opt
            , thread_cpu_lease_detail::BindRequest bindRequest)
            : id_(id)
            , shmName_(shmNameFor(id))
            , shmPath_(shmPathFor(id))
            , lockPath_(lockPathFor(id))
            , opt_(opt)
            , clientsMutex_()
            , pending_()
            , version_(0)
            , ack_(0)
            , active_()
            , matched_()
            , localVersion_(0)
            , shmFd_(-1)
            , lockFd_(-1)
            , mem_(nullptr)
            , memSize_(0)
            , hdr_(nullptr)
            , slots_(nullptr)
            , slotSize_(0)
            , capacity_(0)
            , slotCount_(0)
            , mask_(0)
            , ino_(0)
            , lastEpoch_(0)
            , publisherAlive_(true)
            , warnedNoWake_(false)
            , buf_()
            , next_(1)
            , lost_(0)
            , overwritten_(0)
            , running_(true)
            , exited_(false)
            , th_() {
            validateIdentifier(id);
            bindRequest_ = std::move(bindRequest);
            std::optional<thread_cpu_lease_detail::BindHandshake> handshake;
            if (bindRequest_.needed) {
                handshake.emplace();
            }
            th_ = std::thread([this, hs = handshake ? &*handshake : nullptr]() {
                run(hs);
            });
            if (handshake) {
                try {
                    handshake->future.get();
                } catch (...) {
                    //the reader thread has already returned
                    th_.join();
                    throw;
                }
            }
        }
        ~ShmCursorSubscription() {
            running_.store(false, std::memory_order_release);
            if (th_.joinable()) {
                if (th_.get_id() == std::this_thread::get_id()) {
                    th_.detach();
                } else {
                    th_.join();
                }
            }
        }
        ShmCursorSubscription(ShmCursorSubscription const &) = delete;
        ShmCursorSubscription &operator=(ShmCursorSubscription const &) = delete;

        void addSubscription(
            uint32_t id, std::variant<ShmCursorComponent::NoTopicSelection, std::string, std::regex> const &topic, std::function<void(basic::ByteDataWithTopic &&)> handler, std::optional<WireToUserHook> wireToUserHook) {
            std::lock_guard<std::mutex> _(clientsMutex_);
            switch (topic.index()) {
            case 0:
                pending_.noFilter.push_back({id, handler, wireToUserHook});
                break;
            case 1:
                pending_.stringMatch.push_back({std::get<std::string>(topic), topicHash(std::get<std::string>(topic)), {id, handler, wireToUserHook}});
                break;
            case 2:
                pending_.regexMatch.push_back({std::get<std::regex>(topic), {id, handler, wireToUserHook}, {}});
                break;
            default:
                break;
            }
            version_.fetch_add(1, std::memory_order_release);
        }
        // after this returns the callback is not invoked any more (unless called
        // from inside a callback on the reader thread itself)
        void removeSubscription(uint32_t id) {
            uint64_t v;
            {
                std::lock_guard<std::mutex> _(clientsMutex_);
                auto &nf = pending_.noFilter;
                nf.erase(std::remove_if(nf.begin(), nf.end(), [id](auto const &x) { return x.id == id; }), nf.end());
                auto &sm = pending_.stringMatch;
                sm.erase(std::remove_if(sm.begin(), sm.end(), [id](auto const &x) { return x.client.id == id; }), sm.end());
                auto &rm = pending_.regexMatch;
                rm.erase(std::remove_if(rm.begin(), rm.end(), [id](auto const &x) { return x.client.id == id; }), rm.end());
                v = version_.fetch_add(1, std::memory_order_release) + 1;
            }
            if (th_.get_id() == std::this_thread::get_id()) {
                return;
            }
            while (ack_.load(std::memory_order_acquire) < v) {
                std::this_thread::yield();
            }
        }
        bool empty() {
            std::lock_guard<std::mutex> _(clientsMutex_);
            return pending_.size() == 0;
        }
        void requestStop() {
            running_.store(false, std::memory_order_release);
        }
        bool exited() const {
            return exited_.load(std::memory_order_acquire);
        }
        bool isReaderThread() const {
            return th_.get_id() == std::this_thread::get_id();
        }
        uint64_t lostCount() const {
            return lost_.load(std::memory_order_relaxed);
        }
        uint64_t overwrittenCount() const {
            return overwritten_.load(std::memory_order_relaxed);
        }
        std::thread::native_handle_type getThreadHandle() {
            return th_.native_handle();
        }
        thread_cpu_lease_detail::BindRequest const &bindRequest() const {
            return bindRequest_;
        }
        //There is one reader thread per segment: the first locator decides the
        //binding, later different values only warn once.
        void warnCpuMismatch(ConnectionLocator const &locator, thread_cpu_lease_detail::BindRequest const &request) {
            if (warnedCpuMismatch_) {
                return;
            }
            warnedCpuMismatch_ = true;
            std::cerr << "shm_cursor: reader thread for " << locator.identifier()
                << " already uses cpu='" << bindRequest_.spec
                << "', ignoring cpu='" << request.spec << "' requested by "
                << locator.toPrintFormat() << std::endl;
        }
    };

    class ShmCursorComponentImpl {
    private:
        std::mutex mutex_;

        //the binding provider, settable once before the graph starts
        std::mutex providerMutex_;
        ThreadCpuLeaseProvider provider_;
        bool providerSet_ = false;

        ThreadCpuLeaseProvider getThreadCpuLeaseProvider() {
            std::lock_guard<std::mutex> _(providerMutex_);
            return provider_;
        }
        std::unordered_map<std::string, std::shared_ptr<ShmCursorSubscription>> subscriptions_;
        // stopped subscriptions whose reader thread may still be winding down,
        // or which another removeSubscriptionClient call still references
        std::vector<std::shared_ptr<ShmCursorSubscription>> stopped_;
        std::unordered_map<std::string, std::unique_ptr<ShmCursorSender>> senders_;

        std::mutex idMutex_;
        uint32_t counter_;
        std::unordered_map<uint32_t, std::shared_ptr<ShmCursorSubscription>> idToSubscriptionMap_;

        // must be called with mutex_ held
        void reapStopped() {
            stopped_.erase(
                std::remove_if(stopped_.begin(), stopped_.end(), [](auto const &p) {
                    return p.use_count() == 1 && p->exited() && !p->isReaderThread();
                }),
                stopped_.end());
        }

    public:
        ShmCursorComponentImpl()
            : mutex_()
            , subscriptions_()
            , stopped_()
            , senders_()
            , idMutex_()
            , counter_(0)
            , idToSubscriptionMap_() {
        }
        ~ShmCursorComponentImpl() {
            std::lock_guard<std::mutex> _(mutex_);
            subscriptions_.clear();
            stopped_.clear();
            senders_.clear();
        }
        void setThreadCpuLease(ThreadCpuLeaseProvider provider) {
            std::lock_guard<std::mutex> _(providerMutex_);
            if (providerSet_) {
                throw std::logic_error("shm_cursor: thread CPU lease provider is already set");
            }
            provider_ = std::move(provider);
            providerSet_ = true;
        }
        void setThreadCpuLeaseIfUnset(ThreadCpuLeaseProvider provider) {
            std::lock_guard<std::mutex> _(providerMutex_);
            if (providerSet_) {
                return;
            }
            provider_ = std::move(provider);
            providerSet_ = true;
        }
        uint32_t addSubscriptionClient(ConnectionLocator const &locator,
            std::variant<ShmCursorComponent::NoTopicSelection, std::string, std::regex> const &topic,
            std::function<void(basic::ByteDataWithTopic &&)> client,
            std::optional<WireToUserHook> wireToUserHook) {
            auto bindRequest = thread_cpu_lease_detail::makeThreadCpuBindRequest(
                locator, "cpu", "shm_cursor reader "+locator.identifier(), getThreadCpuLeaseProvider()
                , "shm_cursor", "shm_cursor_setThreadCpuLease");
            std::lock_guard<std::mutex> _(mutex_);
            reapStopped();
            auto iter = subscriptions_.find(locator.identifier());
            if (iter == subscriptions_.end()) {
                auto opt = ShmCursorOptions::fromLocator(locator);
                iter = subscriptions_.insert({locator.identifier(), std::make_shared<ShmCursorSubscription>(
                    locator.identifier(), opt, std::move(bindRequest))}).first;
            } else if (!thread_cpu_lease_detail::sameCpuBinding(iter->second->bindRequest(), bindRequest)) {
                iter->second->warnCpuMismatch(locator, bindRequest);
            }
            auto p = iter->second;
            std::lock_guard<std::mutex> _2(idMutex_);
            ++counter_;
            p->addSubscription(counter_, topic, client, wireToUserHook);
            idToSubscriptionMap_[counter_] = p;
            return counter_;
        }
        void removeSubscriptionClient(uint32_t id) {
            std::shared_ptr<ShmCursorSubscription> p;
            {
                std::lock_guard<std::mutex> _(idMutex_);
                auto iter = idToSubscriptionMap_.find(id);
                if (iter == idToSubscriptionMap_.end()) {
                    return;
                }
                p = iter->second;
                idToSubscriptionMap_.erase(iter);
            }
            // outside mutex_ so that a callback calling add/remove cannot deadlock
            p->removeSubscription(id);
            std::lock_guard<std::mutex> _(mutex_);
            for (auto iter = subscriptions_.begin(); iter != subscriptions_.end(); ++iter) {
                if (iter->second == p) {
                    if (p->empty()) {
                        p->requestStop();
                        stopped_.push_back(std::move(iter->second));
                        subscriptions_.erase(iter);
                    }
                    break;
                }
            }
            p.reset();
            reapStopped();
        }
        template <bool Wake, bool Lock>
        static std::function<void(basic::ByteDataWithTopic &&)> makePublisher(ShmCursorSender *p, std::optional<UserToWireHook> const &userToWireHook) {
            if (userToWireHook) {
                auto hook = userToWireHook->hook;
                return [p, hook](basic::ByteDataWithTopic &&data) {
                    auto w = hook(basic::ByteData{std::move(data.content)});
                    p->publish<Wake, Lock>(data.topic, w.content);
                };
            } else {
                return [p](basic::ByteDataWithTopic &&data) {
                    p->publish<Wake, Lock>(data.topic, data.content);
                };
            }
        }
        std::function<void(basic::ByteDataWithTopic &&)> getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook) {
            ShmCursorSender *p = nullptr;
            {
                std::lock_guard<std::mutex> _(mutex_);
                auto iter = senders_.find(locator.identifier());
                if (iter == senders_.end()) {
                    auto opt = ShmCursorOptions::fromLocator(locator);
                    iter = senders_.insert({locator.identifier(), std::make_unique<ShmCursorSender>(locator.identifier(), opt)}).first;
                }
                p = iter->second.get();
            }
            if (p->wake()) {
                return p->lock() ? makePublisher<true, true>(p, userToWireHook) : makePublisher<true, false>(p, userToWireHook);
            } else {
                return p->lock() ? makePublisher<false, true>(p, userToWireHook) : makePublisher<false, false>(p, userToWireHook);
            }
        }
        std::unordered_map<ConnectionLocator, std::thread::native_handle_type> threadHandles() {
            std::unordered_map<ConnectionLocator, std::thread::native_handle_type> retVal;
            std::lock_guard<std::mutex> _(mutex_);
            for (auto &item : subscriptions_) {
                ConnectionLocator idOnly{"", 0, "", "", item.first};
                retVal[idOnly] = item.second->getThreadHandle();
            }
            return retVal;
        }
        uint64_t subscriptionLossCount(ConnectionLocator const &locator) {
            std::lock_guard<std::mutex> _(mutex_);
            auto iter = subscriptions_.find(locator.identifier());
            return (iter == subscriptions_.end()) ? 0 : iter->second->lostCount();
        }
        uint64_t subscriptionOverwriteCount(ConnectionLocator const &locator) {
            std::lock_guard<std::mutex> _(mutex_);
            auto iter = subscriptions_.find(locator.identifier());
            return (iter == subscriptions_.end()) ? 0 : iter->second->overwrittenCount();
        }
        uint64_t publisherRejectedCount(ConnectionLocator const &locator) {
            std::lock_guard<std::mutex> _(mutex_);
            auto iter = senders_.find(locator.identifier());
            return (iter == senders_.end()) ? 0 : iter->second->rejectedCount();
        }
    };

    ShmCursorComponent::ShmCursorComponent()
        : impl_(std::make_unique<ShmCursorComponentImpl>()) {}
    ShmCursorComponent::~ShmCursorComponent() {}
    uint32_t ShmCursorComponent::shm_cursor_addSubscriptionClient(ConnectionLocator const &locator,
        std::variant<ShmCursorComponent::NoTopicSelection, std::string, std::regex> const &topic,
        std::function<void(basic::ByteDataWithTopic &&)> client,
        std::optional<WireToUserHook> wireToUserHook) {
        return impl_->addSubscriptionClient(locator, topic, client, wireToUserHook);
    }
    void ShmCursorComponent::shm_cursor_removeSubscriptionClient(uint32_t id) {
        impl_->removeSubscriptionClient(id);
    }
    std::function<void(basic::ByteDataWithTopic &&)> ShmCursorComponent::shm_cursor_getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook) {
        return impl_->getPublisher(locator, userToWireHook);
    }
    std::unordered_map<ConnectionLocator, std::thread::native_handle_type> ShmCursorComponent::shm_cursor_threadHandles() {
        return impl_->threadHandles();
    }
    uint64_t ShmCursorComponent::shm_cursor_subscriptionLossCount(ConnectionLocator const &locator) {
        return impl_->subscriptionLossCount(locator);
    }
    uint64_t ShmCursorComponent::shm_cursor_subscriptionOverwriteCount(ConnectionLocator const &locator) {
        return impl_->subscriptionOverwriteCount(locator);
    }
    uint64_t ShmCursorComponent::shm_cursor_publisherRejectedCount(ConnectionLocator const &locator) {
        return impl_->publisherRejectedCount(locator);
    }
    void ShmCursorComponent::shm_cursor_setThreadCpuLease(ThreadCpuLeaseProvider provider) {
        impl_->setThreadCpuLease(std::move(provider));
    }
    void ShmCursorComponent::shm_cursor_setThreadCpuLeaseIfUnset(ThreadCpuLeaseProvider provider) {
        impl_->setThreadCpuLeaseIfUnset(std::move(provider));
    }

}}}}}
