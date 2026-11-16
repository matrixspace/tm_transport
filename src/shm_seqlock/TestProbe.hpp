#ifndef TM_KIT_TRANSPORT_SHM_SEQLOCK_TEST_PROBE_HPP_
#define TM_KIT_TRANSPORT_SHM_SEQLOCK_TEST_PROBE_HPP_

// Internal, deliberately not installed. Only the system-test executable defines
// this macro and supplies the implementation. Production preprocessing removes
// every call, including argument evaluation.
#ifdef TM_KIT_SHM_SEQLOCK_TEST_PROBES
#include <cstring>
namespace dev
{
    namespace cd606
    {
        namespace tm
        {
            namespace transport
            {
                namespace shm_seqlock
                {
                    void systemTestProbe(char const *event);
                    inline void systemTestCopy(void *destination, void const *source, std::size_t size, char const *event)
                    {
                        auto *d = static_cast<char *>(destination);
                        auto const *s = static_cast<char const *>(source);
                        std::memcpy(d, s, size / 2);
                        systemTestProbe(event);
                        std::memcpy(d + size / 2, s + size / 2, size - size / 2);
                    }
                } // namespace shm_seqlock
            } // namespace transport
        } // namespace tm
    } // namespace cd606
} // namespace dev
#define TM_SHM_TEST_PROBE(event) systemTestProbe(event)
#define TM_SHM_TEST_COPY(d, s, n, event) systemTestCopy(d, s, n, event)
#else
#define TM_SHM_TEST_PROBE(event) ((void)0)
#define TM_SHM_TEST_COPY(d, s, n, event) std::memcpy(d, s, n)
#endif

#endif
