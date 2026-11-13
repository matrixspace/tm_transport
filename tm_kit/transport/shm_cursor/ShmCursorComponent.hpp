#ifndef TM_KIT_TRANSPORT_SHM_CURSOR_COMPONENT_HPP_
#define TM_KIT_TRANSPORT_SHM_CURSOR_COMPONENT_HPP_

#include <memory>
#include <functional>
#include <optional>
#include <regex>
#include <variant>
#include <string>
#include <unordered_map>
#include <thread>

#include <tm_kit/infra/WithTimeData.hpp>
#include <tm_kit/basic/ByteData.hpp>
#include <tm_kit/transport/ConnectionLocator.hpp>
#include <tm_kit/transport/ByteDataHook.hpp>
#include <tm_kit/transport/ThreadCpuLeaseProvider.hpp>

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace shm_cursor {

    // Linux-only lossy single-producer/multi-consumer shared memory transport.
    //
    // Locator format: shm_cursor://::::<segmentName>[prop=value,...]
    //   slotSize   : bytes per slot, multiple of 64, >= 64 (default 1024),
    //                max topic+payload per message is slotSize-16
    //   slotCount  : number of slots, power of two (default 4096)
    //   wait       : "spin" (default) or "futex"; on the publisher side "futex"
    //                enables wakeups of sleeping readers, on the reader side
    //                it selects spin-then-futex waiting
    //   spinCount  : (reader, futex mode) empty polls before sleeping (default 20000)
    //   pauseAfter : (reader, spin mode) empty polls before PAUSE is used (default 100000)
    //   futexTimeoutMs : (reader, futex mode) max sleep per futex wait (default 10)
    //   join       : (reader) "latest" (default: the first message delivered is
    //                the last one committed when the reader attached, no older
    //                backlog) or "next" (only messages committed after attaching)
    //
    //   publisherLock : (publisher) "false" (default) or "true"; set to true when
    //                one process publishes to the segment from more than one
    //                thread (e.g. data plus HeartbeatAndAlertComponent heartbeats)
    //
    //   cpu        : (reader) CPU binding for the segment's reader thread,
    //                CpuBindingSpec syntax (none/auto/numa:<n>/cpus:<list>/<list>;
    //                no commas, use ranges). It needs a provider installed through
    //                shm_cursor_setThreadCpuLease (or
    //                installThreadCpuLeaseProviders<Env>). There is one reader
    //                thread per segment, so only the locator that creates it
    //                decides the binding; later different values warn.
    //
    // Only the identifier is used to key the segment; the first locator seen
    // for a segment in this process determines its properties. slotSize and
    // slotCount only matter on the publisher side (a publisher whose layout
    // differs from the existing segment re-creates it); readers always take
    // the layout from the segment header.
    // A segment must have exactly one publisher process (enforced with an
    // exclusive flock on /dev/shm/tm_shm_cursor.<name>.lock), and unless
    // publisherLock=true the publisher function must not be called
    // concurrently from multiple threads.
    class ShmCursorComponentImpl;

    class ShmCursorComponent {
    private:
        std::unique_ptr<ShmCursorComponentImpl> impl_;

    public:
        ShmCursorComponent();
        ~ShmCursorComponent();
        struct NoTopicSelection {};
        uint32_t shm_cursor_addSubscriptionClient(ConnectionLocator const &locator,
            std::variant<NoTopicSelection, std::string, std::regex> const &topic,
            std::function<void(basic::ByteDataWithTopic &&)> client,
            std::optional<WireToUserHook> wireToUserHook = std::nullopt);
        void shm_cursor_removeSubscriptionClient(uint32_t id);
        std::function<void(basic::ByteDataWithTopic &&)> shm_cursor_getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook = std::nullopt);
        std::unordered_map<ConnectionLocator, std::thread::native_handle_type> shm_cursor_threadHandles();
        // number of messages this process's reader for the segment has lost
        //(lapped or overwritten during copy), 0 if not subscribed
        uint64_t shm_cursor_subscriptionLossCount(ConnectionLocator const &locator);
        // how many times this process's reader for the segment detected that the
        // slot it was copying got overwritten (each such event is also counted
        // in the loss count above)
        uint64_t shm_cursor_subscriptionOverwriteCount(ConnectionLocator const &locator);
        // number of messages this process's publisher for the segment has
        // rejected because topic+payload exceeded the slot capacity
        uint64_t shm_cursor_publisherRejectedCount(ConnectionLocator const &locator);
        //Installs the CPU binding provider used by the "cpu" locator property.
        //The provider is invoked on the reader thread itself as its first action.
        //It can be set only once; a second call throws std::logic_error.
        void shm_cursor_setThreadCpuLease(ThreadCpuLeaseProvider provider);
    private:
        template <class Env>
        friend void ::dev::cd606::tm::transport::installThreadCpuLeaseProviders(Env *);
        //Framework entry point: installs the default provider unless one is set.
        void shm_cursor_setThreadCpuLeaseIfUnset(ThreadCpuLeaseProvider provider);
    };

}}}}}

#endif
