#ifndef TM_KIT_TRANSPORT_ZEROMQ_ZEROMQ_COMPONENT_HPP_
#define TM_KIT_TRANSPORT_ZEROMQ_ZEROMQ_COMPONENT_HPP_

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

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace zeromq {
    
    class ZeroMQComponentImpl;

    class ZeroMQComponent {
    private:
        std::unique_ptr<ZeroMQComponentImpl> impl_;
    public:
        ZeroMQComponent();
        ~ZeroMQComponent();
        //only host and port (or identifier for inproc/ipc) are needed in the locators.
        //Optional locator properties:
        //  hwm=<n>    high water mark for both publisher and subscriber sockets
        //  sndhwm=<n> publisher send hwm, overrides hwm
        //  rcvhwm=<n> subscriber receive hwm, overrides hwm
        //n is a non-negative integer, 0 means unlimited; unset keeps libzmq's default (1000).
        //Sockets are shared per host/port/identifier, so these only apply when the
        //socket is first created. An invalid value throws std::invalid_argument.
        //
        //Optional locator property:
        //  cpu=<spec>  CPU binding for the receive thread, CpuBindingSpec syntax
        //              (none/auto/numa:<n>/cpus:<list>/<list>; no commas, use ranges).
        //              It needs a provider installed through zeroMQ_setThreadCpuLease
        //              (or installThreadCpuLeaseProviders<Env>). The receive thread is
        //              shared per host/port/identifier, so only the locator that
        //              creates it decides the binding; later different values warn.
        struct NoTopicSelection {};
        uint32_t zeroMQ_addSubscriptionClient(ConnectionLocator const &locator,
                        std::variant<NoTopicSelection, std::string, std::regex> const &topic,
                        std::function<void(basic::ByteDataWithTopic &&)> client,
                        std::optional<WireToUserHook> wireToUserHook = std::nullopt);
        void zeroMQ_removeSubscriptionClient(uint32_t id);
        std::function<void(basic::ByteDataWithTopic &&)> zeroMQ_getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook = std::nullopt);
        std::unordered_map<ConnectionLocator, std::thread::native_handle_type> zeromq_threadHandles();
        //Installs the CPU binding provider used by the "cpu" locator property.
        //The provider is invoked on the receive thread itself as its first action.
        //It can be set only once; a second call throws std::logic_error.
        void zeroMQ_setThreadCpuLease(ThreadCpuLeaseProvider provider);
    private:
        template <class Env>
        friend void ::dev::cd606::tm::transport::installThreadCpuLeaseProviders(Env *);
        //Framework entry point: installs the default provider unless one is set.
        void zeroMQ_setThreadCpuLeaseIfUnset(ThreadCpuLeaseProvider provider);
    };

} } } } }


#endif