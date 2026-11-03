#ifndef TM_KIT_TRANSPORT_TEST_IPC_CONCURRENCY_TRANSPORT_ADAPTER_HPP_
#define TM_KIT_TRANSPORT_TEST_IPC_CONCURRENCY_TRANSPORT_ADAPTER_HPP_

//Uniform access to the IPC transports under test, selected by a MultiTransport
//channel string (shm_seqlock://, shm_cursor://, shared_memory_broadcast://,
//aeron_ipc:// when built with Aeron).

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>

#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/basic/ByteData.hpp>
#include <tm_kit/basic/TrivialBoostLoggingComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>
#include <tm_kit/transport/MultiTransportBroadcastListener.hpp>

namespace ipc_concurrency {

    using namespace dev::cd606::tm;
    using CT = transport::MultiTransportBroadcastListenerConnectionType;

    using Env = infra::Environment<
        infra::CheckTimeComponent<false>
        , infra::TrivialExitControlComponent
        , basic::TrivialBoostLoggingComponent
        , basic::real_time_clock::ClockComponent
        , transport::shm_seqlock::ShmSeqlockComponent
        , transport::shm_cursor::ShmCursorComponent
        , transport::shared_memory_broadcast::SharedMemoryBroadcastComponent
#ifdef TM_KIT_TRANSPORT_AERON_IPC
        , transport::aeron_ipc::AeronIpcComponent
#endif
    >;
    using Publisher = std::function<void(basic::ByteDataWithTopic &&)>;

    struct Channel {
        CT type;
        transport::ConnectionLocator locator;
        std::string text;
        std::string name;
    };

    inline Channel parseChannel(std::string const &s) {
        auto p = transport::parseMultiTransportBroadcastChannel(s);
        if (!p) {
            throw std::runtime_error("cannot parse channel '"+s+"'");
        }
        switch (std::get<0>(*p)) {
        case CT::ShmSeqlock:
            return {CT::ShmSeqlock, std::get<1>(*p), s, "shm_seqlock"};
        case CT::ShmCursor:
            return {CT::ShmCursor, std::get<1>(*p), s, "shm_cursor"};
        case CT::SharedMemoryBroadcast:
            return {CT::SharedMemoryBroadcast, std::get<1>(*p), s, "shared_memory_broadcast"};
#ifdef TM_KIT_TRANSPORT_AERON_IPC
        case CT::AeronIpc:
            return {CT::AeronIpc, std::get<1>(*p), s, "aeron_ipc"};
#endif
        default:
            throw std::runtime_error("channel '"+s+"' is not an IPC transport supported by this build");
        }
    }

    inline Publisher getPublisher(Env *env, Channel const &c) {
        switch (c.type) {
        case CT::ShmSeqlock:
            return env->shm_seqlock_getPublisher(c.locator);
        case CT::ShmCursor:
            return env->shm_cursor_getPublisher(c.locator);
        case CT::SharedMemoryBroadcast:
            return env->shared_memory_broadcast_getPublisher(c.locator);
#ifdef TM_KIT_TRANSPORT_AERON_IPC
        case CT::AeronIpc:
            return env->aeron_ipc_getPublisher(c.locator);
#endif
        default:
            throw std::runtime_error("unsupported channel "+c.text);
        }
    }

    inline uint32_t subscribe(Env *env, Channel const &c, Publisher cb) {
        switch (c.type) {
        case CT::ShmSeqlock:
            return env->shm_seqlock_addSubscriptionClient(c.locator
                , transport::shm_seqlock::ShmSeqlockComponent::NoTopicSelection {}, std::move(cb));
        case CT::ShmCursor:
            return env->shm_cursor_addSubscriptionClient(c.locator
                , transport::shm_cursor::ShmCursorComponent::NoTopicSelection {}, std::move(cb));
        case CT::SharedMemoryBroadcast:
            return env->shared_memory_broadcast_addSubscriptionClient(c.locator
                , transport::shared_memory_broadcast::SharedMemoryBroadcastComponent::NoTopicSelection {}, std::move(cb));
#ifdef TM_KIT_TRANSPORT_AERON_IPC
        case CT::AeronIpc:
            return env->aeron_ipc_addSubscriptionClient(c.locator
                , transport::aeron_ipc::AeronIpcComponent::NoTopicSelection {}, std::move(cb));
#endif
        default:
            throw std::runtime_error("unsupported channel "+c.text);
        }
    }

    inline void unsubscribe(Env *env, Channel const &c, uint32_t id) {
        switch (c.type) {
        case CT::ShmSeqlock:
            env->shm_seqlock_removeSubscriptionClient(id);
            break;
        case CT::ShmCursor:
            env->shm_cursor_removeSubscriptionClient(id);
            break;
        case CT::SharedMemoryBroadcast:
            env->shared_memory_broadcast_removeSubscriptionClient(id);
            break;
#ifdef TM_KIT_TRANSPORT_AERON_IPC
        case CT::AeronIpc:
            env->aeron_ipc_removeSubscriptionClient(id);
            break;
#endif
        default:
            break;
        }
    }

    //transport-side counters, as "key=value ..." (empty when none are exposed)
    inline std::string publisherStats(Env *env, Channel const &c) {
        std::ostringstream oss;
        if (c.type == CT::ShmSeqlock) {
            uint64_t published = 0, rejected = 0;
            for (auto const &item : env->shm_seqlock_publisherStats()) {
                published += item.second.published;
                rejected += item.second.rejected;
            }
            oss << "tpPublished=" << published << " tpRejected=" << rejected;
        } else if (c.type == CT::ShmCursor) {
            oss << "tpRejected=" << env->shm_cursor_publisherRejectedCount(c.locator);
#ifdef TM_KIT_TRANSPORT_AERON_IPC
        } else if (c.type == CT::AeronIpc) {
            if (auto s = env->aeron_ipc_publisherStats(c.locator)) {
                oss << "tpPublished=" << s->published << " tpNotConnected=" << s->notConnected
                    << " tpBackPressured=" << s->backPressured << " tpFailed=" << s->failed;
            }
#endif
        }
        return oss.str();
    }
    inline std::string subscriberStats(Env *env, Channel const &c) {
        std::ostringstream oss;
        if (c.type == CT::ShmSeqlock) {
            uint64_t lost = 0, corrupt = 0;
            for (auto const &item : env->shm_seqlock_readerStats()) {
                lost += item.second.lost;
                corrupt += item.second.corrupt;
            }
            oss << "tsLost=" << lost << " tsCorrupt=" << corrupt;
        } else if (c.type == CT::ShmCursor) {
            oss << "tsLost=" << env->shm_cursor_subscriptionLossCount(c.locator)
                << " tsOverwrite=" << env->shm_cursor_subscriptionOverwriteCount(c.locator);
#ifdef TM_KIT_TRANSPORT_AERON_IPC
        } else if (c.type == CT::AeronIpc) {
            if (auto s = env->aeron_ipc_subscriberStats(c.locator)) {
                oss << "tsDelivered=" << s->delivered << " tsMalformed=" << s->malformed;
            }
#endif
        }
        return oss.str();
    }

}

#endif
