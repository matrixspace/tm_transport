#ifndef TM_KIT_TRANSPORT_AERON_IPC_COMPONENT_HPP_
#define TM_KIT_TRANSPORT_AERON_IPC_COMPONENT_HPP_

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

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace aeron_ipc {

    //Single-publisher/multiple-reader IPC over an Aeron media driver (Linux only).
    //The implementation lives in the separate library tm_kit_transport_aeron_ipc,
    //so only programs whose environment contains this component need to link Aeron.
    //
    //Locator: aeron_ipc://::::<name>[prop=value,prop=value,...]
    //  name is mapped to an Aeron stream id (32-bit FNV-1a) unless "stream" is given.
    //Properties (all optional):
    //  dir           aeron directory (default: AERON_DIR env, else Aeron's /dev/shm default)
    //  stream        explicit Aeron stream id
    //  slotSize      max bytes of topic+payload per message (default 1024); larger messages are dropped
    //  slotCount     approximate number of slotSize messages per term (default 8192)
    //  termLength    explicit term length (power of 2, 64KB..1GB), overrides slotCount
    //  wait          spin (default) | lowcpu
    //  spinCount     empty polls before PAUSE (spin) or before sleeping (lowcpu)
    //  maxSleepUs    longest sleep in lowcpu mode (default 1000)
    //  joinLatest    true (default): when a reader first sees a publisher session it discards
    //                everything already committed, so it starts from the latest message
    //                (Aeron joins an IPC image at the slowest active reader's position, which
    //                can be behind the publisher)
    //  tether        false (default): a slow reader cannot hold the publisher back indefinitely
    //  untetheredWindowLimitTimeoutMs / untetheredLingerTimeoutMs / untetheredRestingTimeoutMs
    //                (publisher side, defaults 10 / 10 / 100). While an untethered reader
    //                holds the window the publisher is back pressured (with
    //                backPressure=drop every reader loses those messages), so raising
    //                these trades the other readers' delivery for the slow reader's
    //                fewer losses. A reader ejected mid-fragment never sees torn data:
    //                the fragment is dropped and counted in AeronIpcSubscriberStats::overrun.
    //  backPressure  drop (default): a claim that is back pressured is dropped and counted
    //                retry: spin until the claim succeeds (a stalled tethered reader stalls the publisher)
    //  driver        auto (default) | embedded | external   (publisher side only; auto embeds a driver
    //                thread unless a live driver already owns the directory)
    //  driverTimeoutMs  driver liveness timeout used by clients and the embedded driver (default 5000)
    //  driverThreading  shared (default) | shared_network | dedicated   (embedded driver only)
    //  driverIdle    spin (default) | noop | yield | backoff | sleeping (embedded driver only)
    //  driverTimerIntervalUs  embedded driver timer interval (default 1000, Aeron's own default is 1s);
    //                untethered readers are only checked on this timer
    //  driverCpu     CPU binding for the embedded driver's agent threads, in
    //                CpuBindingSpec syntax (e.g. 3, 2:3, 2-3, auto, numa:0).
    //                (env TM_AERON_IPC_DRIVER_CPU is used when absent; otherwise the
    //                standard AERON_*_CPU_AFFINITY variables apply.) Needs a provider
    //                installed through aeron_ipc_setThreadCpuLease (or
    //                installThreadCpuLeaseProviders<Env>); a failed binding is
    //                counted in AeronIpcPublisherStats::driverBindFailed and the
    //                driver keeps running. In dedicated threading mode each of the
    //                3 agent threads takes its own lease, so "auto" occupies 3
    //                CPUs; an explicit list is the same one for every agent, and a
    //                strict allocator (hwloc) refuses the second reservation
    //                (driverBindFailed counts it), so prefer "auto" there.
    //  cpu           CPU binding for this locator's subscription poll thread, same
    //                syntax. There is one poll thread per complete locator, so this
    //                is never shared with another locator.
    //  AERON_THREADING_MODE / AERON_SHARED_IDLE_STRATEGY are honoured when the props are absent.
    //A reader keeps retrying until a driver is live, and rebuilds its client when the driver
    //goes away (publisher restart with an embedded driver).
    //The publisher function returned for a locator must be called from one thread at a time.
    class AeronIpcComponentImpl;

    struct AeronIpcPublisherStats {
        uint64_t published = 0;
        uint64_t notConnected = 0;    //no reader attached, message dropped
        uint64_t backPressured = 0;   //claim refused because of a slow (not yet ejected) reader
        uint64_t adminAction = 0;     //term rotation, retried immediately
        uint64_t oversized = 0;       //larger than slotSize, dropped
        uint64_t failed = 0;          //publication closed or other error
        uint64_t driverBindFailed = 0; //an embedded driver agent thread could not bind
    };
    struct AeronIpcSubscriberStats {
        uint64_t delivered = 0;       //fragments decoded and handed to clients
        uint64_t imagesJoined = 0;    //publisher sessions (images) seen
        uint64_t skippedOnJoin = 0;   //backlog fragments discarded by joinLatest
        uint64_t clientRebuilds = 0;  //aeron client recreated after the driver went away
        uint64_t malformed = 0;
        uint64_t overrun = 0;         //fragments dropped because term was reused while reading
    };

    class AeronIpcComponent {
    private:
        std::unique_ptr<AeronIpcComponentImpl> impl_;
    public:
        AeronIpcComponent();
        ~AeronIpcComponent();
        struct NoTopicSelection {};
        uint32_t aeron_ipc_addSubscriptionClient(ConnectionLocator const &locator,
                        std::variant<NoTopicSelection, std::string, std::regex> const &topic,
                        std::function<void(basic::ByteDataWithTopic &&)> client,
                        std::optional<WireToUserHook> wireToUserHook = std::nullopt);
        void aeron_ipc_removeSubscriptionClient(uint32_t id);
        std::function<void(basic::ByteDataWithTopic &&)> aeron_ipc_getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook = std::nullopt);
        std::unordered_map<ConnectionLocator, std::thread::native_handle_type> aeron_ipc_threadHandles();
        //Counters are written with relaxed atomics by the publishing / polling thread.
        std::optional<AeronIpcPublisherStats> aeron_ipc_publisherStats(ConnectionLocator const &locator);
        std::optional<AeronIpcSubscriberStats> aeron_ipc_subscriberStats(ConnectionLocator const &locator);
        //Installs the CPU binding provider used by the "cpu"/"driverCpu" locator
        //properties. It is invoked on the subscription poll thread and on each
        //embedded driver agent thread. It can be set only once; a second call
        //throws std::logic_error.
        void aeron_ipc_setThreadCpuLease(ThreadCpuLeaseProvider provider);
    private:
        template <class Env>
        friend void ::dev::cd606::tm::transport::installThreadCpuLeaseProviders(Env *);
        //Framework entry point: installs the default provider unless one is set.
        void aeron_ipc_setThreadCpuLeaseIfUnset(ThreadCpuLeaseProvider provider);
    };

} } } } }

#endif
