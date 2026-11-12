#include <tm_kit/transport/aeron_ipc/AeronIpcComponent.hpp>

#include "../ThreadCpuLeaseSupport.hpp"

#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <limits>

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <cerrno>

extern "C" {
#include <aeronc.h>
#include <aeronmd.h>
}

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace aeron_ipc {

    namespace {
        //wire format of one message (one unfragmented Aeron frame):
        //  [uint32 topic length][topic bytes][content bytes]
        constexpr std::size_t kTopicLengthBytes = sizeof(uint32_t);
        constexpr std::size_t kAeronHeaderLength = 32;
        constexpr std::size_t kAeronMaxMtu = 65504;
        constexpr std::size_t kAeronMinTermLength = 64*1024;
        constexpr std::size_t kAeronMaxTermLength = 1024*1024*1024;

        inline void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
        //counters have a single writer, so a relaxed load+store is enough
        inline void bump(std::atomic<uint64_t> &c, uint64_t n=1) {
            c.store(c.load(std::memory_order_relaxed)+n, std::memory_order_relaxed);
        }
        inline std::size_t align32(std::size_t x) {
            return (x+31) & ~static_cast<std::size_t>(31);
        }
        inline std::size_t nextPow2Checked(std::size_t x) {
            std::size_t p = 1;
            while (p < x) {
                if (p > std::numeric_limits<std::size_t>::max()/2) {
                    throw std::invalid_argument("aeron_ipc: value is too large");
                }
                p <<= 1;
            }
            return p;
        }
        std::string lowerCase(std::string s) {
            for (auto &c : s) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        }
        bool queryBool(ConnectionLocator const &l, std::string const &field, bool defaultValue) {
            auto s = lowerCase(l.query(field, ""));
            if (s.empty()) {
                return defaultValue;
            }
            return (s == "true" || s == "1" || s == "yes" || s == "on");
        }
        uint64_t queryU64(ConnectionLocator const &l, std::string const &field, uint64_t defaultValue) {
            auto s = l.query(field, "");
            if (s.empty()) {
                return defaultValue;
            }
            try {
                return std::stoull(s);
            } catch (...) {
                throw std::invalid_argument("aeron_ipc: bad value for "+field+": "+s);
            }
        }
        int32_t streamIdFor(ConnectionLocator const &l) {
            auto s = l.query("stream", "");
            if (!s.empty()) {
                return static_cast<int32_t>(std::stol(s));
            }
            //FNV-1a of the channel name
            uint32_t h = 2166136261u;
            for (unsigned char c : l.identifier()) {
                h ^= c;
                h *= 16777619u;
            }
            return static_cast<int32_t>(h);
        }
        std::string aeronDirFor(ConnectionLocator const &l) {
            auto s = l.query("dir", "");
            if (!s.empty()) {
                return s;
            }
            char const *e = std::getenv("AERON_DIR");
            if (e != nullptr && *e != '\0') {
                return e;
            }
            char buf[4096];
            aeron_default_path(buf, sizeof(buf));
            return std::string(buf);
        }
        std::string aeronError(std::string const &what) {
            return "aeron_ipc: "+what+": "+aeron_errmsg();
        }
        void noLog(const char *) {}
        bool driverIsActive(std::string const &dir, uint64_t timeoutMs) {
            return aeron_is_driver_active(dir.c_str(), static_cast<int64_t>(timeoutMs), &noLog);
        }
        //True when the CnC file in dir names a driver process that no longer exists. Its
        //heartbeat can still look fresh for up to driverTimeoutMs after a SIGKILL.
        bool driverProcessIsDead(std::string const &dir) {
            aeron_cnc_t *cnc = nullptr;
            if (aeron_cnc_init(&cnc, dir.c_str(), 0) < 0) {
                return false;
            }
            aeron_cnc_constants_t c;
            bool dead = false;
            if (aeron_cnc_constants(cnc, &c) == 0 && c.pid > 0) {
                dead = (::kill(static_cast<pid_t>(c.pid), 0) != 0 && errno == ESRCH);
            }
            aeron_cnc_close(cnc);
            return dead;
        }
        bool driverIsLive(std::string const &dir, uint64_t timeoutMs) {
            return driverIsActive(dir, timeoutMs) && !driverProcessIsDead(dir);
        }
        std::string durationMs(uint64_t ms) {
            return std::to_string(ms)+"ms";
        }

        struct ChannelLayout {
            std::size_t slotSize;
            std::size_t mtu;
            std::size_t termLength;
        };
        ChannelLayout layoutFor(ConnectionLocator const &l) {
            ChannelLayout r;
            r.slotSize = queryU64(l, "slotSize", 1024);
            if (r.slotSize > kAeronMaxMtu-kTopicLengthBytes-kAeronHeaderLength) {
                throw std::invalid_argument(
                    "aeron_ipc: slotSize "+std::to_string(r.slotSize)+" too large, max is "
                    +std::to_string(kAeronMaxMtu-kAeronHeaderLength-kTopicLengthBytes)
                );
            }
            r.mtu = align32(r.slotSize+kTopicLengthBytes+kAeronHeaderLength);
            if (r.mtu > kAeronMaxMtu) {
                throw std::invalid_argument(
                    "aeron_ipc: slotSize "+std::to_string(r.slotSize)+" too large, max is "
                    +std::to_string(kAeronMaxMtu-kAeronHeaderLength-kTopicLengthBytes)
                );
            }
            auto explicitTerm = queryU64(l, "termLength", 0);
            if (explicitTerm != 0) {
                if (explicitTerm < kAeronMinTermLength || explicitTerm > kAeronMaxTermLength
                    || (explicitTerm & (explicitTerm-1)) != 0) {
                    throw std::invalid_argument("aeron_ipc: bad term length "+std::to_string(explicitTerm));
                }
                r.termLength = explicitTerm;
            } else {
                auto slotCount = queryU64(l, "slotCount", 8192);
                if (slotCount != 0 && slotCount > kAeronMaxTermLength/r.mtu) {
                    throw std::invalid_argument("aeron_ipc: slotCount is too large for the term length limit");
                }
                r.termLength = nextPow2Checked(std::max<std::size_t>(kAeronMinTermLength, r.mtu*slotCount));
            }
            //Aeron caps the message length at termLength/8
            r.termLength = std::max(r.termLength, nextPow2Checked(r.mtu*8));
            if (r.termLength > kAeronMaxTermLength || (r.termLength & (r.termLength-1)) != 0) {
                throw std::invalid_argument("aeron_ipc: bad term length "+std::to_string(r.termLength));
            }
            return r;
        }

        bool envSet(char const *name) {
            char const *e = std::getenv(name);
            return (e != nullptr && *e != '\0');
        }
    }

    class AeronIpcPublisher {
    private:
        ConnectionLocator locator_;
        std::string dir_;
        //driver thread binding: provider from the component plus the spec chosen
        //in startEmbeddedDriver. Leases stay here for the driver's lifetime.
        ThreadCpuLeaseProvider provider_;
        std::string driverCpuSpec_;
        bool driverCpuConfigured_ = false;
        std::mutex driverLeasesMutex_;
        std::vector<infra::ThreadCpuLease> driverLeases_;
        aeron_driver_context_t *driverContext_ = nullptr;
        aeron_driver_t *driver_ = nullptr;
        aeron_context_t *context_ = nullptr;
        aeron_t *client_ = nullptr;
        aeron_exclusive_publication_t *publication_ = nullptr;
        std::size_t slotSize_ = 0;
        std::size_t maxClaim_ = 0;
        bool retryOnBackPressure_ = false;

        std::atomic<uint64_t> published_ {0};
        std::atomic<uint64_t> notConnected_ {0};
        std::atomic<uint64_t> backPressured_ {0};
        std::atomic<uint64_t> adminAction_ {0};
        std::atomic<uint64_t> oversized_ {0};
        std::atomic<uint64_t> failed_ {0};
        std::atomic<uint64_t> driverBindFailed_ {0};

        static void onError(void *clientd, int errcode, const char *message) {
            auto *self = static_cast<AeronIpcPublisher *>(clientd);
            std::cerr << "aeron_ipc[" << self->locator_.identifier() << "] publisher: aeron error "
                << errcode << ": " << message << '\n';
        }

        //Aeron starts the embedded driver's agent threads through this C
        //callback, which cannot throw: a failed binding is reported and counted,
        //and the driver keeps running.
        static void onDriverAgentStart(void *state, const char *roleName) {
            static_cast<AeronIpcPublisher *>(state)->bindDriverAgent(roleName);
        }
        void bindDriverAgent(char const *roleName) {
            if (!driverCpuConfigured_) {
                return;
            }
            std::string who = std::string("aeron_ipc driver ")
                +((roleName != nullptr && *roleName != '\0') ? roleName : "agent");
            try {
                auto lease = provider_(who, driverCpuSpec_);
                std::lock_guard<std::mutex> _(driverLeasesMutex_);
                driverLeases_.push_back(std::move(lease));
            } catch (std::exception const &e) {
                driverBindFailed_.fetch_add(1, std::memory_order_relaxed);
                std::cerr << "aeron_ipc[" << locator_.identifier() << "] driver: cannot bind "
                    << who << ": " << e.what() << '\n';
            } catch (...) {
                driverBindFailed_.fetch_add(1, std::memory_order_relaxed);
                std::cerr << "aeron_ipc[" << locator_.identifier() << "] driver: cannot bind "
                    << who << '\n';
            }
        }

        void startEmbeddedDriver(uint64_t driverTimeoutMs, bool ownerIsDead) {
            if (aeron_driver_context_init(&driverContext_) < 0) {
                throw std::runtime_error(aeronError("driver context init"));
            }
            aeron_driver_context_set_dir(driverContext_, dir_.c_str());
            //Delete-on-start removes the directory without checking for a live driver, so it
            //is only used once the previous owner's process is known to be gone. Otherwise
            //aeron_driver_init deletes a stale directory itself but fails with EBUSY while
            //another driver's heartbeat is fresh.
            aeron_driver_context_set_dir_delete_on_start(driverContext_, ownerIsDead);
            aeron_driver_context_set_dir_delete_on_shutdown(driverContext_, true);
            aeron_driver_context_set_driver_timeout_ms(driverContext_, driverTimeoutMs);
            aeron_driver_context_set_term_buffer_sparse_file(driverContext_, false);

            auto threading = lowerCase(locator_.query("driverThreading", ""));
            if (threading.empty() && !envSet("AERON_THREADING_MODE")) {
                threading = "shared";
            }
            if (threading == "shared") {
                aeron_driver_context_set_threading_mode(driverContext_, AERON_THREADING_MODE_SHARED);
            } else if (threading == "shared_network") {
                aeron_driver_context_set_threading_mode(driverContext_, AERON_THREADING_MODE_SHARED_NETWORK);
            } else if (threading == "dedicated") {
                aeron_driver_context_set_threading_mode(driverContext_, AERON_THREADING_MODE_DEDICATED);
            } else if (!threading.empty()) {
                throw std::invalid_argument("aeron_ipc: bad driverThreading "+threading);
            }

            auto idle = locator_.query("driverIdle", "");
            auto setIdle = [this,&idle](char const *envName, int (*setter)(aeron_driver_context_t *, const char *)) {
                if (!idle.empty()) {
                    setter(driverContext_, idle.c_str());
                } else if (!envSet(envName)) {
                    setter(driverContext_, "spin");
                }
            };
            setIdle("AERON_SHARED_IDLE_STRATEGY", &aeron_driver_context_set_shared_idle_strategy);
            setIdle("AERON_SHAREDNETWORK_IDLE_STRATEGY", &aeron_driver_context_set_sharednetwork_idle_strategy);
            setIdle("AERON_CONDUCTOR_IDLE_STRATEGY", &aeron_driver_context_set_conductor_idle_strategy);
            setIdle("AERON_SENDER_IDLE_STRATEGY", &aeron_driver_context_set_sender_idle_strategy);
            setIdle("AERON_RECEIVER_IDLE_STRATEGY", &aeron_driver_context_set_receiver_idle_strategy);

            //The conductor checks untethered readers only on this timer (Aeron default 1s),
            //so it bounds how quickly a stalled reader stops back pressuring the publisher.
            auto timerUs = queryU64(locator_, "driverTimerIntervalUs", envSet("AERON_TIMER_INTERVAL") ? 0 : 1000);
            if (timerUs != 0) {
                aeron_driver_context_set_timer_interval_ns(driverContext_, timerUs*1000);
            }

            auto cpuStr = locator_.query("driverCpu", "");
            if (cpuStr.empty()) {
                char const *e = std::getenv("TM_AERON_IPC_DRIVER_CPU");
                if (e != nullptr) {
                    cpuStr = e;
                }
            }
            if (!cpuStr.empty()) {
                //validate in the calling thread before any agent thread exists
                try {
                    infra::CpuBindingSpec::parse(cpuStr);
                } catch (std::invalid_argument const &e) {
                    throw std::invalid_argument(
                        "aeron_ipc: invalid 'driverCpu' value '"+cpuStr+"' in locator '"
                        +locator_.toPrintFormat()+"': "+e.what()
                    );
                }
                if (!provider_) {
                    throw std::invalid_argument(
                        "aeron_ipc: locator '"+locator_.toPrintFormat()+"' sets 'driverCpu="+cpuStr
                        +"' but no thread CPU lease provider is installed for this component; call "
                        "aeron_ipc_setThreadCpuLease() or installThreadCpuLeaseProviders<Env>(env) "
                        "before starting the graph"
                    );
                }
                driverCpuSpec_ = cpuStr;
                driverCpuConfigured_ = true;
                //replaces Aeron's own AERON_*_CPU_AFFINITY handling
                aeron_driver_context_set_agent_on_start_function(driverContext_, &onDriverAgentStart, this);
            }

            if (aeron_driver_init(&driver_, driverContext_) < 0) {
                throw std::runtime_error(aeronError("embedded driver init in "+dir_));
            }
            if (aeron_driver_start(driver_, false) < 0) {
                throw std::runtime_error(aeronError("embedded driver start"));
            }
        }
        void closeEmbeddedDriver() {
            if (driver_ != nullptr) {
                aeron_driver_close(driver_);
                driver_ = nullptr;
            }
            //the agent threads are gone now; return their CPUs. Leases may be
            //released from any thread.
            {
                std::lock_guard<std::mutex> _(driverLeasesMutex_);
                driverLeases_.clear();
            }
            if (driverContext_ != nullptr) {
                aeron_driver_context_close(driverContext_);
                driverContext_ = nullptr;
            }
        }
        void cleanup() {
            if (publication_ != nullptr) {
                aeron_exclusive_publication_close(publication_, nullptr, nullptr);
                publication_ = nullptr;
            }
            if (client_ != nullptr) {
                aeron_close(client_);
                client_ = nullptr;
            }
            if (context_ != nullptr) {
                aeron_context_close(context_);
                context_ = nullptr;
            }
            closeEmbeddedDriver();
        }
        void setup() {
            dir_ = aeronDirFor(locator_);
            auto layout = layoutFor(locator_);
            slotSize_ = layout.slotSize;
            auto bp = lowerCase(locator_.query("backPressure", "drop"));
            if (bp == "retry") {
                retryOnBackPressure_ = true;
            } else if (bp != "drop") {
                throw std::invalid_argument("aeron_ipc: bad backPressure "+bp);
            }
            auto driverTimeoutMs = queryU64(locator_, "driverTimeoutMs", 5000);

            auto driverMode = lowerCase(locator_.query("driver", "auto"));
            if (driverMode == "embedded") {
                startEmbeddedDriver(driverTimeoutMs, driverProcessIsDead(dir_));
            } else if (driverMode == "auto") {
                if (!driverIsLive(dir_, driverTimeoutMs)) {
                    try {
                        startEmbeddedDriver(driverTimeoutMs, driverProcessIsDead(dir_));
                    } catch (std::runtime_error const &) {
                        //lost a race against another driver starting in the same directory
                        closeEmbeddedDriver();
                        if (!driverIsLive(dir_, driverTimeoutMs)) {
                            throw;
                        }
                    }
                }
            } else if (driverMode != "external") {
                throw std::invalid_argument("aeron_ipc: bad driver mode "+driverMode);
            }

            if (aeron_context_init(&context_) < 0) {
                throw std::runtime_error(aeronError("context init"));
            }
            aeron_context_set_dir(context_, dir_.c_str());
            aeron_context_set_driver_timeout_ms(context_, driverTimeoutMs);
            aeron_context_set_pre_touch_mapped_memory(context_, true);
            aeron_context_set_error_handler(context_, &AeronIpcPublisher::onError, this);
            if (aeron_init(&client_, context_) < 0) {
                throw std::runtime_error(aeronError("client init in "+dir_));
            }
            if (aeron_start(client_) < 0) {
                throw std::runtime_error(aeronError("client start"));
            }

            std::string uri = "aeron:ipc?term-length="+std::to_string(layout.termLength)
                +"|mtu="+std::to_string(layout.mtu)
                // Aeron's own defaults are 5 s / 5 s / 5 s; we use 2 s so that a
                // genuinely stalled reader is ejected before it accumulates very large
                // back-pressure, while still giving slow callbacks time to finish.
                +"|untethered-window-limit-timeout="+durationMs(queryU64(locator_, "untetheredWindowLimitTimeoutMs", 2000))
                +"|untethered-linger-timeout="+durationMs(queryU64(locator_, "untetheredLingerTimeoutMs", 2000))
                +"|untethered-resting-timeout="+durationMs(queryU64(locator_, "untetheredRestingTimeoutMs", 100));
            aeron_async_add_exclusive_publication_t *async = nullptr;
            if (aeron_async_add_exclusive_publication(&async, client_, uri.c_str(), streamIdFor(locator_)) < 0) {
                throw std::runtime_error(aeronError("add publication "+uri));
            }
            auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while (true) {
                int rc = aeron_async_add_exclusive_publication_poll(&publication_, async);
                if (rc > 0) {
                    break;
                }
                if (rc < 0) {
                    throw std::runtime_error(aeronError("add publication "+uri));
                }
                if (std::chrono::steady_clock::now() > deadline) {
                    throw std::runtime_error("aeron_ipc: timed out adding publication "+uri);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            aeron_publication_constants_t constants;
            if (aeron_exclusive_publication_constants(publication_, &constants) < 0) {
                throw std::runtime_error(aeronError("publication constants"));
            }
            maxClaim_ = constants.max_payload_length;
            if (slotSize_+kTopicLengthBytes > maxClaim_) {
                throw std::runtime_error(
                    "aeron_ipc: driver max payload "+std::to_string(maxClaim_)
                    +" is smaller than slotSize "+std::to_string(slotSize_)
                );
            }
        }
    public:
        AeronIpcPublisher(ConnectionLocator const &locator, ThreadCpuLeaseProvider provider)
            : locator_(locator), dir_(), provider_(std::move(provider)) {
            try {
                setup();
            } catch (...) {
                cleanup();
                throw;
            }
        }
        ~AeronIpcPublisher() {
            cleanup();
        }
        void publish(std::string_view topic, std::string_view content) {
            if (topic.size()+content.size() > slotSize_) {
                bump(oversized_);
                return;
            }
            std::size_t len = kTopicLengthBytes+topic.size()+content.size();
            aeron_buffer_claim_t claim;
            bool countedBackPressure = false;
            while (true) {
                int64_t rc = aeron_exclusive_publication_try_claim(publication_, len, &claim);
                if (rc > 0) {
                    uint8_t *p = claim.data;
                    uint32_t topicLen = static_cast<uint32_t>(topic.size());
                    std::memcpy(p, &topicLen, kTopicLengthBytes);
                    std::memcpy(p+kTopicLengthBytes, topic.data(), topic.size());
                    std::memcpy(p+kTopicLengthBytes+topic.size(), content.data(), content.size());
                    aeron_buffer_claim_commit(&claim);
                    bump(published_);
                    return;
                }
                switch (rc) {
                case AERON_PUBLICATION_NOT_CONNECTED:
                    bump(notConnected_);
                    return;
                case AERON_PUBLICATION_BACK_PRESSURED:
                    if (!countedBackPressure) {
                        bump(backPressured_);
                        countedBackPressure = true;
                    }
                    if (!retryOnBackPressure_) {
                        return;
                    }
                    cpuRelax();
                    break;
                case AERON_PUBLICATION_ADMIN_ACTION:
                    bump(adminAction_);
                    break;
                default:
                    bump(failed_);
                    return;
                }
            }
        }
        AeronIpcPublisherStats stats() const {
            AeronIpcPublisherStats s;
            s.published = published_.load(std::memory_order_relaxed);
            s.notConnected = notConnected_.load(std::memory_order_relaxed);
            s.backPressured = backPressured_.load(std::memory_order_relaxed);
            s.adminAction = adminAction_.load(std::memory_order_relaxed);
            s.oversized = oversized_.load(std::memory_order_relaxed);
            s.failed = failed_.load(std::memory_order_relaxed);
            s.driverBindFailed = driverBindFailed_.load(std::memory_order_relaxed);
            return s;
        }
    };

    class AeronIpcSubscriber {
    private:
        struct Client {
            uint32_t id;
            std::variant<AeronIpcComponent::NoTopicSelection, std::string, std::regex> topic;
            std::function<void(basic::ByteDataWithTopic &&)> f;
            std::optional<WireToUserHook> hook;
        };
        static constexpr std::size_t kFragmentLimit = 64;
        static constexpr std::size_t kDrainFragmentLimit = 1024;
        static constexpr std::size_t kMaxDrainPolls = 4096;
        static constexpr std::size_t kMaxKnownSessions = 64;

        ConnectionLocator locator_;
        std::string dir_;
        int32_t streamId_;
        std::string uri_;
        uint64_t driverTimeoutMs_;
        bool lowCpu_;
        uint64_t spinCount_;
        uint64_t maxSleepUs_;
        bool joinLatest_;

        std::mutex clientsMutex_;
        std::vector<Client> clients_;
        std::atomic<uint64_t> clientsVersion_ {0};

        //below are only touched by the polling thread
        std::vector<Client> localClients_;
        uint64_t localVersion_ = 0;
        aeron_context_t *context_ = nullptr;
        aeron_t *client_ = nullptr;
        aeron_subscription_t *subscription_ = nullptr;
        bool everConnected_ = false;
        bool haveLastSession_ = false;
        int32_t lastSession_ = 0;
        bool draining_ = false;
        std::vector<int32_t> knownSessions_;
        std::vector<int32_t> pendingJoins_;

        //filled by the client conductor thread when an image becomes available
        std::mutex newImagesMutex_;
        std::vector<int32_t> newImages_;
        std::atomic<bool> haveNewImages_ {false};

        std::atomic<bool> running_ {true};
        std::atomic<bool> driverTimedOut_ {false};

        std::atomic<uint64_t> delivered_ {0};
        std::atomic<uint64_t> imagesJoined_ {0};
        std::atomic<uint64_t> skippedOnJoin_ {0};
        std::atomic<uint64_t> clientRebuilds_ {0};
        std::atomic<uint64_t> malformed_ {0};
        std::atomic<uint64_t> overrun_ {0};

        //scratch buffer owned by the polling thread: each fragment is copied here
        //before the term buffer can be cleaned/reused underneath a slow reader
        std::vector<uint8_t> fragScratch_;

        std::thread th_;
        thread_cpu_lease_detail::BindRequest bindRequest_;

        static void onError(void *clientd, int errcode, const char *message) {
            auto *self = static_cast<AeronIpcSubscriber *>(clientd);
            if (errcode == AERON_CLIENT_ERROR_DRIVER_TIMEOUT) {
                self->driverTimedOut_.store(true, std::memory_order_release);
            }
            std::cerr << "aeron_ipc[" << self->locator_.identifier() << "] subscriber: aeron error "
                << errcode << ": " << message << '\n';
        }
        static void onAvailableImage(void *clientd, aeron_subscription_t *, aeron_image_t *image) {
            auto *self = static_cast<AeronIpcSubscriber *>(clientd);
            aeron_image_constants_t c;
            if (aeron_image_constants(image, &c) < 0) {
                return;
            }
            std::lock_guard<std::mutex> _(self->newImagesMutex_);
            self->newImages_.push_back(c.session_id);
            self->haveNewImages_.store(true, std::memory_order_release);
        }
        //Registers images announced by onAvailableImage before their fragments are polled,
        //so that an image which appears before any message is published loses nothing.
        void takeNewImages() {
            std::vector<int32_t> v;
            {
                std::lock_guard<std::mutex> _(newImagesMutex_);
                v.swap(newImages_);
                haveNewImages_.store(false, std::memory_order_relaxed);
            }
            for (auto sid : v) {
                if (contains(knownSessions_, sid)) {
                    continue;
                }
                if (knownSessions_.size() >= kMaxKnownSessions) {
                    knownSessions_.erase(knownSessions_.begin());
                }
                knownSessions_.push_back(sid);
                bump(imagesJoined_);
                if (joinLatest_) {
                    pendingJoins_.push_back(sid);
                }
            }
            if (!pendingJoins_.empty()) {
                drainPendingJoins();
            }
            haveLastSession_ = false;
        }
        static void onFragment(void *clientd, const uint8_t *buffer, size_t length, aeron_header_t *header) {
            static_cast<AeronIpcSubscriber *>(clientd)->handleFragment(buffer, length, header);
        }
        static bool contains(std::vector<int32_t> const &v, int32_t x) {
            for (auto y : v) {
                if (y == x) {
                    return true;
                }
            }
            return false;
        }
        static bool topicMatches(Client const &c, std::string_view topic) {
            switch (c.topic.index()) {
            case 0:
                return true;
            case 1:
                return (std::get<1>(c.topic) == topic);
            default:
                return std::regex_match(topic.begin(), topic.end(), std::get<2>(c.topic));
            }
        }
        void handleFragment(const uint8_t *buffer, size_t length, aeron_header_t *header) {
            if (draining_) {
                bump(skippedOnJoin_);
                return;
            }
            //Snapshot the frame header, copy the payload out of the shared term
            //buffer, then re-read the header.  If the term was cleaned or reused
            //under a slow untethered reader, the second header differs (frame_length
            //is zeroed by clean_buffer, term_id advances on rotation); drop the
            //fragment and count it as overrun.  Delivery always uses the local
            //copy so clients never read the shared term buffer after the handler
            //returns.
            aeron_header_values_t before;
            if (aeron_header_values(header, &before) != 0) {
                bump(malformed_);
                return;
            }
            if (fragScratch_.size() < length) {
                fragScratch_.resize(length);
            }
            std::memcpy(fragScratch_.data(), buffer, length);
            aeron_header_values_t after;
            if (aeron_header_values(header, &after) != 0
                || std::memcmp(&before.frame, &after.frame, sizeof(before.frame)) != 0) {
                bump(overrun_);
                return;
            }
            int32_t sid = before.frame.session_id;
            if (!haveLastSession_ || sid != lastSession_) {
                if (!contains(knownSessions_, sid)) {
                    if (knownSessions_.size() >= kMaxKnownSessions) {
                        knownSessions_.erase(knownSessions_.begin());
                    }
                    knownSessions_.push_back(sid);
                    bump(imagesJoined_);
                    if (joinLatest_) {
                        pendingJoins_.push_back(sid);
                    }
                }
                if (contains(pendingJoins_, sid)) {
                    bump(skippedOnJoin_);
                    return;
                }
                lastSession_ = sid;
                haveLastSession_ = true;
            }
            deliver(fragScratch_.data(), length);
        }
        void deliver(const uint8_t *buffer, size_t length) {
            if (length < kTopicLengthBytes) {
                bump(malformed_);
                return;
            }
            uint32_t topicLen;
            std::memcpy(&topicLen, buffer, kTopicLengthBytes);
            if (topicLen > length-kTopicLengthBytes) {
                bump(malformed_);
                return;
            }
            char const *p = reinterpret_cast<char const *>(buffer+kTopicLengthBytes);
            std::string_view topic(p, topicLen);
            std::string_view content(p+topicLen, length-kTopicLengthBytes-topicLen);
            for (auto const &c : localClients_) {
                if (!topicMatches(c, topic)) {
                    continue;
                }
                if (c.hook) {
                    auto converted = c.hook->hook(basic::ByteDataView {content});
                    if (converted) {
                        c.f(basic::ByteDataWithTopic {std::string(topic), std::move(converted->content)});
                    }
                } else {
                    c.f(basic::ByteDataWithTopic {std::string(topic), std::string(content)});
                }
            }
            bump(delivered_);
        }
        //Discards whatever a newly joined image has already committed, so that delivery
        //starts from the latest message. Runs outside aeron_subscription_poll.
        void drainPendingJoins() {
            for (auto sid : pendingJoins_) {
                aeron_image_t *image = aeron_subscription_image_by_session_id(subscription_, sid);
                if (image == nullptr) {
                    continue;
                }
                //a poll stops at the end of a term, so keep going until nothing is left;
                //the bound only guards against a publisher that outruns the drain
                draining_ = true;
                for (std::size_t ii=0; ii<kMaxDrainPolls; ++ii) {
                    if (aeron_image_poll(image, &onFragment, this, kDrainFragmentLimit) <= 0) {
                        break;
                    }
                }
                draining_ = false;
                aeron_subscription_image_release(subscription_, image);
            }
            pendingJoins_.clear();
        }
        void refreshClients() {
            std::lock_guard<std::mutex> _(clientsMutex_);
            localClients_ = clients_;
            localVersion_ = clientsVersion_.load(std::memory_order_relaxed);
        }
        void teardown() {
            if (subscription_ != nullptr) {
                if (!driverTimedOut_.load(std::memory_order_acquire) && client_ != nullptr && !aeron_is_closed(client_)) {
                    aeron_subscription_close(subscription_, nullptr, nullptr);
                }
                subscription_ = nullptr;
            }
            if (client_ != nullptr) {
                aeron_close(client_);
                client_ = nullptr;
            }
            if (context_ != nullptr) {
                aeron_context_close(context_);
                context_ = nullptr;
            }
        }
        bool connect() {
            if (!driverIsLive(dir_, driverTimeoutMs_)) {
                return false;
            }
            driverTimedOut_.store(false, std::memory_order_release);
            if (aeron_context_init(&context_) < 0) {
                std::cerr << aeronError("subscriber context init") << '\n';
                return false;
            }
            aeron_context_set_dir(context_, dir_.c_str());
            aeron_context_set_driver_timeout_ms(context_, driverTimeoutMs_);
            aeron_context_set_error_handler(context_, &AeronIpcSubscriber::onError, this);
            if (aeron_init(&client_, context_) < 0 || aeron_start(client_) < 0) {
                std::cerr << aeronError("subscriber client start in "+dir_) << '\n';
                teardown();
                return false;
            }
            aeron_async_add_subscription_t *async = nullptr;
            if (aeron_async_add_subscription(&async, client_, uri_.c_str(), streamId_, &AeronIpcSubscriber::onAvailableImage, this, nullptr, nullptr) < 0) {
                std::cerr << aeronError("add subscription "+uri_) << '\n';
                teardown();
                return false;
            }
            auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while (true) {
                int rc = aeron_async_add_subscription_poll(&subscription_, async);
                if (rc > 0) {
                    break;
                }
                if (rc < 0 || driverTimedOut_.load(std::memory_order_acquire) || std::chrono::steady_clock::now() > deadline) {
                    std::cerr << aeronError("add subscription "+uri_) << '\n';
                    subscription_ = nullptr;
                    teardown();
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (everConnected_) {
                bump(clientRebuilds_);
            }
            everConnected_ = true;
            knownSessions_.clear();
            pendingJoins_.clear();
            haveLastSession_ = false;
            takeNewImages();
            return true;
        }
        void pollLoop() {
            uint64_t idleCount = 0;
            uint64_t sleepUs = 1;
            uint32_t checkCounter = 0;
            while (running_.load(std::memory_order_relaxed)) {
                if (clientsVersion_.load(std::memory_order_acquire) != localVersion_) {
                    refreshClients();
                }
                if (haveNewImages_.load(std::memory_order_acquire)) {
                    takeNewImages();
                }
                int n = aeron_subscription_poll(subscription_, &onFragment, this, kFragmentLimit);
                if (!pendingJoins_.empty()) {
                    drainPendingJoins();
                }
                if (n > 0) {
                    idleCount = 0;
                    sleepUs = 1;
                    continue;
                }
                if (n < 0) {
                    std::cerr << aeronError("subscription poll") << '\n';
                    return;
                }
                if ((++checkCounter & 0x3ff) == 0 || lowCpu_) {
                    if (driverTimedOut_.load(std::memory_order_acquire) || aeron_is_closed(client_)) {
                        return;
                    }
                }
                if (++idleCount <= spinCount_) {
                    continue;
                }
                if (lowCpu_) {
                    std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
                    sleepUs = std::min(sleepUs*2, maxSleepUs_);
                } else {
                    cpuRelax();
                }
            }
        }
        void run(thread_cpu_lease_detail::BindHandshake *handshake) {
            //first action: pin this thread, reports the result to the caller
            auto lease = thread_cpu_lease_detail::bindWorkerThread(&bindRequest_, handshake);
            if (!lease) {
                return;
            }
            while (running_.load(std::memory_order_relaxed)) {
                if (!connect()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                pollLoop();
                teardown();
            }
        }
    public:
        AeronIpcSubscriber(ConnectionLocator const &locator, thread_cpu_lease_detail::BindRequest bindRequest)
            : locator_(locator)
            , dir_(aeronDirFor(locator))
            , streamId_(streamIdFor(locator))
            , uri_(queryBool(locator, "tether", false) ? "aeron:ipc" : "aeron:ipc?tether=false")
            , driverTimeoutMs_(queryU64(locator, "driverTimeoutMs", 5000))
            , lowCpu_(false)
            , spinCount_(0)
            , maxSleepUs_(queryU64(locator, "maxSleepUs", 1000))
            , joinLatest_(queryBool(locator, "joinLatest", true))
        {
            auto wait = lowerCase(locator.query("wait", "spin"));
            if (wait == "lowcpu") {
                lowCpu_ = true;
            } else if (wait != "spin") {
                throw std::invalid_argument("aeron_ipc: bad wait strategy "+wait);
            }
            spinCount_ = queryU64(locator, "spinCount", lowCpu_ ? 1000 : 100000);
            if (maxSleepUs_ == 0) {
                maxSleepUs_ = 1;
            }
            bindRequest_ = std::move(bindRequest);
            std::optional<thread_cpu_lease_detail::BindHandshake> handshake;
            if (bindRequest_.needed) {
                handshake.emplace();
            }
            th_ = std::thread(&AeronIpcSubscriber::run, this, handshake ? &*handshake : nullptr);
            if (handshake) {
                try {
                    handshake->future.get();
                } catch (...) {
                    //the poll thread has already returned
                    th_.join();
                    throw;
                }
            }
        }
        ~AeronIpcSubscriber() {
            running_.store(false, std::memory_order_relaxed);
            if (th_.joinable()) {
                th_.join();
            }
            teardown();
        }
        void addClient(uint32_t id
            , std::variant<AeronIpcComponent::NoTopicSelection, std::string, std::regex> const &topic
            , std::function<void(basic::ByteDataWithTopic &&)> f
            , std::optional<WireToUserHook> hook
        ) {
            std::lock_guard<std::mutex> _(clientsMutex_);
            clients_.push_back(Client {id, topic, std::move(f), std::move(hook)});
            clientsVersion_.fetch_add(1, std::memory_order_release);
        }
        void removeClient(uint32_t id) {
            std::lock_guard<std::mutex> _(clientsMutex_);
            for (auto iter = clients_.begin(); iter != clients_.end(); ++iter) {
                if (iter->id == id) {
                    clients_.erase(iter);
                    clientsVersion_.fetch_add(1, std::memory_order_release);
                    return;
                }
            }
        }
        std::thread::native_handle_type threadHandle() {
            return th_.native_handle();
        }
        AeronIpcSubscriberStats stats() const {
            AeronIpcSubscriberStats s;
            s.delivered = delivered_.load(std::memory_order_relaxed);
            s.imagesJoined = imagesJoined_.load(std::memory_order_relaxed);
            s.skippedOnJoin = skippedOnJoin_.load(std::memory_order_relaxed);
            s.clientRebuilds = clientRebuilds_.load(std::memory_order_relaxed);
            s.malformed = malformed_.load(std::memory_order_relaxed);
            s.overrun = overrun_.load(std::memory_order_relaxed);
            return s;
        }
    };

    class AeronIpcComponentImpl {
    private:
        std::mutex mutex_;
        std::unordered_map<ConnectionLocator, std::unique_ptr<AeronIpcSubscriber>> subscribers_;
        std::unordered_map<ConnectionLocator, std::shared_ptr<AeronIpcPublisher>> publishers_;
        std::unordered_map<uint32_t, ConnectionLocator> idToLocator_;
        uint32_t counter_ = 0;

        //the binding provider, settable once before the graph starts
        std::mutex providerMutex_;
        ThreadCpuLeaseProvider provider_;
        bool providerSet_ = false;

        ThreadCpuLeaseProvider getThreadCpuLeaseProvider() {
            std::lock_guard<std::mutex> _(providerMutex_);
            return provider_;
        }
    public:
        AeronIpcComponentImpl() = default;
        ~AeronIpcComponentImpl() {
            std::lock_guard<std::mutex> _(mutex_);
            subscribers_.clear();
            publishers_.clear();
        }
        void setThreadCpuLease(ThreadCpuLeaseProvider provider) {
            std::lock_guard<std::mutex> _(providerMutex_);
            if (providerSet_) {
                throw std::logic_error("aeron_ipc: thread CPU lease provider is already set");
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
                        std::variant<AeronIpcComponent::NoTopicSelection, std::string, std::regex> const &topic,
                        std::function<void(basic::ByteDataWithTopic &&)> client,
                        std::optional<WireToUserHook> wireToUserHook) {
            auto bindRequest = thread_cpu_lease_detail::makeThreadCpuBindRequest(
                locator, "cpu", "aeron_ipc subscriber "+locator.toPrintFormat(), getThreadCpuLeaseProvider()
                , "aeron_ipc", "aeron_ipc_setThreadCpuLease");
            std::lock_guard<std::mutex> _(mutex_);
            auto iter = subscribers_.find(locator);
            if (iter == subscribers_.end()) {
                iter = subscribers_.emplace(locator, std::make_unique<AeronIpcSubscriber>(locator, std::move(bindRequest))).first;
            }
            uint32_t id = ++counter_;
            iter->second->addClient(id, topic, std::move(client), std::move(wireToUserHook));
            idToLocator_.emplace(id, locator);
            return id;
        }
        void removeSubscriptionClient(uint32_t id) {
            std::lock_guard<std::mutex> _(mutex_);
            auto iter = idToLocator_.find(id);
            if (iter == idToLocator_.end()) {
                return;
            }
            auto subIter = subscribers_.find(iter->second);
            if (subIter != subscribers_.end()) {
                subIter->second->removeClient(id);
            }
            idToLocator_.erase(iter);
        }
        std::function<void(basic::ByteDataWithTopic &&)> getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook) {
            std::shared_ptr<AeronIpcPublisher> publisher;
            {
                std::lock_guard<std::mutex> _(mutex_);
                auto iter = publishers_.find(locator);
                if (iter == publishers_.end()) {
                    iter = publishers_.emplace(locator, std::make_shared<AeronIpcPublisher>(locator, getThreadCpuLeaseProvider())).first;
                }
                publisher = iter->second;
            }
            if (userToWireHook) {
                auto hook = userToWireHook->hook;
                return [publisher,hook](basic::ByteDataWithTopic &&data) {
                    auto wire = hook(basic::ByteData {std::move(data.content)});
                    publisher->publish(data.topic, wire.content);
                };
            } else {
                return [publisher](basic::ByteDataWithTopic &&data) {
                    publisher->publish(data.topic, data.content);
                };
            }
        }
        std::unordered_map<ConnectionLocator, std::thread::native_handle_type> threadHandles() {
            std::unordered_map<ConnectionLocator, std::thread::native_handle_type> retVal;
            std::lock_guard<std::mutex> _(mutex_);
            for (auto &item : subscribers_) {
                retVal[item.first] = item.second->threadHandle();
            }
            return retVal;
        }
        std::optional<AeronIpcPublisherStats> publisherStats(ConnectionLocator const &locator) {
            std::lock_guard<std::mutex> _(mutex_);
            auto iter = publishers_.find(locator);
            if (iter == publishers_.end()) {
                return std::nullopt;
            }
            return iter->second->stats();
        }
        std::optional<AeronIpcSubscriberStats> subscriberStats(ConnectionLocator const &locator) {
            std::lock_guard<std::mutex> _(mutex_);
            auto iter = subscribers_.find(locator);
            if (iter == subscribers_.end()) {
                return std::nullopt;
            }
            return iter->second->stats();
        }
    };

    AeronIpcComponent::AeronIpcComponent() : impl_(std::make_unique<AeronIpcComponentImpl>()) {}
    AeronIpcComponent::~AeronIpcComponent() = default;
    uint32_t AeronIpcComponent::aeron_ipc_addSubscriptionClient(ConnectionLocator const &locator,
                        std::variant<NoTopicSelection, std::string, std::regex> const &topic,
                        std::function<void(basic::ByteDataWithTopic &&)> client,
                        std::optional<WireToUserHook> wireToUserHook) {
        return impl_->addSubscriptionClient(locator, topic, std::move(client), std::move(wireToUserHook));
    }
    void AeronIpcComponent::aeron_ipc_removeSubscriptionClient(uint32_t id) {
        impl_->removeSubscriptionClient(id);
    }
    std::function<void(basic::ByteDataWithTopic &&)> AeronIpcComponent::aeron_ipc_getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook) {
        return impl_->getPublisher(locator, std::move(userToWireHook));
    }
    std::unordered_map<ConnectionLocator, std::thread::native_handle_type> AeronIpcComponent::aeron_ipc_threadHandles() {
        return impl_->threadHandles();
    }
    void AeronIpcComponent::aeron_ipc_setThreadCpuLease(ThreadCpuLeaseProvider provider) {
        impl_->setThreadCpuLease(std::move(provider));
    }
    void AeronIpcComponent::aeron_ipc_setThreadCpuLeaseIfUnset(ThreadCpuLeaseProvider provider) {
        impl_->setThreadCpuLeaseIfUnset(std::move(provider));
    }
    std::optional<AeronIpcPublisherStats> AeronIpcComponent::aeron_ipc_publisherStats(ConnectionLocator const &locator) {
        return impl_->publisherStats(locator);
    }
    std::optional<AeronIpcSubscriberStats> AeronIpcComponent::aeron_ipc_subscriberStats(ConnectionLocator const &locator) {
        return impl_->subscriberStats(locator);
    }

} } } } }
