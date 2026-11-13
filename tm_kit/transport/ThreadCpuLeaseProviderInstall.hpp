#ifndef TM_KIT_TRANSPORT_THREAD_CPU_LEASE_PROVIDER_INSTALL_HPP_
#define TM_KIT_TRANSPORT_THREAD_CPU_LEASE_PROVIDER_INSTALL_HPP_

// Definition of installThreadCpuLeaseProviders.  Kept apart from
// ThreadCpuLeaseProvider.hpp so that the component headers can include only the
// small provider interface (they need it for their setter declarations) while
// this header, which needs every component class, is included by the
// ImporterExporter headers and MultiTransportBroadcastListener.

#include <type_traits>

#include <tm_kit/transport/ThreadCpuLeaseProvider.hpp>

#include <tm_kit/transport/multicast/MulticastComponent.hpp>
#include <tm_kit/transport/zeromq/ZeroMQComponent.hpp>
#include <tm_kit/transport/nng/NNGComponent.hpp>
#include <tm_kit/transport/shared_memory_broadcast/SharedMemoryBroadcastComponent.hpp>
#include <tm_kit/transport/singlecast/SinglecastComponent.hpp>
#ifdef __linux__
#include <tm_kit/transport/shm_seqlock/ShmSeqlockComponent.hpp>
#include <tm_kit/transport/shm_cursor/ShmCursorComponent.hpp>
#ifdef TM_KIT_TRANSPORT_AERON_IPC
#include <tm_kit/transport/aeron_ipc/AeronIpcComponent.hpp>
#endif // TM_KIT_TRANSPORT_AERON_IPC
#endif // __linux__

namespace dev { namespace cd606 { namespace tm { namespace transport {

    template <class Env>
    void installThreadCpuLeaseProviders(Env *env)
    {
        if (env == nullptr) {
            return;
        }
        if constexpr (std::is_convertible_v<Env *, multicast::MulticastComponent *>) {
            static_cast<multicast::MulticastComponent *>(env)
                ->multicast_setThreadCpuLeaseIfUnset(threadCpuLeaseProviderFor<Env>());
        }
        if constexpr (std::is_convertible_v<Env *, zeromq::ZeroMQComponent *>) {
            static_cast<zeromq::ZeroMQComponent *>(env)
                ->zeroMQ_setThreadCpuLeaseIfUnset(threadCpuLeaseProviderFor<Env>());
        }
        if constexpr (std::is_convertible_v<Env *, nng::NNGComponent *>) {
            static_cast<nng::NNGComponent *>(env)
                ->nng_setThreadCpuLeaseIfUnset(threadCpuLeaseProviderFor<Env>());
        }
        if constexpr (std::is_convertible_v<Env *, shared_memory_broadcast::SharedMemoryBroadcastComponent *>) {
            static_cast<shared_memory_broadcast::SharedMemoryBroadcastComponent *>(env)
                ->shared_memory_broadcast_setThreadCpuLeaseIfUnset(threadCpuLeaseProviderFor<Env>());
        }
        if constexpr (std::is_convertible_v<Env *, singlecast::SinglecastComponent *>) {
            static_cast<singlecast::SinglecastComponent *>(env)
                ->singlecast_setThreadCpuLeaseIfUnset(threadCpuLeaseProviderFor<Env>());
        }
#ifdef __linux__
        if constexpr (std::is_convertible_v<Env *, shm_seqlock::ShmSeqlockComponent *>) {
            static_cast<shm_seqlock::ShmSeqlockComponent *>(env)
                ->shm_seqlock_setThreadCpuLeaseIfUnset(threadCpuLeaseProviderFor<Env>());
        }
        if constexpr (std::is_convertible_v<Env *, shm_cursor::ShmCursorComponent *>) {
            static_cast<shm_cursor::ShmCursorComponent *>(env)
                ->shm_cursor_setThreadCpuLeaseIfUnset(threadCpuLeaseProviderFor<Env>());
        }
#ifdef TM_KIT_TRANSPORT_AERON_IPC
        if constexpr (std::is_convertible_v<Env *, aeron_ipc::AeronIpcComponent *>) {
            static_cast<aeron_ipc::AeronIpcComponent *>(env)
                ->aeron_ipc_setThreadCpuLeaseIfUnset(threadCpuLeaseProviderFor<Env>());
        }
#endif // TM_KIT_TRANSPORT_AERON_IPC
#endif // __linux__
    }

} } } }

#endif
