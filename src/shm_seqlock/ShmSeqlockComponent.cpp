#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <climits>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#include <tm_kit/transport/shm_seqlock/ShmSeqlockComponent.hpp>

#include "../ThreadCpuLeaseSupport.hpp"

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace shm_seqlock {

    //Shared memory layout
    //
    //  [header: 5 lines of 128 bytes][slot 0][slot 1]...[slot slotCount-1]
    //
    //Each slot starts with ShmSeqlockSlotHead (24 bytes, same cache line as the
    //first data bytes) followed by topic bytes and then payload bytes.
    //
    //With layout=split (layout version 2) each slot starts with a 64B line
    //holding only seq, followed by ShmSeqlockSlotHead (its seq unused), topic
    //and payload. Right after committing message m the publisher pre-marks
    //the slot of m+1 odd, so the line readers poll while waiting for a
    //message gets one writer store (the commit). One slot is always
    //pre-marked, so a reader can lag at most slotCount-1 messages.
    //
    //With layout=inline-premark (layout version 3) the slot layout is the
    //same as inline (no extra 64B seq line), but the publisher also pre-marks
    //the next slot after each commit. Additionally the hot-path write order is
    //reordered: data bytes beyond line 0 (bytes >= LINE0_DATA_BYTES, i.e. the
    //portion that cannot fit on the same line as the slot head) are written
    //first, then lengths/topic-prefix/commit-seq are written to line 0 as one
    //burst. This means the polled line (line 0 which holds seq) receives two
    //stores per message in the pre-mark phase (odd pre-mark then even commit),
    //but the cold data RFOs happen before the line-0 burst, so the commit store
    //arrives as quickly as possible after the reader sees the pre-mark. One slot
    //is always pre-marked, so a reader can lag at most slotCount-1 messages.
    //
    //Message i (global index, monotonic across publisher restarts) lives in
    //slot (i & (slotCount-1)). Its seq is 2i+1 while being written and 2i+2
    //when done. Since indices continue across publisher restarts, readers do
    //not need an epoch in seq to stay correct; the epoch in the header only
    //lets readers count restarts.
    namespace {
        constexpr uint64_t SHM_SEQLOCK_MAGIC = 0x314B434F4C514553ULL; //"SEQLOCK1"
        //written into the header of a segment that a restarted publisher replaced
        //(the name now points to a new segment, readers must reopen)
        constexpr uint64_t SHM_SEQLOCK_SUPERSEDED = 0x44454445434F4C53ULL;
        //layout version 1 (inline): seq in the slot head, on the same line as the
        //first data bytes; layout version 2 (split): seq alone on the first 64B
        //line of the slot, slot head and data from the second line on;
        //layout version 3 (inline-premark): same slot layout as inline, but the
        //publisher pre-marks the next slot after each commit and reorders the
        //write: data beyond line 0 (bytes >= LINE0_DATA_BYTES) is written before
        //the line-0 burst (lengths + first LINE0_DATA_BYTES + commit seq), so
        //cold data RFOs happen before the polled line is committed
        constexpr uint32_t SHM_SEQLOCK_LAYOUT_INLINE = 1;
        constexpr uint32_t SHM_SEQLOCK_LAYOUT_SPLIT = 2;
        constexpr uint32_t SHM_SEQLOCK_LAYOUT_INLINE_PREMARK = 3;
        constexpr std::size_t SHM_SEQLOCK_SPLIT_HEAD_OFFSET = 64;
        constexpr uint32_t SHM_SEQLOCK_FLAG_FUTEX = 1;

        struct ShmSeqlockHeader {
            alignas(128) std::atomic<uint64_t> magic; //stored last (release) when the segment is initialized
            uint32_t layoutVersion;
            uint32_t slotCapacity;
            uint32_t slotCount;
            uint32_t slotStride;
            std::atomic<uint32_t> flags;
            alignas(128) std::atomic<uint64_t> epoch;
            alignas(128) std::atomic<uint64_t> published; //number of fully published messages = next index
            alignas(128) std::atomic<uint32_t> futexWord;
            alignas(128) std::atomic<uint32_t> waiters;
        };
        static_assert(sizeof(ShmSeqlockHeader) == 5*128, "unexpected shm_seqlock header size");

        struct ShmSeqlockSlotHead {
            std::atomic<uint64_t> seq;
            std::atomic<uint32_t> payloadLen;
            std::atomic<uint32_t> topicLen;
            std::atomic<uint32_t> topicHash;
            uint32_t reserved;
        };
        static_assert(sizeof(ShmSeqlockSlotHead) == 24, "unexpected shm_seqlock slot head size");
        static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free, "shm_seqlock requires lock-free atomics");
        //number of data bytes that fit on line 0 of an inline/inline-premark slot
        //(64B line, 24B slot head => 40B data on line 0). Used by inline-premark to
        //split the write: bytes >= this are written first, then line 0 is burst.
        constexpr std::size_t SHM_SEQLOCK_LINE0_DATA_BYTES = 64 - sizeof(ShmSeqlockSlotHead);

        inline uint32_t topicHash(char const *p, std::size_t n) {
            uint32_t h = 2166136261u;
            for (std::size_t ii=0; ii<n; ++ii) {
                h ^= static_cast<unsigned char>(p[ii]);
                h *= 16777619u;
            }
            return h;
        }
        inline std::size_t slotStrideFor(std::size_t capacity, bool stride128, bool split) {
            //inline-premark uses the same stride as inline (no extra seq line)
            std::size_t unit = (stride128 ? 128 : 64);
            return ((split ? SHM_SEQLOCK_SPLIT_HEAD_OFFSET : 0)+sizeof(ShmSeqlockSlotHead)+capacity+unit-1)/unit*unit;
        }
        inline std::size_t segmentSize(std::size_t slotCount, std::size_t stride) {
            return sizeof(ShmSeqlockHeader)+slotCount*stride;
        }
        inline int futexWait(std::atomic<uint32_t> *addr, uint32_t val, long timeoutUs) {
            struct timespec ts;
            ts.tv_sec = timeoutUs/1000000;
            ts.tv_nsec = (timeoutUs%1000000)*1000;
            return static_cast<int>(syscall(SYS_futex, reinterpret_cast<uint32_t *>(addr), FUTEX_WAIT, val, &ts, nullptr, 0));
        }
        inline long futexWakeAll(std::atomic<uint32_t> *addr) {
            return syscall(SYS_futex, reinterpret_cast<uint32_t *>(addr), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
        }
        //a reader killed while sleeping leaves its registration behind; after this
        //many consecutive wakes that found no sleeper the publisher resets the count
        constexpr uint32_t SHM_SEQLOCK_EMPTY_WAKES_BEFORE_RESET = 64;
        inline void cpuPause() {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
        inline std::string shmNameFor(ConnectionLocator const &locator) {
            auto const &id = locator.identifier();
            if (id.empty() || id.find('/') != std::string::npos) {
                throw std::runtime_error("[ShmSeqlockComponent] invalid segment name '"+id+"' (must be non-empty and contain no '/')");
            }
            return "/"+id;
        }
        inline std::size_t queryNumber(ConnectionLocator const &locator, std::string const &field, std::size_t defaultValue) {
            auto s = locator.query(field, "");
            if (s.empty()) {
                return defaultValue;
            }
            try {
                return static_cast<std::size_t>(std::stoull(s));
            } catch (std::exception const &) {
                throw std::runtime_error("[ShmSeqlockComponent] property '"+field+"' has invalid value '"+s+"'");
            }
        }
        inline bool queryBool(ConnectionLocator const &locator, std::string const &field, bool defaultValue) {
            auto s = locator.query(field, "");
            if (s.empty()) {
                return defaultValue;
            }
            return (s == "true" || s == "1" || s == "yes");
        }

        //One mapping of a segment, used by both sides
        struct ShmSeqlockMapping {
            void *base = nullptr;
            std::size_t size = 0;
            ino_t inode = 0;
            ShmSeqlockHeader *header = nullptr;
            char *slots = nullptr;
            std::size_t slotStride = 0;
            uint64_t slotMask = 0;
            std::size_t slotCapacity = 0;
            bool split = false;
            bool inlinePremark = false;

            template <bool Split>
            ShmSeqlockSlotHead *headAt(uint64_t idx) const {
                return reinterpret_cast<ShmSeqlockSlotHead *>(slots+(idx & slotMask)*slotStride+(Split ? SHM_SEQLOCK_SPLIT_HEAD_OFFSET : 0));
            }
            //the polled sequence word: in the slot head (inline/inline-premark) or
            //alone on the first line of the slot (split). inline-premark uses the
            //same offset as inline, since its slot layout is identical.
            template <bool Split>
            std::atomic<uint64_t> *seqAt(uint64_t idx) const {
                if constexpr (Split) {
                    return reinterpret_cast<std::atomic<uint64_t> *>(slots+(idx & slotMask)*slotStride);
                } else {
                    return &(headAt<false>(idx)->seq);
                }
            }
            static char *dataOf(ShmSeqlockSlotHead *s) {
                return reinterpret_cast<char *>(s)+sizeof(ShmSeqlockSlotHead);
            }
            void unmap() {
                if (base) {
                    ::munmap(base, size);
                }
                base = nullptr;
                header = nullptr;
                slots = nullptr;
                size = 0;
            }
            bool map(int fd, std::size_t sz, bool doMlock) {
                void *p = ::mmap(nullptr, sz, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, fd, 0);
                if (p == MAP_FAILED) {
                    return false;
                }
                if (doMlock) {
                    //best effort, usually limited by RLIMIT_MEMLOCK
                    (void) ::mlock(p, sz);
                }
                base = p;
                size = sz;
                header = reinterpret_cast<ShmSeqlockHeader *>(p);
                slots = reinterpret_cast<char *>(p)+sizeof(ShmSeqlockHeader);
                return true;
            }
            void setLayoutFromHeader() {
                slotStride = header->slotStride;
                slotMask = static_cast<uint64_t>(header->slotCount)-1;
                slotCapacity = header->slotCapacity;
                split = (header->layoutVersion == SHM_SEQLOCK_LAYOUT_SPLIT);
                inlinePremark = (header->layoutVersion == SHM_SEQLOCK_LAYOUT_INLINE_PREMARK);
            }
        };
    }

    class ShmSeqlockComponentImpl {
    private:
        class OneShmSeqlockSender {
        private:
            ConnectionLocator locator_;
            std::string shmName_;
            std::string lockPath_;
            int lockFd_;
            ShmSeqlockMapping map_;
            bool futex_;
            bool writerLock_;
            uint32_t layout_;
            bool noPremark_;
            std::atomic_flag writing_ = ATOMIC_FLAG_INIT;            uint64_t next_;
            std::string lastTopic_;
            uint32_t lastTopicHash_;
            std::atomic<uint64_t> rejected_;
            std::atomic<uint32_t> emptyWakes_;
            std::atomic<uint64_t> waiterResets_;

            void reject(std::size_t topicLen, std::size_t payloadLen) {
                // fetch_add: concurrent callers must not lose counts (non-atomic
                // load+store would race when writerLock is off or between processes)
                auto r = rejected_.fetch_add(1, std::memory_order_relaxed)+1;
                if (r == 1 || r % 10000 == 0) {
                    std::cerr << "[ShmSeqlockComponent] segment '" << locator_.identifier()
                        << "': rejected message with topic length " << topicLen
                        << " and payload length " << payloadLen
                        << " (slot capacity " << map_.slotCapacity << "), " << r << " rejected so far\n";
                }
            }
            void fail(std::string const &what) {
                map_.unmap();
                if (lockFd_ >= 0) {
                    ::flock(lockFd_, LOCK_UN);
                    ::close(lockFd_);
                    lockFd_ = -1;
                }
                throw std::runtime_error("[ShmSeqlockComponent] segment '"+locator_.identifier()+"': "+what);
            }
        public:
            OneShmSeqlockSender(ConnectionLocator const &locator)
                : locator_(locator), shmName_(shmNameFor(locator)), lockPath_(), lockFd_(-1), map_()
                , futex_(locator.query("wait", "spin") == "futex")
                , writerLock_(queryBool(locator, "writerLock", false))
                , layout_(SHM_SEQLOCK_LAYOUT_INLINE), noPremark_(queryBool(locator, "testNoPremark", false))
                , next_(0), lastTopic_(), lastTopicHash_(topicHash(nullptr, 0)), rejected_(0), emptyWakes_(0), waiterResets_(0)
            {
                lockPath_ = "/dev/shm"+shmName_+".lock";
                std::size_t capacity = queryNumber(locator, "slotSize", 1000);
                std::size_t count = queryNumber(locator, "slotCount", 4096);
                bool stride128 = queryBool(locator, "stride128", false);
                bool doMlock = queryBool(locator, "mlock", true);
                auto layoutName = locator.query("layout", "inline");
                if (layoutName == "split") {
                    layout_ = SHM_SEQLOCK_LAYOUT_SPLIT;
                } else if (layoutName == "inline-premark") {
                    layout_ = SHM_SEQLOCK_LAYOUT_INLINE_PREMARK;
                } else if (layoutName != "inline") {
                    throw std::runtime_error("[ShmSeqlockComponent] layout must be inline, split, or inline-premark");
                }
                if (count < 2 || count > (std::size_t(1) << 24) || (count & (count-1)) != 0) {
                    throw std::runtime_error("[ShmSeqlockComponent] slotCount must be a power of two between 2 and 2^24");
                }
                if (capacity < 1 || capacity > (std::size_t(1) << 26)) {
                    throw std::runtime_error("[ShmSeqlockComponent] slotSize must be between 1 and 2^26");
                }
                std::size_t stride = slotStrideFor(capacity, stride128, layout_ == SHM_SEQLOCK_LAYOUT_SPLIT);
                std::size_t size = segmentSize(count, stride);

                lockFd_ = ::open(lockPath_.c_str(), O_RDWR|O_CREAT|O_CLOEXEC, 0666);
                if (lockFd_ < 0) {
                    fail("cannot open lock file "+lockPath_+": "+std::strerror(errno));
                }
                (void) ::fchmod(lockFd_, 0666);
                //readers probe liveness with a momentary LOCK_SH, so retry briefly
                //before concluding that another publisher is alive
                bool locked = false;
                for (int ii=0; ii<20 && !locked; ++ii) {
                    if (::flock(lockFd_, LOCK_EX|LOCK_NB) == 0) {
                        locked = true;
                    } else if (errno == EWOULDBLOCK) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    } else {
                        break;
                    }
                }
                if (!locked) {
                    fail("another live publisher holds "+lockPath_);
                }

                //We hold the lock, so any existing segment content is ours to reuse or replace
                bool reused = false;
                int fd = ::shm_open(shmName_.c_str(), O_RDWR|O_CLOEXEC, 0);
                if (fd >= 0) {
                    struct stat st;
                    if (::fstat(fd, &st) == 0
                        && static_cast<std::size_t>(st.st_size) >= sizeof(ShmSeqlockHeader)
                        && map_.map(fd, static_cast<std::size_t>(st.st_size), doMlock)) {
                        auto *h = map_.header;
                        auto magic = h->magic.load(std::memory_order_acquire);
                        if (magic == SHM_SEQLOCK_MAGIC
                            && static_cast<std::size_t>(st.st_size) == size
                            && h->layoutVersion == layout_
                            && h->slotCapacity == capacity
                            && h->slotCount == count
                            && h->slotStride == stride) {
                            reused = true;
                            map_.inode = st.st_ino;
                        } else {
                            if (magic == SHM_SEQLOCK_MAGIC) {
                                //tell readers still mapping it to reopen by name
                                h->magic.store(SHM_SEQLOCK_SUPERSEDED, std::memory_order_release);
                            }
                            map_.unmap();
                        }
                    }
                    ::close(fd);
                }
                if (reused) {
                    map_.setLayoutFromHeader();
                    //Continue after the highest message index found in the ring: a
                    //crashed publisher may have completed a slot without updating
                    //"published", and readers may already have delivered it.
                    //A slot left odd (crash during write, or the next slot pre-marked
                    //by a split/inline-premark layout publisher) is reset; its index
                    //is rewritten by this publisher.
                    uint64_t resume = map_.header->published.load(std::memory_order_acquire);
                    for (uint64_t ii=0; ii<count; ++ii) {
                        auto *sq = (layout_ == SHM_SEQLOCK_LAYOUT_SPLIT ? map_.seqAt<true>(ii) : map_.seqAt<false>(ii));
                        auto seq = sq->load(std::memory_order_relaxed);
                        if (seq & 1) {
                            sq->store(0, std::memory_order_relaxed);
                        } else if (seq != 0) {
                            resume = std::max(resume, seq/2);
                        }
                    }
                    //prefault
                    for (std::size_t off=0; off<size; off+=4096) {
                        (void) *(reinterpret_cast<volatile char *>(map_.base)+off);
                    }
                    map_.header->flags.store(futex_ ? SHM_SEQLOCK_FLAG_FUTEX : 0, std::memory_order_relaxed);
                    map_.header->published.store(resume, std::memory_order_release);
                    map_.header->epoch.fetch_add(1, std::memory_order_acq_rel);
                } else {
                    //Readers still mapping an old segment keep their mapping alive
                    //and detect the replacement through the inode change
                    ::shm_unlink(shmName_.c_str());
                    fd = ::shm_open(shmName_.c_str(), O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC, 0666);
                    if (fd < 0) {
                        fail(std::string("cannot create shared memory segment: ")+std::strerror(errno));
                    }
                    (void) ::fchmod(fd, 0666);
                    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
                        int e = errno;
                        ::close(fd);
                        ::shm_unlink(shmName_.c_str());
                        fail(std::string("cannot size shared memory segment: ")+std::strerror(e));
                    }
                    struct stat st;
                    ::fstat(fd, &st);
                    if (!map_.map(fd, size, doMlock)) {
                        int e = errno;
                        ::close(fd);
                        ::shm_unlink(shmName_.c_str());
                        fail(std::string("cannot map shared memory segment: ")+std::strerror(e));
                    }
                    ::close(fd);
                    map_.inode = st.st_ino;
                    //prefault by writing (the segment is already zero filled)
                    std::memset(map_.slots, 0, count*stride);
                    auto *h = map_.header;
                    h->layoutVersion = layout_;
                    h->slotCapacity = static_cast<uint32_t>(capacity);
                    h->slotCount = static_cast<uint32_t>(count);
                    h->slotStride = static_cast<uint32_t>(stride);
                    h->flags.store(futex_ ? SHM_SEQLOCK_FLAG_FUTEX : 0, std::memory_order_relaxed);
                    h->epoch.store(1, std::memory_order_relaxed);
                    h->published.store(0, std::memory_order_relaxed);
                    h->futexWord.store(0, std::memory_order_relaxed);
                    h->waiters.store(0, std::memory_order_relaxed);
                    h->magic.store(SHM_SEQLOCK_MAGIC, std::memory_order_release);
                    map_.setLayoutFromHeader();
                }
                next_ = map_.header->published.load(std::memory_order_acquire);
                if (layout_ == SHM_SEQLOCK_LAYOUT_SPLIT && !noPremark_) {
                    //split layout: the slot of the next message is always pre-marked odd
                    //before its data is written, so that its polled seq line gets a single
                    //writer store (the commit) while readers wait for it
                    map_.seqAt<true>(next_)->store(2*next_+1, std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_release);
                }
                if (layout_ == SHM_SEQLOCK_LAYOUT_INLINE_PREMARK && !noPremark_) {
                    //inline-premark layout: same as split, the next slot is pre-marked
                    //odd so a polling reader sees the pre-mark immediately; the polled
                    //line is line 0 (shared with the slot head and first data bytes)
                    map_.seqAt<false>(next_)->store(2*next_+1, std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_release);
                }
            }
            ~OneShmSeqlockSender() {
                //The segment is left in place so that a restarted publisher continues it
                map_.unmap();
                if (lockFd_ >= 0) {
                    ::flock(lockFd_, LOCK_UN);
                    ::close(lockFd_);
                }
            }
            bool futex() const {
                return futex_;
            }
            bool writerLock() const {
                return writerLock_;
            }
            bool split() const {
                return layout_ == SHM_SEQLOCK_LAYOUT_SPLIT;
            }
            bool inlinePremark() const {
                return layout_ == SHM_SEQLOCK_LAYOUT_INLINE_PREMARK;
            }
            bool noPremark() const {
                return noPremark_;
            }
            template <bool Futex, bool Lock, bool Split, bool InlinePremark, bool Premark>
            void publish(std::string const &topic, std::string const &content) {
                std::size_t tl = topic.size();
                std::size_t pl = content.size();
                if (tl+pl > map_.slotCapacity) {
                    reject(tl, pl);
                    return;
                }
                if constexpr (Lock) {
                    while (writing_.test_and_set(std::memory_order_acquire)) {
                        cpuPause();
                    }
                }
                if (tl != lastTopic_.size() || std::memcmp(topic.data(), lastTopic_.data(), tl) != 0) {
                    lastTopic_ = topic;
                    lastTopicHash_ = topicHash(topic.data(), tl);
                }
                uint64_t i = next_;
                auto *s = map_.headAt<Split>(i);
                auto *sq = map_.seqAt<Split>(i);
                char *d = ShmSeqlockMapping::dataOf(s);
                if constexpr (!Split && !InlinePremark) {
                    //inline: odd mark first, then write everything, then commit
                    sq->store(2*i+1, std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_release);
                    s->payloadLen.store(static_cast<uint32_t>(pl), std::memory_order_relaxed);
                    s->topicLen.store(static_cast<uint32_t>(tl), std::memory_order_relaxed);
                    s->topicHash.store(lastTopicHash_, std::memory_order_relaxed);
                    std::memcpy(d, topic.data(), tl);
                    std::memcpy(d+tl, content.data(), pl);
                    sq->store(2*i+2, std::memory_order_release);
                } else if constexpr (InlinePremark) {
                    //inline-premark: slot was pre-marked odd after the previous commit.
                    //Write data beyond line 0 first (bytes >= LINE0_DATA_BYTES from the
                    //start of the data region), so the cold RFOs happen before the
                    //line-0 burst. Then write lengths + topic + first LINE0_DATA_BYTES
                    //+ even commit seq to line 0 in one burst.
                    //Data layout: d[0..tl) = topic, d[tl..tl+pl) = content.
                    constexpr std::size_t L0 = SHM_SEQLOCK_LINE0_DATA_BYTES;
                    std::size_t total = tl+pl;
                    if (total > L0) {
                        //write d[L0..total) now: may span topic and/or content
                        std::size_t tail = total-L0;
                        if (L0 >= tl) {
                            //L0 falls inside content
                            std::memcpy(d+L0, content.data()+(L0-tl), tail);
                        } else {
                            //L0 falls inside topic; tail = topic[L0..tl) + content[0..pl)
                            std::size_t topicTail = tl-L0;
                            std::memcpy(d+L0, topic.data()+L0, topicTail);
                            if (pl > 0) {
                                std::memcpy(d+tl, content.data(), pl);
                            }
                        }
                        std::atomic_thread_fence(std::memory_order_release);
                    }
                    //burst line 0: lengths, topicHash, then topic+content prefix
                    s->payloadLen.store(static_cast<uint32_t>(pl), std::memory_order_relaxed);
                    s->topicLen.store(static_cast<uint32_t>(tl), std::memory_order_relaxed);
                    s->topicHash.store(lastTopicHash_, std::memory_order_relaxed);
                    std::size_t l0copy = (total < L0 ? total : L0);
                    if (l0copy <= tl) {
                        std::memcpy(d, topic.data(), l0copy);
                    } else {
                        std::memcpy(d, topic.data(), tl);
                        std::memcpy(d+tl, content.data(), l0copy-tl);
                    }
                    sq->store(2*i+2, std::memory_order_release);
                } else {
                    //split: the slot was already pre-marked odd after the previous commit
                    //(or when this publisher started), the commit is its only store here
                    s->payloadLen.store(static_cast<uint32_t>(pl), std::memory_order_relaxed);
                    s->topicLen.store(static_cast<uint32_t>(tl), std::memory_order_relaxed);
                    s->topicHash.store(lastTopicHash_, std::memory_order_relaxed);
                    std::memcpy(d, topic.data(), tl);
                    std::memcpy(d+tl, content.data(), pl);
                    sq->store(2*i+2, std::memory_order_release);
                }
                next_ = i+1;
                map_.header->published.store(i+1, std::memory_order_release);
                if constexpr ((Split || InlinePremark) && Premark) {
                    //pre-mark the next slot while readers are still busy with this message;
                    //the fence orders it before any data store of the next message
                    map_.seqAt<Split>(i+1)->store(2*i+3, std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_release);
                }
                if constexpr (Lock) {
                    writing_.clear(std::memory_order_release);
                }
                if constexpr (Futex) {
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    if (map_.header->waiters.load(std::memory_order_relaxed) != 0) {
                        map_.header->futexWord.fetch_add(1, std::memory_order_release);
                        if (futexWakeAll(&(map_.header->futexWord)) > 0) {
                            emptyWakes_.store(0, std::memory_order_relaxed);
                        } else if (emptyWakes_.fetch_add(1, std::memory_order_relaxed)+1 >= SHM_SEQLOCK_EMPTY_WAKES_BEFORE_RESET) {
                            //registrations leaked by killed readers; a live reader
                            //caught by the reset is woken by its futex timeout
                            map_.header->waiters.store(0, std::memory_order_relaxed);
                            emptyWakes_.store(0, std::memory_order_relaxed);
                            waiterResets_.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
            ShmSeqlockPublisherStats stats() const {
                return {
                    map_.header->published.load(std::memory_order_relaxed)
                    , rejected_.load(std::memory_order_relaxed)
                    , map_.header->epoch.load(std::memory_order_relaxed)
                    , waiterResets_.load(std::memory_order_relaxed)
                };
            }
        };

        class OneShmSeqlockSubscription {
        private:
            struct ClientCB {
                uint32_t id;
                std::function<void(basic::ByteDataWithTopic &&)> cb;
                std::optional<WireToUserHook> hook;
            };
            struct StringMatchClient {
                std::string topic;
                uint32_t hash;
                ClientCB cb;
            };
            struct ClientSet {
                std::vector<ClientCB> noFilterClients;
                std::vector<StringMatchClient> stringMatchClients;
                std::vector<std::tuple<std::regex, ClientCB>> regexMatchClients;
                std::size_t size() const {
                    return noFilterClients.size()+stringMatchClients.size()+regexMatchClients.size();
                }
            };

            ConnectionLocator locator_;
            std::string shmName_;
            std::string lockPath_;
            bool spec_;
            bool futex_;
            bool joinLatest_;
            bool doMlock_;
            uint64_t spinCount_;
            long futexTimeoutUs_;
            uint64_t pauseAfter_;

            ShmSeqlockMapping map_;
            std::unique_ptr<char[]> buf_;
            uint64_t next_;
            uint64_t lastEpoch_;
            uint64_t idleChecks_;
            //speculative copy length, always <= slot capacity
            uint32_t predict_;

            //only touched by the reader thread on the hot path
            ClientSet active_;
            uint64_t activeVersion_;

            //client changes are published to the reader thread through
            //version_; the reader copies pending_ when it sees a new version
            std::mutex mutex_;
            ClientSet pending_;
            alignas(64) std::atomic<uint64_t> version_;
            alignas(64) std::atomic<uint64_t> ackVersion_;

            alignas(64) std::atomic<uint64_t> received_;
            std::atomic<uint64_t> lost_;
            std::atomic<uint64_t> lapResyncs_;
            std::atomic<uint64_t> publisherRestarts_;
            std::atomic<uint64_t> corrupt_;
            std::atomic<uint64_t> reopens_;
            std::atomic<uint64_t> splitLayout_;

            alignas(64) std::atomic<bool> running_;
            std::thread th_;
            std::thread::id thId_;
            thread_cpu_lease_detail::BindRequest bindRequest_;
            bool warnedCpuMismatch_ = false;

            static void bump(std::atomic<uint64_t> &x, uint64_t d=1) {
                //single writer (the reader thread)
                x.store(x.load(std::memory_order_relaxed)+d, std::memory_order_relaxed);
            }

            bool publisherAlive() const {
                int fd = ::open(lockPath_.c_str(), O_RDONLY|O_CLOEXEC);
                if (fd < 0) {
                    return false;
                }
                bool alive = false;
                if (::flock(fd, LOCK_SH|LOCK_NB) == 0) {
                    ::flock(fd, LOCK_UN);
                } else if (errno == EWOULDBLOCK) {
                    alive = true;
                }
                ::close(fd);
                return alive;
            }
            bool tryOpen() {
                int fd = ::shm_open(shmName_.c_str(), O_RDWR|O_CLOEXEC, 0);
                if (fd < 0) {
                    return false;
                }
                bool ok = false;
                struct stat st;
                if (::fstat(fd, &st) == 0
                    && static_cast<std::size_t>(st.st_size) >= sizeof(ShmSeqlockHeader)
                    && map_.map(fd, static_cast<std::size_t>(st.st_size), doMlock_)) {
                    auto *h = map_.header;
                    if (h->magic.load(std::memory_order_acquire) == SHM_SEQLOCK_MAGIC
                        && (h->layoutVersion == SHM_SEQLOCK_LAYOUT_INLINE
                            || h->layoutVersion == SHM_SEQLOCK_LAYOUT_SPLIT
                            || h->layoutVersion == SHM_SEQLOCK_LAYOUT_INLINE_PREMARK)) {
                        std::size_t count = h->slotCount;
                        std::size_t capacity = h->slotCapacity;
                        std::size_t stride = h->slotStride;
                        if (count >= 2 && (count & (count-1)) == 0
                            && stride >= (h->layoutVersion == SHM_SEQLOCK_LAYOUT_SPLIT ? SHM_SEQLOCK_SPLIT_HEAD_OFFSET : 0)+sizeof(ShmSeqlockSlotHead)+capacity
                            && segmentSize(count, stride) == static_cast<std::size_t>(st.st_size)) {
                            ok = true;
                        }
                    }
                    if (ok) {
                        map_.inode = st.st_ino;
                        map_.setLayoutFromHeader();
                    } else {
                        map_.unmap();
                    }
                }
                ::close(fd);
                return ok;
            }
            //blocks until the segment exists and is initialized, or the reader is stopped
            bool openAndJoin() {
                map_.unmap();
                while (running_.load(std::memory_order_relaxed)) {
                    if (tryOpen()) {
                        buf_ = std::make_unique<char[]>(map_.slotCapacity+64);
                        predict_ = static_cast<uint32_t>(std::min<std::size_t>(map_.slotCapacity, 320));
                        uint64_t p = map_.header->published.load(std::memory_order_acquire);
                        //"latest" join: the most recent message is delivered first, as long as
                        //its publisher is alive (a dead publisher's last message is stale)
                        if (joinLatest_ && p > 0 && publisherAlive()) {
                            next_ = p-1;
                        } else {
                            next_ = p;
                        }
                        lastEpoch_ = map_.header->epoch.load(std::memory_order_relaxed);
                        splitLayout_.store(map_.split ? 1 : 0, std::memory_order_relaxed);
                        if (futex_ && !(map_.header->flags.load(std::memory_order_relaxed) & SHM_SEQLOCK_FLAG_FUTEX)) {
                            std::cerr << "[ShmSeqlockComponent] segment '" << locator_.identifier()
                                << "': reader uses wait=futex but the publisher does not, readers will poll every 1ms when idle\n";
                        }
                        return true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                return false;
            }
            //called only when the ring is idle; returns false if the loop must exit (stop, or reopen)
            bool idleCheck(bool withStat) {
                auto *h = map_.header;
                bool reopen = (h->magic.load(std::memory_order_acquire) != SHM_SEQLOCK_MAGIC);
                if (!reopen && withStat) {
                    struct stat st;
                    std::string path = "/dev/shm"+shmName_;
                    if (::stat(path.c_str(), &st) == 0 && st.st_ino != map_.inode) {
                        reopen = true;
                    }
                }
                if (reopen) {
                    bump(reopens_);
                    bump(publisherRestarts_);
                    //the new segment may use another layout, run() reopens it
                    return false;
                }
                auto e = h->epoch.load(std::memory_order_relaxed);
                if (e != lastEpoch_) {
                    bump(publisherRestarts_, e-lastEpoch_);
                    lastEpoch_ = e;
                }
                return true;
            }
            void resync() {
                uint64_t p = map_.header->published.load(std::memory_order_acquire);
                uint64_t latest = (p > 0 ? p-1 : 0);
                bump(lapResyncs_);
                if (latest > next_) {
                    bump(lost_, latest-next_);
                    next_ = latest;
                } else {
                    //only reachable if the segment content is inconsistent
                    bump(corrupt_);
                    next_ = p;
                }
            }
            void refreshClients() {
                std::lock_guard<std::mutex> _(mutex_);
                active_ = pending_;
                activeVersion_ = version_.load(std::memory_order_relaxed);
                ackVersion_.store(activeVersion_, std::memory_order_release);
            }
            static void callClient(ClientCB const &c, char const *buf, uint32_t tl, uint32_t pl) {
                if (c.hook) {
                    auto b = (c.hook->hook)(basic::ByteDataView {std::string_view(buf+tl, pl)});
                    if (b) {
                        c.cb({std::string(buf, tl), std::move(b->content)});
                    }
                } else {
                    c.cb({std::string(buf, tl), std::string(buf+tl, pl)});
                }
            }
            void dispatch(char const *buf, uint32_t tl, uint32_t pl, uint32_t hash) {
                if (version_.load(std::memory_order_acquire) != activeVersion_) {
                    refreshClients();
                }
                for (auto const &c : active_.noFilterClients) {
                    callClient(c, buf, tl, pl);
                }
                for (auto const &c : active_.stringMatchClients) {
                    if (c.hash == hash && c.topic.size() == tl && std::memcmp(c.topic.data(), buf, tl) == 0) {
                        callClient(c.cb, buf, tl, pl);
                    }
                }
                for (auto const &c : active_.regexMatchClients) {
                    if (std::regex_match(buf, buf+tl, std::get<0>(c))) {
                        callClient(std::get<1>(c), buf, tl, pl);
                    }
                }
            }
            template <bool Split, bool InlinePremark>
            void futexSleep(std::atomic<uint64_t> *sq, uint64_t want) {
                auto *h = map_.header;
                long timeout = futexTimeoutUs_;
                if (!(h->flags.load(std::memory_order_relaxed) & SHM_SEQLOCK_FLAG_FUTEX)) {
                    timeout = std::min(timeout, 1000L);
                }
                uint32_t fw = h->futexWord.load(std::memory_order_acquire);
                h->waiters.fetch_add(1, std::memory_order_seq_cst);
                //pairs with the publisher's seq_cst fence before it reads waiters.
                //split and inline-premark: a pre-marked slot (want-1) is the normal
                //idle state, so sleep until the commit.
                //inline: want-1 means a write is in progress.
                if (sq->load(std::memory_order_seq_cst) < ((Split || InlinePremark) ? want : want-1) && running_.load(std::memory_order_relaxed)) {
                    futexWait(&(h->futexWord), fw, timeout);
                }
                //saturating: the publisher may have reset the count meanwhile
                uint32_t w = h->waiters.load(std::memory_order_relaxed);
                while (w != 0 && !h->waiters.compare_exchange_weak(w, w-1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                }
            }

            template <bool Spec, bool Futex, bool Split, bool InlinePremark>
            void runLoop() {
                uint64_t empty = 0;
                while (running_.load(std::memory_order_relaxed)) {
                    auto *s = map_.headAt<Split>(next_);
                    auto *sq = map_.seqAt<Split>(next_);
                    uint64_t want = 2*next_+2;
                    uint64_t seq1 = sq->load(std::memory_order_acquire);
                    bool got = false;
                    uint32_t tl = 0, pl = 0, hash = 0;
                    if constexpr (Spec) {
                        //copy without waiting for seq1, validate afterwards. The
                        //first copy uses a locally predicted length (size of the
                        //previous message), so its loads do not depend on the
                        //slot head line and all lines are fetched in parallel;
                        //only a longer message needs a second, dependent copy.
                        char *src = ShmSeqlockMapping::dataOf(s);
                        uint32_t pre = predict_;
                        std::memcpy(buf_.get(), src, pre);
                        tl = s->topicLen.load(std::memory_order_relaxed);
                        pl = s->payloadLen.load(std::memory_order_relaxed);
                        hash = s->topicHash.load(std::memory_order_relaxed);
                        uint32_t cap = static_cast<uint32_t>(map_.slotCapacity);
                        tl = std::min(tl, cap);
                        pl = std::min(pl, cap-tl);
                        if (tl+pl > pre) {
                            std::memcpy(buf_.get()+pre, src+pre, tl+pl-pre);
                        }
                        std::atomic_thread_fence(std::memory_order_acquire);
                        uint64_t seq2 = sq->load(std::memory_order_relaxed);
                        got = (seq1 == want && seq2 == want);
                        seq1 = std::max(seq1, seq2);
                    } else {
                        if (seq1 == want) {
                            tl = s->topicLen.load(std::memory_order_relaxed);
                            pl = s->payloadLen.load(std::memory_order_relaxed);
                            hash = s->topicHash.load(std::memory_order_relaxed);
                            uint32_t cap = static_cast<uint32_t>(map_.slotCapacity);
                            tl = std::min(tl, cap);
                            pl = std::min(pl, cap-tl);
                            std::memcpy(buf_.get(), ShmSeqlockMapping::dataOf(s), tl+pl);
                            std::atomic_thread_fence(std::memory_order_acquire);
                            uint64_t seq2 = sq->load(std::memory_order_relaxed);
                            got = (seq2 == want);
                            seq1 = seq2;
                        }
                    }
                    if (got) {
                        empty = 0;
                        //the stat() in idleCheck needs a long consecutive idle period
                        idleChecks_ = 0;
                        if constexpr (Spec) {
                            predict_ = tl+pl;
                        }
                        ++next_;
                        bump(received_);
                        dispatch(buf_.get(), tl, pl, hash);
                        continue;
                    }
                    if (seq1 > want) {
                        empty = 0;
                        resync();
                        continue;
                    }
                    ++empty;
                    if constexpr (Futex) {
                        if (empty >= spinCount_) {
                            futexSleep<Split, InlinePremark>(sq, want);
                            //sleep again on the next empty poll; a delivery resets it
                            empty = spinCount_-1;
                            if (!idleCheck((++idleChecks_ & 15) == 0)) {
                                return;
                            }
                            continue;
                        }
                    } else {
                        if (pauseAfter_ != 0 && empty > pauseAfter_) {
                            cpuPause();
                        }
                    }
                    //idle only: a header load every 4096 empty polls, a stat() of the
                    //segment name (detects manual deletion+recreation) every 2^24
                    if ((empty & 4095) == 0) {
                        if (!idleCheck((++idleChecks_ & 4095) == 0)) {
                            return;
                        }
                    }
                }
            }
            template <bool Spec, bool Futex>
            void runWithLayout() {
                if (map_.split) {
                    runLoop<Spec, Futex, true, false>();
                } else if (map_.inlinePremark) {
                    runLoop<Spec, Futex, false, true>();
                } else {
                    runLoop<Spec, Futex, false, false>();
                }
            }
            void run(thread_cpu_lease_detail::BindHandshake *handshake) {
                //first action: pin this thread, reports the result to the caller
                auto lease = thread_cpu_lease_detail::bindWorkerThread(&bindRequest_, handshake);
                if (!lease) {
                    return;
                }
                //the layout comes from the segment header and may change when the
                //publisher replaces the segment, so it is dispatched on every (re)open
                while (running_.load(std::memory_order_relaxed)) {
                    if (!openAndJoin()) {
                        return;
                    }
                    if (spec_) {
                        if (futex_) {
                            runWithLayout<true, true>();
                        } else {
                            runWithLayout<true, false>();
                        }
                    } else {
                        if (futex_) {
                            runWithLayout<false, true>();
                        } else {
                            runWithLayout<false, false>();
                        }
                    }
                }
            }
        public:
            OneShmSeqlockSubscription(ConnectionLocator const &locator, thread_cpu_lease_detail::BindRequest bindRequest)
                : locator_(locator), shmName_(shmNameFor(locator)), lockPath_()
                //read=classic is the default: in the pinned IPC profiling matrix
                //(2026-10-26) it beat read=spec on every tail metric, more so with
                //more readers: 1 reader p50/p90/p99 classic 356/470/545ns vs spec
                //318/555/692ns; 4 readers p90/p99 classic 629/731ns vs spec 965/1155ns.
                //Spec also raised the publisher's publish() p99 (266-465ns vs 213-277ns),
                //most likely because a spinning speculative reader keeps pulling the next
                //slot's lines into its cache, so every publisher write pays an extra
                //ownership transfer. read=spec stays available for comparison.
                , spec_(locator.query("read", "classic") == "spec")
                , futex_(locator.query("wait", "spin") == "futex")
                , joinLatest_(locator.query("join", "latest") != "next")
                , doMlock_(queryBool(locator, "mlock", true))
                , spinCount_(std::max<std::size_t>(1, queryNumber(locator, "spinCount", 20000)))
                , futexTimeoutUs_(static_cast<long>(std::max<std::size_t>(1, queryNumber(locator, "futexTimeoutUs", 100000))))
                , pauseAfter_(queryNumber(locator, "pauseAfter", 0))
                , map_(), buf_(), next_(0), lastEpoch_(0), idleChecks_(0), predict_(0)
                , active_(), activeVersion_(0)
                , mutex_(), pending_(), version_(0), ackVersion_(0)
                , received_(0), lost_(0), lapResyncs_(0), publisherRestarts_(0), corrupt_(0), reopens_(0), splitLayout_(0)
                , running_(true), th_(), thId_()
                , bindRequest_(std::move(bindRequest))
            {
                lockPath_ = "/dev/shm"+shmName_+".lock";
            }
            ~OneShmSeqlockSubscription() {
                running_ = false;
                if (th_.joinable()) {
                    if (std::this_thread::get_id() == thId_) {
                        th_.detach();
                    } else {
                        th_.join();
                    }
                }
                map_.unmap();
            }
            ConnectionLocator const &locator() const {
                return locator_;
            }
            bool isReaderThread() const {
                return std::this_thread::get_id() == thId_;
            }
            void addSubscription(
                uint32_t id
                , std::variant<ShmSeqlockComponent::NoTopicSelection, std::string, std::regex> const &topic
                , std::function<void(basic::ByteDataWithTopic &&)> handler
                , std::optional<WireToUserHook> wireToUserHook
            ) {
                std::lock_guard<std::mutex> _(mutex_);
                switch (topic.index()) {
                case 0:
                    pending_.noFilterClients.push_back({id, handler, wireToUserHook});
                    break;
                case 1:
                    {
                        auto const &t = std::get<std::string>(topic);
                        pending_.stringMatchClients.push_back({t, topicHash(t.data(), t.size()), {id, handler, wireToUserHook}});
                    }
                    break;
                case 2:
                    pending_.regexMatchClients.push_back({std::get<std::regex>(topic), {id, handler, wireToUserHook}});
                    break;
                default:
                    break;
                }
                version_.fetch_add(1, std::memory_order_release);
                //started only once there is a client, so the first (latest) message
                //is never dispatched to an empty client set
                if (!th_.joinable()) {
                    std::optional<thread_cpu_lease_detail::BindHandshake> handshake;
                    if (bindRequest_.needed) {
                        handshake.emplace();
                    }
                    th_ = std::thread(&OneShmSeqlockSubscription::run, this, handshake ? &*handshake : nullptr);
                    thId_ = th_.get_id();
                    if (handshake) {
                        try {
                            handshake->future.get();
                        } catch (...) {
                            //the reader thread has already returned
                            th_.join();
                            thId_ = std::thread::id();
                            throw;
                        }
                    }
                }
            }
            //The reader thread picks up the change before its next delivery, so a
            //removed callback may still receive the message being dispatched when
            //this is called (same as the other transports' at-most-one race).
            void removeSubscription(uint32_t id) {
                std::lock_guard<std::mutex> _(mutex_);
                auto &a = pending_.noFilterClients;
                a.erase(std::remove_if(a.begin(), a.end(), [id](auto const &x) {return x.id == id;}), a.end());
                auto &b = pending_.stringMatchClients;
                b.erase(std::remove_if(b.begin(), b.end(), [id](auto const &x) {return x.cb.id == id;}), b.end());
                auto &c = pending_.regexMatchClients;
                c.erase(std::remove_if(c.begin(), c.end(), [id](auto const &x) {return std::get<1>(x).id == id;}), c.end());
                version_.fetch_add(1, std::memory_order_release);
            }
            bool checkWhetherNeedsToStop() {
                std::lock_guard<std::mutex> _(mutex_);
                if (pending_.size() == 0) {
                    running_ = false;
                    return true;
                } else {
                    return false;
                }
            }
            std::thread::native_handle_type getThreadHandle() {
                return th_.native_handle();
            }
            thread_cpu_lease_detail::BindRequest const &bindRequest() const {
                return bindRequest_;
            }
            //There is one reader thread per segment: the first locator decides
            //the binding, later different values only warn once.
            void warnCpuMismatch(ConnectionLocator const &locator, thread_cpu_lease_detail::BindRequest const &request) {
                if (warnedCpuMismatch_) {
                    return;
                }
                warnedCpuMismatch_ = true;
                std::cerr << "shm_seqlock: reader thread for " << locator_.toPrintFormat()
                    << " already uses cpu='" << bindRequest_.spec
                    << "', ignoring cpu='" << request.spec << "' requested by "
                    << locator.toPrintFormat() << std::endl;
            }
            ShmSeqlockReaderStats stats() const {
                return {
                    received_.load(std::memory_order_relaxed)
                    , lost_.load(std::memory_order_relaxed)
                    , lapResyncs_.load(std::memory_order_relaxed)
                    , publisherRestarts_.load(std::memory_order_relaxed)
                    , corrupt_.load(std::memory_order_relaxed)
                    , reopens_.load(std::memory_order_relaxed)
                    , splitLayout_.load(std::memory_order_relaxed)
                };
            }
        };

        std::unordered_map<ConnectionLocator, std::unique_ptr<OneShmSeqlockSubscription>> subscriptions_;
        std::unordered_map<ConnectionLocator, std::unique_ptr<OneShmSeqlockSender>> senders_;
        //subscriptions stopped from inside their own reader thread, destroyed later
        std::vector<std::unique_ptr<OneShmSeqlockSubscription>> stoppedSubscriptions_;
        std::mutex mutex_;

        //the binding provider, settable once before the graph starts
        std::mutex providerMutex_;
        ThreadCpuLeaseProvider provider_;
        bool providerSet_ = false;

        ThreadCpuLeaseProvider getThreadCpuLeaseProvider() {
            std::lock_guard<std::mutex> _(providerMutex_);
            return provider_;
        }
        uint32_t counter_;
        std::unordered_map<uint32_t, OneShmSeqlockSubscription *> idToSubscriptionMap_;
        std::mutex idMutex_;

        static ConnectionLocator idOnlyLocator(ConnectionLocator const &d) {
            return ConnectionLocator {"", 0, "", "", d.identifier()};
        }
        //caller holds mutex_
        OneShmSeqlockSubscription *getOrStartSubscription_locked(ConnectionLocator const &d
            , thread_cpu_lease_detail::BindRequest const &bindRequest, bool &created) {
            created = false;
            auto idOnly = idOnlyLocator(d);
            auto iter = subscriptions_.find(idOnly);
            if (iter == subscriptions_.end()) {
                iter = subscriptions_.insert({idOnly, std::make_unique<OneShmSeqlockSubscription>(d, bindRequest)}).first;
                created = true;
            } else if (!thread_cpu_lease_detail::sameCpuBinding(iter->second->bindRequest(), bindRequest)) {
                iter->second->warnCpuMismatch(d, bindRequest);
            }
            return iter->second.get();
        }
        void potentiallyStopSubscription(OneShmSeqlockSubscription *p) {
            //the reader thread is joined outside mutex_, since its callbacks may
            //call back into this component
            std::unique_ptr<OneShmSeqlockSubscription> toDestroy;
            {
                std::lock_guard<std::mutex> _(mutex_);
                //two concurrent removals of the last clients may both get here;
                //the second one must not touch the already destroyed subscription
                auto iter = subscriptions_.begin();
                for (; iter != subscriptions_.end(); ++iter) {
                    if (iter->second.get() == p) {
                        break;
                    }
                }
                if (iter == subscriptions_.end()) {
                    return;
                }
                if (p->checkWhetherNeedsToStop()) {
                    if (p->isReaderThread()) {
                        stoppedSubscriptions_.push_back(std::move(iter->second));
                    } else {
                        toDestroy = std::move(iter->second);
                    }
                    subscriptions_.erase(iter);
                }
            }
        }
        OneShmSeqlockSender *getOrStartSender(ConnectionLocator const &d) {
            auto idOnly = idOnlyLocator(d);
            std::lock_guard<std::mutex> _(mutex_);
            auto iter = senders_.find(idOnly);
            if (iter == senders_.end()) {
                iter = senders_.insert({idOnly, std::make_unique<OneShmSeqlockSender>(d)}).first;
            }
            return iter->second.get();
        }
        template <bool Futex, bool Lock, bool Split, bool InlinePremark, bool Premark>
        static std::function<void(basic::ByteDataWithTopic &&)> makePublisher(OneShmSeqlockSender *p, std::optional<UserToWireHook> const &userToWireHook) {
            if (userToWireHook) {
                auto hook = userToWireHook->hook;
                return [p,hook](basic::ByteDataWithTopic &&data) {
                    auto w = hook(basic::ByteData {std::move(data.content)});
                    p->template publish<Futex,Lock,Split,InlinePremark,Premark>(data.topic, w.content);
                };
            } else {
                return [p](basic::ByteDataWithTopic &&data) {
                    p->template publish<Futex,Lock,Split,InlinePremark,Premark>(data.topic, data.content);
                };
            }
        }
        template <bool Futex, bool Lock>
        static std::function<void(basic::ByteDataWithTopic &&)> makePublisherForLayout(OneShmSeqlockSender *p, std::optional<UserToWireHook> const &userToWireHook) {
            if (p->split()) {
                if (!p->noPremark()) {
                    return makePublisher<Futex,Lock,true,false,true>(p, userToWireHook);
                } else {
                    //test control only: split layout without the pre-mark, which is
                    //not a valid seqlock (torn reads are not detected)
                    return makePublisher<Futex,Lock,true,false,false>(p, userToWireHook);
                }
            } else if (p->inlinePremark()) {
                if (!p->noPremark()) {
                    return makePublisher<Futex,Lock,false,true,true>(p, userToWireHook);
                } else {
                    //test control only: inline-premark without the pre-mark
                    return makePublisher<Futex,Lock,false,true,false>(p, userToWireHook);
                }
            } else {
                //plain inline: no pre-mark template parameter needed (always no premark)
                return makePublisher<Futex,Lock,false,false,true>(p, userToWireHook);
            }
        }
    public:
        ShmSeqlockComponentImpl()
            : subscriptions_(), senders_(), stoppedSubscriptions_(), mutex_()
            , counter_(0), idToSubscriptionMap_(), idMutex_()
        {
        }
        ~ShmSeqlockComponentImpl() {
            //reader threads are joined outside mutex_ (callbacks may re-enter)
            std::unordered_map<ConnectionLocator, std::unique_ptr<OneShmSeqlockSubscription>> subs;
            std::vector<std::unique_ptr<OneShmSeqlockSubscription>> stopped;
            {
                std::lock_guard<std::mutex> _(mutex_);
                subs.swap(subscriptions_);
                stopped.swap(stoppedSubscriptions_);
            }
            subs.clear();
            stopped.clear();
            std::lock_guard<std::mutex> _(mutex_);
            senders_.clear();
        }
        void setThreadCpuLease(ThreadCpuLeaseProvider provider) {
            std::lock_guard<std::mutex> _(providerMutex_);
            if (providerSet_) {
                throw std::logic_error("shm_seqlock: thread CPU lease provider is already set");
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
            std::variant<ShmSeqlockComponent::NoTopicSelection, std::string, std::regex> const &topic,
            std::function<void(basic::ByteDataWithTopic &&)> client,
            std::optional<WireToUserHook> wireToUserHook) {
            auto bindRequest = thread_cpu_lease_detail::makeThreadCpuBindRequest(
                locator, "cpu", "shm_seqlock reader "+locator.identifier(), getThreadCpuLeaseProvider()
                , "shm_seqlock", "shm_seqlock_setThreadCpuLease");
            //mutex_ is held until the client is registered, so that a concurrent
            //potentiallyStopSubscription cannot destroy the subscription in between
            std::lock_guard<std::mutex> _(mutex_);
            bool created = false;
            auto *p = getOrStartSubscription_locked(locator, bindRequest, created);
            std::lock_guard<std::mutex> _id(idMutex_);
            ++counter_;
            try {
                p->addSubscription(counter_, topic, client, wireToUserHook);
            } catch (...) {
                //the reader thread failed to start (e.g. CPU binding failed):
                //drop the subscription we just created so no half-started object
                //is left in the cache
                if (created) {
                    auto iter = subscriptions_.find(idOnlyLocator(locator));
                    if (iter != subscriptions_.end() && iter->second.get() == p) {
                        subscriptions_.erase(iter);
                    }
                }
                throw;
            }
            idToSubscriptionMap_[counter_] = p;
            return counter_;
        }
        void removeSubscriptionClient(uint32_t id) {
            OneShmSeqlockSubscription *p = nullptr;
            {
                std::lock_guard<std::mutex> _(idMutex_);
                auto iter = idToSubscriptionMap_.find(id);
                if (iter == idToSubscriptionMap_.end()) {
                    return;
                }
                p = iter->second;
                idToSubscriptionMap_.erase(iter);
            }
            if (p != nullptr) {
                p->removeSubscription(id);
                potentiallyStopSubscription(p);
            }
        }
        std::function<void(basic::ByteDataWithTopic &&)> getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook) {
            auto *p = getOrStartSender(locator);
            if (p->futex()) {
                if (p->writerLock()) {
                    return makePublisherForLayout<true,true>(p, userToWireHook);
                } else {
                    return makePublisherForLayout<true,false>(p, userToWireHook);
                }
            } else {
                if (p->writerLock()) {
                    return makePublisherForLayout<false,true>(p, userToWireHook);
                } else {
                    return makePublisherForLayout<false,false>(p, userToWireHook);
                }
            }
        }
        std::unordered_map<ConnectionLocator, std::thread::native_handle_type> threadHandles() {
            std::unordered_map<ConnectionLocator, std::thread::native_handle_type> retVal;
            std::lock_guard<std::mutex> _(mutex_);
            for (auto &item : subscriptions_) {
                retVal[item.first] = item.second->getThreadHandle();
            }
            return retVal;
        }
        std::unordered_map<ConnectionLocator, ShmSeqlockReaderStats> readerStats() {
            std::unordered_map<ConnectionLocator, ShmSeqlockReaderStats> retVal;
            std::lock_guard<std::mutex> _(mutex_);
            for (auto &item : subscriptions_) {
                retVal[item.first] = item.second->stats();
            }
            return retVal;
        }
        std::unordered_map<ConnectionLocator, ShmSeqlockPublisherStats> publisherStats() {
            std::unordered_map<ConnectionLocator, ShmSeqlockPublisherStats> retVal;
            std::lock_guard<std::mutex> _(mutex_);
            for (auto &item : senders_) {
                retVal[item.first] = item.second->stats();
            }
            return retVal;
        }
    };

    ShmSeqlockComponent::ShmSeqlockComponent() : impl_(std::make_unique<ShmSeqlockComponentImpl>()) {}
    ShmSeqlockComponent::~ShmSeqlockComponent() {}
    uint32_t ShmSeqlockComponent::shm_seqlock_addSubscriptionClient(ConnectionLocator const &locator,
        std::variant<ShmSeqlockComponent::NoTopicSelection, std::string, std::regex> const &topic,
        std::function<void(basic::ByteDataWithTopic &&)> client,
        std::optional<WireToUserHook> wireToUserHook) {
        return impl_->addSubscriptionClient(locator, topic, client, wireToUserHook);
    }
    void ShmSeqlockComponent::shm_seqlock_removeSubscriptionClient(uint32_t id) {
        impl_->removeSubscriptionClient(id);
    }
    std::function<void(basic::ByteDataWithTopic &&)> ShmSeqlockComponent::shm_seqlock_getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook) {
        return impl_->getPublisher(locator, userToWireHook);
    }
    std::unordered_map<ConnectionLocator, std::thread::native_handle_type> ShmSeqlockComponent::shm_seqlock_threadHandles() {
        return impl_->threadHandles();
    }
    std::unordered_map<ConnectionLocator, ShmSeqlockReaderStats> ShmSeqlockComponent::shm_seqlock_readerStats() {
        return impl_->readerStats();
    }
    std::unordered_map<ConnectionLocator, ShmSeqlockPublisherStats> ShmSeqlockComponent::shm_seqlock_publisherStats() {
        return impl_->publisherStats();
    }
    void ShmSeqlockComponent::shm_seqlock_setThreadCpuLease(ThreadCpuLeaseProvider provider) {
        impl_->setThreadCpuLease(std::move(provider));
    }
    void ShmSeqlockComponent::shm_seqlock_setThreadCpuLeaseIfUnset(ThreadCpuLeaseProvider provider) {
        impl_->setThreadCpuLeaseIfUnset(std::move(provider));
    }

} } } } }
