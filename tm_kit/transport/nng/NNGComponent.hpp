#ifndef TM_KIT_TRANSPORT_NNG_NNG_COMPONENT_HPP_
#define TM_KIT_TRANSPORT_NNG_NNG_COMPONENT_HPP_

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

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace nng {
    
    class NNGComponentImpl;

    class NNGComponent {
    private:
        std::unique_ptr<NNGComponentImpl> impl_;
    public:
        NNGComponent();
        ~NNGComponent();
        //only host and port are needed in the locators
        //Optional locator property:
        //  cpu=<spec>  CPU binding for the receive thread, CpuBindingSpec syntax
        //              (none/auto/numa:<n>/cpus:<list>/<list>; no commas, use ranges).
        //              It needs a provider installed through nng_setThreadCpuLease
        //              (or installThreadCpuLeaseProviders<Env>). The receive thread is
        //              shared per host/port/identifier, so only the locator that
        //              creates it decides the binding; later different values warn.
        struct NoTopicSelection {};
        uint32_t nng_addSubscriptionClient(ConnectionLocator const &locator,
                        std::variant<NoTopicSelection, std::string, std::regex> const &topic,
                        std::function<void(basic::ByteDataWithTopic &&)> client,
                        std::optional<WireToUserHook> wireToUserHook = std::nullopt);
        void nng_removeSubscriptionClient(uint32_t id);
        std::function<void(basic::ByteDataWithTopic &&)> nng_getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook = std::nullopt);
        std::unordered_map<ConnectionLocator, std::thread::native_handle_type> nng_threadHandles();
        //Installs the CPU binding provider used by the "cpu" locator property.
        //The provider is invoked on the receive thread itself as its first action.
        //It can be set only once; a second call throws std::logic_error.
        void nng_setThreadCpuLease(ThreadCpuLeaseProvider provider);
    private:
        template <class Env>
        friend void ::dev::cd606::tm::transport::installThreadCpuLeaseProviders(Env *);
        //Framework entry point: installs the default provider unless one is set.
        void nng_setThreadCpuLeaseIfUnset(ThreadCpuLeaseProvider provider);
    };

} } } } }


#endif