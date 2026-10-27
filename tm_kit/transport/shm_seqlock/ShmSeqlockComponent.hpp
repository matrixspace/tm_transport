#ifndef TM_KIT_TRANSPORT_SHM_SEQLOCK_COMPONENT_HPP_
#define TM_KIT_TRANSPORT_SHM_SEQLOCK_COMPONENT_HPP_

#include <memory>
#include <functional>
#include <optional>
#include <regex>
#include <variant>
#include <string>
#include <unordered_map>
#include <thread>
#include <cstdint>

#include <tm_kit/infra/WithTimeData.hpp>
#include <tm_kit/basic/ByteData.hpp>
#include <tm_kit/transport/ConnectionLocator.hpp>
#include <tm_kit/transport/ByteDataHook.hpp>

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace shm_seqlock {

    //Low latency single-publisher multi-reader shared memory broadcast (Linux only).
    //
    //A segment is a ring of fixed size slots in /dev/shm, each slot protected by
    //its own sequence number (seqlock). The publisher never waits for readers; a
    //reader that falls behind by more than slotCount messages is resynced to the
    //latest message and the skipped messages are counted as lost.
    //
    //Locator format: shm_seqlock://::::<segmentName>[prop=value,...]
    //Only the identifier (segmentName) and the properties are used.
    //
    //Publisher properties (they define the segment layout):
    //  slotSize=<n>      bytes available for topic+payload per slot (default 1000)
    //  slotCount=<n>     number of slots, power of two (default 4096)
    //  stride128=true    pad slot stride to a multiple of 128 bytes (default: 64)
    //  layout=inline|split inline: seq shares the first slot line with the slot
    //                    head and the first data bytes (default); split: seq has
    //                    its own 64B line, and the next slot is pre-marked right
    //                    after each commit so that line is written once per
    //                    message. With split a reader can lag at most slotCount-1
    //                    messages before it is lapped. Readers always follow the
    //                    layout of the segment header, whatever their locator says.
    //  testNoPremark=true (tests only) split layout without the pre-mark; this
    //                    is not a valid seqlock and lets torn reads through
    //  wait=spin|futex   futex: publisher wakes futex-waiting readers (default spin)
    //  mlock=true|false  best-effort mlock of the segment (default true)
    //  writerLock=true   serialize publish calls made from several threads of
    //                    this process on this segment, e.g. data plus heartbeat
    //                    (default false: the caller must publish from one thread
    //                    at a time, concurrent unserialized calls corrupt messages)
    //Restarting the publisher with the same layout continues the message indices,
    //so readers resume transparently; with a different layout the old segment is
    //marked superseded and readers reopen the new one.
    //Reader properties (layout is read from the segment header):
    //  read=classic|spec classic: spin until the slot is ready, then copy (default),
    //                    spec: copy the slot speculatively then validate (lower p50
    //                    but worse tails and publisher cost in profiling, see the .cpp)
    //  wait=spin|futex   futex: spin spinCount polls then FUTEX_WAIT (default spin)
    //  spinCount=<n>     polls before sleeping in futex mode (default 20000)
    //  futexTimeoutUs=<n> futex wait timeout in microseconds (default 100000)
    //                    (also bounds the extra delay when the publisher clears
    //                    waiter registrations leaked by readers killed while asleep)
    //  pauseAfter=<n>    in spin mode, issue PAUSE after n empty polls (default 0 = never)
    //  join=latest|next  latest: first delivered message is the latest already
    //                    published one (default); next: first message published
    //                    after joining
    //  mlock=true|false  best-effort mlock of the mapping (default true)
    //
    //There is one reader thread per segment per component; the first subscription
    //on a segment decides the reader properties. Only one thread in one process
    //may publish to a segment (enforced across processes with flock, not within
    //a process).
    struct ShmSeqlockReaderStats {
        uint64_t received;
        uint64_t lost;
        uint64_t lapResyncs;
        uint64_t publisherRestarts;
        uint64_t corrupt;
        uint64_t reopens;
        uint64_t splitLayout; //1 if the current segment uses layout=split
    };
    struct ShmSeqlockPublisherStats {
        uint64_t published;
        uint64_t rejected;
        uint64_t epoch;
        uint64_t waiterResets; //futex mode: leaked reader registrations cleared
    };

    class ShmSeqlockComponentImpl;

    class ShmSeqlockComponent {
    private:
        std::unique_ptr<ShmSeqlockComponentImpl> impl_;
    public:
        ShmSeqlockComponent();
        ~ShmSeqlockComponent();
        struct NoTopicSelection {};
        uint32_t shm_seqlock_addSubscriptionClient(ConnectionLocator const &locator,
                        std::variant<NoTopicSelection, std::string, std::regex> const &topic,
                        std::function<void(basic::ByteDataWithTopic &&)> client,
                        std::optional<WireToUserHook> wireToUserHook = std::nullopt);
        void shm_seqlock_removeSubscriptionClient(uint32_t id);
        std::function<void(basic::ByteDataWithTopic &&)> shm_seqlock_getPublisher(ConnectionLocator const &locator, std::optional<UserToWireHook> userToWireHook = std::nullopt);
        std::unordered_map<ConnectionLocator, std::thread::native_handle_type> shm_seqlock_threadHandles();
        std::unordered_map<ConnectionLocator, ShmSeqlockReaderStats> shm_seqlock_readerStats();
        std::unordered_map<ConnectionLocator, ShmSeqlockPublisherStats> shm_seqlock_publisherStats();
    };

} } } } }

#endif
