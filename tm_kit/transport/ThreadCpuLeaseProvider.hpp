#ifndef TM_KIT_TRANSPORT_THREAD_CPU_LEASE_PROVIDER_HPP_
#define TM_KIT_TRANSPORT_THREAD_CPU_LEASE_PROVIDER_HPP_

#include <functional>
#include <string>
#include <type_traits>

#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>

namespace dev { namespace cd606 { namespace tm { namespace transport {

    // A thread CPU binding provider.  A transport's own worker thread calls it as
    // its first action, with
    //   who  -- a description of the thread, e.g. "shm_seqlock reader feed"
    //   spec -- the raw locator property value ("auto", "numa:0", "cpus:4-5", ...)
    // It pins the calling thread and returns the lease; the lease must be kept for
    // as long as the thread runs, since its destructor returns the CPUs to the
    // allocator.  The signature matches ThreadCpuLease::bindCurrentThreadBy.
    using ThreadCpuLeaseProvider = std::function<infra::ThreadCpuLease(std::string const &who, std::string const &spec)>;

    // The default provider: routes the spec through the Environment's
    // IThreadCpuAllocator (ThreadCpuLease::bindCurrentThreadBy<Env>).  An Env
    // without a ThreadCpuAllocatorComponent still compiles; any real binding then
    // throws infra::ThreadCpuBindError, exactly like bindCurrentThreadBy.
    template <class Env>
    ThreadCpuLeaseProvider threadCpuLeaseProviderFor()
    {
        return [](std::string const &who, std::string const &spec) {
            return infra::ThreadCpuLease::bindCurrentThreadBy<Env>(spec, who);
        };
    }

    // Installs threadCpuLeaseProviderFor<Env>() into every transport component
    // present in Env.  Only components that have no provider yet are touched, so
    // this can be called repeatedly (e.g. from every ImporterExporter::start).
    // Implemented in ThreadCpuLeaseProviderInstall.hpp.
    template <class Env>
    void installThreadCpuLeaseProviders(Env *env);

} } } }

#endif
