#ifndef TM_KIT_TRANSPORT_SINGLECAST_SINGLECAST_COMPONENT_HPP_
#define TM_KIT_TRANSPORT_SINGLECAST_SINGLECAST_COMPONENT_HPP_

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

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace singlecast {
    
    class SinglecastComponentImpl;

    class SinglecastComponent {
    private:
        std::unique_ptr<SinglecastComponentImpl> impl_;
    public:
        SinglecastComponent();
        ~SinglecastComponent();
        //only host and port are needed in the locators
        //Optional locator properties (CpuBindingSpec syntax: none/auto/numa:<n>/
        //cpus:<list>/<list>; no commas, use ranges):
        //  cpu=<spec>        binding for the per host:port receive thread
        //  senderCpu=<spec>  binding for the component-wide sender thread,
        //                    decided by the first getPublisher
        //Both need a provider installed through singlecast_setThreadCpuLease (or
        //installThreadCpuLeaseProviders<Env>).
        struct NoTopicSelection {};
        uint32_t singlecast_addSubscriptionClient(ConnectionLocator const &locator,
                        std::variant<NoTopicSelection, std::string, std::regex> const &topic,
                        std::function<void(basic::ByteDataWithTopic &&)> client,
                        std::optional<WireToUserHook> wireToUserHook = std::nullopt);
        void singlecast_removeSubscriptionClient(uint32_t id);
        std::function<void(basic::ByteDataWithTopic &&)> singlecast_getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook = std::nullopt);
        std::unordered_map<ConnectionLocator, std::thread::native_handle_type> singlecast_threadHandles();
        //Installs the CPU binding provider used by the "cpu"/"senderCpu" locator
        //properties. The provider is invoked on the worker thread itself as its
        //first action. It can be set only once; a second call throws
        //std::logic_error.
        void singlecast_setThreadCpuLease(ThreadCpuLeaseProvider provider);
    private:
        template <class Env>
        friend void ::dev::cd606::tm::transport::installThreadCpuLeaseProviders(Env *);
        //Framework entry point: installs the default provider unless one is set.
        void singlecast_setThreadCpuLeaseIfUnset(ThreadCpuLeaseProvider provider);
    };

} } } } }


#endif