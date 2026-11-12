#ifndef TM_KIT_TRANSPORT_SRC_THREAD_CPU_LEASE_SUPPORT_HPP_
#define TM_KIT_TRANSPORT_SRC_THREAD_CPU_LEASE_SUPPORT_HPP_

// Internal helpers shared by the IPC transport components that bind their own
// worker threads.  Not installed.

#include <exception>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <tm_kit/transport/ConnectionLocator.hpp>
#include <tm_kit/transport/ThreadCpuLeaseProvider.hpp>

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace thread_cpu_lease_detail {

    // Everything a worker thread needs in order to bind itself.
    struct BindRequest {
        ThreadCpuLeaseProvider provider;
        std::string who;
        std::string spec;   // raw property value, passed to the provider
        infra::CpuBindingSpec parsed;
        bool needed = false;
    };

    inline BindRequest makeThreadCpuBindRequestFromSpec(
        std::string const &rawSpec
        , std::string const &locatorPrint
        , std::string const &property
        , std::string const &who
        , ThreadCpuLeaseProvider const &provider
        , std::string const &transportName
        , std::string const &setterName
    )
    {
        BindRequest req;
        req.who = who;
        req.spec = rawSpec;
        try {
            req.parsed = infra::CpuBindingSpec::parse(rawSpec);
        } catch (std::invalid_argument const &e) {
            throw std::invalid_argument(
                transportName+": invalid '"+property+"' value '"+rawSpec+"' in locator '"
                +locatorPrint+"': "+e.what()
            );
        }
        if (req.parsed.kind == infra::CpuBindKind::None) {
            return req;
        }
        if (!provider) {
            throw std::invalid_argument(
                transportName+": locator '"+locatorPrint+"' sets '"+property+"="+rawSpec
                +"' but no thread CPU lease provider is installed for this component; call "
                +setterName+"() or installThreadCpuLeaseProviders<Env>(env) before starting the graph"
            );
        }
        req.provider = provider;
        req.needed = true;
        return req;
    }

    inline BindRequest makeThreadCpuBindRequest(
        ConnectionLocator const &locator
        , std::string const &property
        , std::string const &who
        , ThreadCpuLeaseProvider const &provider
        , std::string const &transportName
        , std::string const &setterName
    )
    {
        return makeThreadCpuBindRequestFromSpec(
            locator.query(property, "")
            , locator.toPrintFormat()
            , property
            , who
            , provider
            , transportName
            , setterName
        );
    }

    // True when two requests ask for the same effective binding.  Only used to
    // warn about a second locator that maps to an already running shared thread.
    inline bool sameCpuBinding(BindRequest const &a, BindRequest const &b)
    {
        if (a.needed != b.needed) {
            return false;
        }
        if (!a.needed) {
            return true;
        }
        return a.parsed.kind == b.parsed.kind
            && a.parsed.numaNode == b.parsed.numaNode
            && a.parsed.cpus == b.parsed.cpus;
    }

    // Synchronises a worker thread's first-line binding with the caller of
    // addSubscription / getPublisher, so a provider exception surfaces from the
    // latter.  Only created when a binding is actually requested.
    struct BindHandshake {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();
    };

    // Called as the first statement of a worker thread.  Returns an inactive
    // lease when no binding is needed; returns std::nullopt (after signalling the
    // handshake) when binding failed, in which case the thread must return.
    inline std::optional<infra::ThreadCpuLease> bindWorkerThread(
        BindRequest const *request, BindHandshake *handshake)
    {
        if (request == nullptr || !request->needed) {
            if (handshake != nullptr) {
                handshake->promise.set_value();
            }
            return infra::ThreadCpuLease{};
        }
        try {
            auto lease = request->provider(request->who, request->spec);
            if (handshake != nullptr) {
                handshake->promise.set_value();
            }
            return lease;
        } catch (...) {
            if (handshake != nullptr) {
                handshake->promise.set_exception(std::current_exception());
            }
            return std::nullopt;
        }
    }

} } } } }

#endif
