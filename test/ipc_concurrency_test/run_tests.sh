#!/usr/bin/env bash
# Concurrency tests for the IPC transports: multi-thread / multi-process read and write.
#
#   run_tests.sh <path-to-ipc_concurrency_test> [transports...] [-- log dir via LOGDIR env]
#   transports: shm_seqlock shm_cursor aeron_ipc shared_memory_broadcast (default: all)
#
# Two kinds of checks:
#   PASS/FAIL  a supported usage; any torn / mixed / reordered / duplicated message fails
#   RISK/SAFE  an unsupported usage (e.g. unlocked concurrent publish); it is only
#              reported, RISK means the problem was actually observed in this run
# Lost messages (gaps) are allowed everywhere: all these transports are lossy.
set -u

BIN=${1:?usage: run_tests.sh <path-to-ipc_concurrency_test> [transports...]}
shift
TRANSPORTS=${*:-shm_seqlock shm_cursor aeron_ipc shared_memory_broadcast}
LOGDIR=${LOGDIR:-$(mktemp -d /tmp/ipc_concurrency_test.XXXXXX)}
mkdir -p "$LOGDIR"
TAG="cc$$"
PASS=0; FAIL=0; RISK=0; SAFE=0
PIDS=()

[ -x "$BIN" ] || { echo "missing $BIN"; exit 2; }
echo "logs in $LOGDIR"

cleanup() {
    for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf /dev/shm/${TAG}_* /dev/shm/tm_shm_cursor.${TAG}_* /dev/shm/aeron-${TAG}-* 2>/dev/null
}
trap cleanup EXIT

# chan <transport> [extra props] -> a channel on a fresh segment
# (called as $(chan ...), i.e. in a subshell, so the counter lives in a file)
NFILE=$LOGDIR/.segment_counter
echo 0 > "$NFILE"
chan() {
    local n=$(( $(cat "$NFILE") + 1 ))
    echo "$n" > "$NFILE"
    local seg=${TAG}_$n extra=${2:+,$2}
    case $1 in
    shm_seqlock) echo "shm_seqlock://::::$seg[slotSize=256,slotCount=1024$extra]" ;;
    shm_cursor)  echo "shm_cursor://::::$seg[slotSize=320,slotCount=1024$extra]" ;;
    aeron_ipc)   echo "aeron_ipc://::::$seg[dir=/dev/shm/aeron-$TAG-$n,slotSize=256$extra]" ;;
    shared_memory_broadcast) echo "shared_memory_broadcast://::::$seg[size=268435456$extra]" ;;
    esac
}
# the writer-serialization property of a transport ("" if it has none)
lockprop() {
    case $1 in
    shm_seqlock) echo "writerLock=true" ;;
    shm_cursor)  echo "publisherLock=true" ;;
    *) echo "" ;;
    esac
}
# field <log> <key> -> value from the RESULT line
field() {
    grep '^RESULT ' "$1" | tail -1 | tr ' ' '\n' | grep "^$2=" | head -1 | cut -d= -f2
}
summary() {
    if ! grep -q '^RESULT ' "$1" 2>/dev/null; then
        # no RESULT line: the process crashed or hung (EXIT line written by run/bg)
        local e; e=$(grep '^EXIT ' "$1" 2>/dev/null | tail -1 | cut -d' ' -f2)
        case $e in
        139) echo "CRASH(SIGSEGV) " ;;
        124) echo "HANG(timeout) " ;;
        "")  echo "no-RESULT " ;;
        *)   echo "no-RESULT(exit=$e) " ;;
        esac
        return
    fi
    grep '^RESULT ' "$1" | tail -1 | tr ' ' '\n' \
        | grep -E '^(status|written|received|corrupt|topicBad|ooo|dup|gaps|error|tp[A-Za-z]*)=' | tr '\n' ' '
}
# expect_clean <name> <log...>: supported usage, every log must be status=ok clean=1
expect_clean() {
    local name=$1; shift
    local ok=1 detail=""
    for l in "$@"; do
        [ "$(field "$l" status)" = ok ] && [ "$(field "$l" clean)" = 1 ] || ok=0
        detail="$detail[$(basename "$l" .log): $(summary "$l")] "
    done
    if [ $ok = 1 ]; then echo "PASS $name"; PASS=$((PASS+1))
    else echo "FAIL $name  $detail"; FAIL=$((FAIL+1)); fi
}
# expect_no_crash <name> <log...>: supported usage where ooo/dup are transport-inherent;
#   requires status=ok and no crash/corrupt, but permits ooo and dup
expect_no_crash() {
    local name=$1; shift
    local ok=1 detail=""
    for l in "$@"; do
        local s c
        s=$(field "$l" status)
        c=$(field "$l" corrupt)
        [ "$s" = ok ] && [ "${c:-0}" = 0 ] \
            && [ "$(field "$l" topicBad)" = 0 ] || ok=0
        # crash is signalled by absence of a RESULT line
        grep -q '^RESULT ' "$l" 2>/dev/null || ok=0
        detail="$detail[$(basename "$l" .log): $(summary "$l")] "
    done
    if [ $ok = 1 ]; then echo "PASS $name"; PASS=$((PASS+1))
    else echo "FAIL $name  $detail"; FAIL=$((FAIL+1)); fi
}
probe() {
    local name=$1; shift
    local bad=0 detail=""
    for l in "$@"; do
        [ "$(field "$l" status)" = ok ] && [ "$(field "$l" clean)" = 1 ] || bad=1
        detail="$detail[$(basename "$l" .log): $(summary "$l")] "
    done
    if [ $bad = 1 ]; then echo "RISK $name  $detail"; RISK=$((RISK+1))
    else echo "SAFE $name (not observed in this run)"; SAFE=$((SAFE+1)); fi
}
# run <log> <args...>: foreground run with a hard timeout
run() {
    local log=$1; shift
    timeout 180 "$BIN" "$@" > "$log" 2>&1
    echo "EXIT $?" >> "$log"
}
# bg <log> <args...>: background run, pid appended to PIDS
bg() {
    local log=$1; shift
    { timeout 180 "$BIN" "$@" > "$log" 2>&1; echo "EXIT $?" >> "$log"; } &
    PIDS+=($!)
    LASTPID=$!
}
waitready() { # waitready <log> <tag>
    for _ in $(seq 1 300); do
        grep -q "^$2" "$1" 2>/dev/null && return 0
        grep -q '^RESULT' "$1" 2>/dev/null && return 1
        sleep 0.05
    done
    return 1
}

# per-transport message count per writer (shared_memory_broadcast allocates per message)
countfor() {
    case $1 in
    shared_memory_broadcast) echo 20000 ;;
    *) echo 200000 ;;
    esac
}

for T in $TRANSPORTS; do
    echo "=== $T"
    C=$(countfor "$T")
    LP=$(lockprop "$T")
    L=$LOGDIR/$T
    mkdir -p "$L"

    # 1. in-process multi-thread read: 1 writer, 3 components x 3 clients, plus client churn
    # aeron_ipc: untethered readers (default) can read a term the publisher is already
    # reusing; use tether=true for the supported multi-reader test.
    if [ "$T" = aeron_ipc ]; then
        run "$L/1_mt_read.log" --mode=inproc --channel="$(chan "$T" "tether=true")" --writers=1 --count="$C" \
            --readerComps=3 --clientsPerComp=3 --churn=true
        expect_clean "$T in-process multi-thread read (+subscribe/unsubscribe churn, tether=true)" "$L/1_mt_read.log"
    elif [ "$T" = shared_memory_broadcast ]; then
        run "$L/1_mt_read.log" --mode=inproc --channel="$(chan "$T")" --writers=1 --count="$C" \
            --readerComps=3 --clientsPerComp=3 --churn=true
        # ooo/dup during subscription churn are inherent to the per-subscriber-queue
        # design; corrupt and crash are not tolerated
        expect_no_crash "$T in-process multi-thread read (+subscribe/unsubscribe churn)" "$L/1_mt_read.log"
    else
        run "$L/1_mt_read.log" --mode=inproc --channel="$(chan "$T")" --writers=1 --count="$C" \
            --readerComps=3 --clientsPerComp=3 --churn=true
        expect_clean "$T in-process multi-thread read (+subscribe/unsubscribe churn)" "$L/1_mt_read.log"
    fi

    # 2. in-process multi-thread write through the transport's own serialization property
    if [ -n "$LP" ]; then
        run "$L/2_mt_write_locked.log" --mode=inproc --channel="$(chan "$T" "$LP")" --writers=4 --count="$C"
        expect_clean "$T in-process 4 writer threads with $LP" "$L/2_mt_write_locked.log"
    fi

    # 3. in-process multi-thread write without serialization (each thread calls getPublisher)
    run "$L/3_mt_write_unlocked.log" --mode=inproc --channel="$(chan "$T")" --writers=4 --count="$C"
    probe "$T in-process 4 writer threads, no lock" "$L/3_mt_write_unlocked.log"

    # 4. same, all threads share ONE publisher function object
    run "$L/4_mt_write_shared.log" --mode=inproc --channel="$(chan "$T")" --writers=4 --count="$C" \
        --sharedPublisher=true
    probe "$T in-process 4 writer threads sharing one publisher, no lock" "$L/4_mt_write_shared.log"

    # 5. lock property only on some locators: the first locator of the segment wins
    if [ -n "$LP" ]; then
        CH=$(chan "$T")
        ALT=${CH%]},$LP]
        run "$L/5_lock_mismatch.log" --mode=inproc --channel="$CH" --altChannel="$ALT" --writers=4 --count="$C"
        probe "$T $LP only on odd writers' locator (first locator decides)" "$L/5_lock_mismatch.log"
    fi

    # 6. TM Kit graph: 4 threaded importers -> one MultiTransportBroadcastPublisherManagingUtils sink
    run "$L/6_graph_direct.log" --mode=graph --channel="$(chan "$T")" --writers=4 --count="$C"
    probe "$T graph: 4 threaded importers -> one exporter (threaded=false)" "$L/6_graph_direct.log"

    # aeron_ipc: untethered readers (the default) can read a term that the publisher is
    # already reusing, even with a single writer thread; that reader-side issue is probed
    # separately in 7b so that 7 only tests the write-side serialization
    if [ "$T" = aeron_ipc ]; then TP="tether=true"; else TP=""; fi
    run "$L/7_graph_threaded.log" --mode=graph --channel="$(chan "$T" "$TP")" --writers=4 --count="$C" \
        --threadedSink=true
    expect_clean "$T graph: 4 threaded importers -> one exporter (threaded=true)${TP:+ with $TP}" "$L/7_graph_threaded.log"
    if [ "$T" = aeron_ipc ]; then
        run "$L/7b_single_writer_untethered.log" --mode=inproc --channel="$(chan "$T")" --writers=1 --count="$C" \
            --readerComps=2 --clientsPerComp=2
        probe "$T 1 writer thread, untethered readers (default tether=false)" "$L/7b_single_writer_untethered.log"
    fi

    # 8. graph with threaded=true plus a heartbeat-like direct publisher on the same channel
    run "$L/8_graph_extra.log" --mode=graph --channel="$(chan "$T")" --writers=2 --count="$C" \
        --threadedSink=true --extraDirectPublisher=true
    probe "$T graph threaded=true + heartbeat-like extra publisher, no lock" "$L/8_graph_extra.log"
    if [ -n "$LP" ]; then
        run "$L/8b_graph_extra_locked.log" --mode=graph --channel="$(chan "$T" "$LP")" --writers=2 --count="$C" \
            --threadedSink=true --extraDirectPublisher=true
        expect_clean "$T graph threaded=true + heartbeat-like extra publisher with $LP" "$L/8b_graph_extra_locked.log"
    fi

    # 9. multi-process read: 1 publisher process, 3 subscriber processes
    CH=$(chan "$T")
    bg "$L/9_pub.log" --mode=pub --channel="$CH" --writers=1 --count="$C" --warmupMs=2000 --holdMs=1500
    PUB=$LASTPID
    waitready "$L/9_pub.log" PUBREADY
    SUBS=()
    for s in 1 2 3; do
        bg "$L/9_sub$s.log" --mode=sub --channel="$CH" --totalWriters=1 --readerComps=2 --clientsPerComp=2 \
            --timeoutSec=60
        SUBS+=($LASTPID)
    done
    wait "$PUB" "${SUBS[@]}" 2>/dev/null
    expect_clean "$T multi-process read: 1 pub process, 3 sub processes" \
        "$L/9_pub.log" "$L/9_sub1.log" "$L/9_sub2.log" "$L/9_sub3.log"

    # 10. multi-process write: two publisher processes on the same segment / stream
    CH=$(chan "$T")
    bg "$L/10_sub.log" --mode=sub --channel="$CH" --totalWriters=2 --readerComps=1 --clientsPerComp=2 \
        --timeoutSec=60
    SUB=$LASTPID
    bg "$L/10_pubA.log" --mode=pub --channel="$CH" --writers=1 --writerBase=0 --totalWriters=2 \
        --count="$C" --warmupMs=2000 --holdMs=3000
    PA=$LASTPID
    waitready "$L/10_pubA.log" PUBREADY
    bg "$L/10_pubB.log" --mode=pub --channel="$CH" --writers=1 --writerBase=1 --totalWriters=2 \
        --count="$C" --warmupMs=1500 --holdMs=3000
    PB=$LASTPID
    wait "$PA" "$PB" "$SUB" 2>/dev/null
    case $T in
    shm_seqlock|shm_cursor)
        # single publisher process per segment is enforced with flock: B must be refused
        if [ "$(field "$L/10_pubA.log" status)" = ok ] && [ "$(field "$L/10_pubB.log" status)" = error ] \
            && [ "$(field "$L/10_sub.log" clean)" = 1 ]; then
            echo "PASS $T multi-process write: second publisher process refused"; PASS=$((PASS+1))
        else
            echo "FAIL $T multi-process write: second publisher process not refused  [pubB: $(summary "$L/10_pubB.log")] [sub: $(summary "$L/10_sub.log")]"
            FAIL=$((FAIL+1))
        fi ;;
    aeron_ipc)
        # each process has its own exclusive publication (own Aeron session / image)
        expect_clean "$T multi-process write: 2 pub processes, one stream" \
            "$L/10_pubA.log" "$L/10_pubB.log" "$L/10_sub.log" ;;
    *)
        probe "$T multi-process write: 2 pub processes, one segment" \
            "$L/10_pubA.log" "$L/10_pubB.log" "$L/10_sub.log" ;;
    esac
    rm -rf /dev/shm/${TAG}_* /dev/shm/tm_shm_cursor.${TAG}_* /dev/shm/aeron-${TAG}-* 2>/dev/null
done

echo "SUMMARY pass=$PASS fail=$FAIL risk=$RISK safe=$SAFE (logs in $LOGDIR)"
[ "$FAIL" = 0 ]
