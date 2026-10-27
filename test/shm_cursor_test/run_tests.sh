#!/usr/bin/env bash
# Multi-process functional tests for the shm_cursor transport.
# Usage: run_tests.sh <path-to-shm_cursor_test> [work-dir]
# Correctness only; do not read latency numbers from here.
set -u

B=${1:?usage: run_tests.sh <path-to-shm_cursor_test> [work-dir]}
W=${2:-$(mktemp -d)}
mkdir -p "$W"
TAG="t$$"
PIDS=()
PASS=0
FAIL=0

cleanup() {
    for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -f /dev/shm/tm_shm_cursor.${TAG}_*
}
trap cleanup EXIT

start() { # start <name> <args...>   (runs in background, output in $W/<name>.out/.err)
    local n=$1; shift
    "$B" "$@" > "$W/$n.out" 2> "$W/$n.err" &
    PIDS+=($!)
    eval "PID_$n=$!"
}
val() { # val <name> <key>
    grep '^RESULT ' "$W/$1.out" | tr ' ' '\n' | grep "^$2=" | head -1 | cut -d= -f2
}
waitfor() { # waitfor <name>...
    for n in "$@"; do
        local p; eval "p=\$PID_$n"
        wait "$p" 2>/dev/null
    done
}
check() { # check <test> <description> <condition-expr>
    if eval "$3"; then
        echo "PASS [$1] $2"; PASS=$((PASS+1))
    else
        echo "FAIL [$1] $2   ($3)"; FAIL=$((FAIL+1))
    fi
}
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat"; }
result() { grep '^RESULT ' "$W/$1.out" | cut -c8-; }

ch() { echo "shm_cursor://::::${TAG}_$1$2"; }

# ---------------------------------------------------------------- 1: 3 readers keep up, in order
C=$(ch t1 "")
for i in 1 2 3; do start t1_r$i --mode=sub --channel="$C" --label=r$i --count=100000 --max-wait-ms=20000; done
sleep 0.3
start t1_pub --mode=pub --channel="$C" --count=100000 --rate=200000 --size=256
waitfor t1_pub t1_r1 t1_r2 t1_r3
for i in 1 2 3; do
    check 1 "reader r$i got 100000 in order, intact" \
        "[ '$(val t1_r$i received)' = 100000 ] && [ '$(val t1_r$i gaps)' = 0 ] && [ '$(val t1_r$i outOfOrder)' = 0 ] && [ '$(val t1_r$i corrupt)' = 0 ] && [ '$(val t1_r$i firstSeq)' = 1 ] && [ '$(val t1_r$i lastSeq)' = 100000 ] && [ '$(val t1_r$i transportLost)' = 0 ]"
done
echo "      $(result t1_pub)"

# ---------------------------------------------------------------- 2: topic filtering
C=$(ch t2 "")
T=t.a,t.b,u.c
start t2_exact --mode=sub --channel="$C" --label=exact --topic=t.a --topics=$T --expect-topics=t.a --count=10000
start t2_regex --mode=sub --channel="$C" --label=regex '--topic=r/t\..*/' --topics=$T --expect-topics=t.a,t.b --count=20000
start t2_rmq --mode=sub --channel="$C" --label=rabbitstyle '--topic=u.*' --topics=$T --expect-topics=u.c --count=10000
start t2_all --mode=sub --channel="$C" --label=all --topics=$T --expect-topics=t.a,t.b,u.c --count=30000
sleep 0.3
start t2_pub --mode=pub --channel="$C" --count=30000 --rate=100000 --topics=$T
waitfor t2_pub t2_exact t2_regex t2_rmq t2_all
check 2 "exact string 't.a' -> 10000, only t.a" "[ '$(val t2_exact received)' = 10000 ] && [ '$(val t2_exact wrongTopic)' = 0 ] && [ '$(val t2_exact corrupt)' = 0 ]"
check 2 "regex r/t\\..*/ -> 20000, only t.a/t.b" "[ '$(val t2_regex received)' = 20000 ] && [ '$(val t2_regex wrongTopic)' = 0 ] && [ '$(val t2_regex corrupt)' = 0 ]"
check 2 "rabbitmq-style 'u.*' -> 10000, only u.c" "[ '$(val t2_rmq received)' = 10000 ] && [ '$(val t2_rmq wrongTopic)' = 0 ]"
check 2 "NoTopicSelection -> all 30000 in order" "[ '$(val t2_all received)' = 30000 ] && [ '$(val t2_all gaps)' = 0 ] && [ '$(val t2_all wrongTopic)' = 0 ]"

# ---------------------------------------------------------------- 3: slow reader
C=$(ch t3 "")
start t3_fast1 --mode=sub --channel="$C" --label=fast1 --count=100000
start t3_fast2 --mode=sub --channel="$C" --label=fast2 --count=100000
start t3_slow --mode=sub --channel="$C" --label=slow --sleep-us=100 --idle-ms=1500
sleep 0.3
start t3_pub --mode=pub --channel="$C" --count=100000 --rate=200000
waitfor t3_pub t3_fast1 t3_fast2 t3_slow
for n in fast1 fast2; do
    check 3 "$n lost nothing next to slow reader" "[ '$(val t3_$n received)' = 100000 ] && [ '$(val t3_$n gaps)' = 0 ] && [ '$(val t3_$n corrupt)' = 0 ]"
done
check 3 "slow reader lost messages (counted by transport), no corruption" \
    "[ '$(val t3_slow transportLost)' -gt 0 ] && [ '$(val t3_slow gaps)' -gt 0 ] && [ '$(val t3_slow corrupt)' = 0 ] && [ '$(val t3_slow outOfOrder)' = 0 ]"
check 3 "rate-limited publisher kept its schedule (elapsed < 600ms for 100k @200k/s)" "awk 'BEGIN{exit !($(val t3_pub elapsedMs) < 600)}'"
echo "      slow: $(result t3_slow)"
# unthrottled publisher: alone vs. with a slow reader attached
C=$(ch t3b "")
start t3b_pub0 --mode=pub --channel="$C" --count=2000000 --rate=0 --linger-ms=0
waitfor t3b_pub0
C=$(ch t3c "")
start t3c_slow --mode=sub --channel="$C" --label=slow --sleep-us=100 --idle-ms=1000
sleep 0.3
start t3c_pub1 --mode=pub --channel="$C" --count=2000000 --rate=0 --linger-ms=0
waitfor t3c_pub1 t3c_slow
R0=$(val t3b_pub0 msgPerSec); R1=$(val t3c_pub1 msgPerSec)
check 3 "unthrottled publisher rate with slow reader >= 70% of rate alone ($R1 vs $R0 msg/s)" "awk 'BEGIN{exit !($R1 >= 0.7*$R0)}'"

# ---------------------------------------------------------------- 4: reader SIGKILL + restart starts at latest
C=$(ch t4 "")
start t4_pub --mode=pub --channel="$C" --count=0 --rate=1000
start t4_r1 --mode=sub --channel="$C" --label=before_kill --idle-ms=100000 --max-wait-ms=100000
sleep 1.5
kill -9 "$PID_t4_r1"
sleep 1.0
start t4_r2 --mode=sub --channel="$C" --label=restarted --count=500
waitfor t4_r2
kill -9 "$PID_t4_pub"
check 4 "restarted reader's first message is the latest (seq >= 2000, age < 50ms), no backlog" \
    "[ '$(val t4_r2 firstSeq)' -ge 2000 ] && [ '$(val t4_r2 firstAgeUs)' -lt 50000 ] && [ '$(val t4_r2 received)' -ge 500 ] && [ '$(val t4_r2 gaps)' = 0 ] && [ '$(val t4_r2 corrupt)' = 0 ]"
echo "      $(result t4_r2)"
C=$(ch t4b "[join=next]")
start t4b_pub --mode=pub --channel="$C" --count=0 --rate=1000
sleep 1.0
start t4b_r --mode=sub --channel="$C" --label=join_next --count=200
waitfor t4b_r
kill -9 "$PID_t4b_pub"
# publisher starts after a 300ms delay, so ~700 messages are committed before the reader attaches
check 4 "join=next: first message is newer than anything committed before attach (age < 50ms)" "[ '$(val t4b_r firstAgeUs)' -lt 50000 ] && [ '$(val t4b_r firstSeq)' -ge 600 ]"

# ---------------------------------------------------------------- 5: publisher SIGKILL + restart
C=$(ch t5 "")
start t5_pub1 --mode=pub --channel="$C" --count=0 --rate=1000
start t5_r1 --mode=sub --channel="$C" --label=spin --idle-ms=2500 --max-wait-ms=20000
start t5_r2 --mode=sub --channel="$C[wait=futex]" --label=futex --idle-ms=2500 --max-wait-ms=20000
sleep 1.5
kill -9 "$PID_t5_pub1"
sleep 1.0
start t5_pub2 --mode=pub --channel="$C" --count=2000 --rate=1000
waitfor t5_pub2 t5_r1 t5_r2
check 5 "restarted publisher could take the flock and publish" "[ '$(val t5_pub2 sent)' = 2000 ]"
for n in r1 r2; do
    check 5 "reader $n resumed after publisher restart (all 2000 from new publisher)" \
        "[ '$(val t5_$n pubSwitches)' = 1 ] && [ '$(val t5_$n lastPubReceived)' = 2000 ] && [ '$(val t5_$n corrupt)' = 0 ] && [ '$(val t5_$n outOfOrder)' = 0 ]"
done
check 5 "reader saw publisher death and restart" "grep -q 'is not running' $W/t5_r1.err && grep -q 'publisher restarted' $W/t5_r1.err"
# restart with a different layout: segment is re-created, reader re-attaches
C=$(ch t5b "")
start t5b_pub1 --mode=pub --channel="$C" --count=0 --rate=1000
start t5b_r --mode=sub --channel="$C" --label=relayout --idle-ms=2500 --max-wait-ms=20000
sleep 1.5
kill -9 "$PID_t5b_pub1"
sleep 0.5
start t5b_pub2 --mode=pub --channel="$C[slotCount=8192,slotSize=512]" --count=2000 --rate=1000
waitfor t5b_pub2 t5b_r
check 5 "publisher restart with new layout: reader re-attached and resumed" \
    "[ '$(val t5b_r pubSwitches)' = 1 ] && [ '$(val t5b_r lastPubReceived)' -ge 1900 ] && [ '$(val t5b_r corrupt)' = 0 ] && grep -q 're-attaching' $W/t5b_r.err"

# ---------------------------------------------------------------- 6: oversized messages
C=$(ch t6 "[slotSize=1024]")
start t6_r --mode=sub --channel="$C" --label=r --count=9950
sleep 0.3
start t6_pub --mode=pub --channel="$C" --count=10000 --rate=100000 --oversize-every=100
waitfor t6_pub t6_r
check 6 "publisher rejected exactly the 50 oversized messages" "[ '$(val t6_pub rejected)' = 50 ] && [ '$(val t6_pub expectedRejected)' = 50 ]"
# the last message (seq 10000) is itself rejected, so only 49 gaps are visible to the reader
check 6 "reader got 9950 intact (incl. 50 exact-capacity), gaps only at the rejected ones" \
    "[ '$(val t6_r received)' = 9950 ] && [ '$(val t6_r corrupt)' = 0 ] && [ '$(val t6_r gaps)' = 49 ] && [ '$(val t6_r transportLost)' = 0 ]"
check 6 "rejection was logged" "grep -q 'dropped oversized message' $W/t6_pub.err"

# ---------------------------------------------------------------- 7: low-CPU (futex) wait mode
C=$(ch t7 "[wait=futex]")
start t7_futex --mode=sub --channel="$C" --label=futex --count=2000 --max-wait-ms=20000
start t7_spin --mode=sub --channel="$(ch t7 "")" --label=spin --count=2000 --max-wait-ms=20000
start t7_lowspin --mode=sub --channel="$(ch t7 "[wait=futex,spinCount=200]")" --label=lowspin --count=2000 --max-wait-ms=20000
sleep 0.3
start t7_pub --mode=pub --channel="$C" --count=2000 --rate=1000 --linger-ms=4000
sleep 0.5
# while messages flow at 1k/s
a1=$(cpu_ticks "$PID_t7_futex"); s1=$(cpu_ticks "$PID_t7_spin"); l1=$(cpu_ticks "$PID_t7_lowspin"); sleep 1
a2=$(cpu_ticks "$PID_t7_futex"); s2=$(cpu_ticks "$PID_t7_spin"); l2=$(cpu_ticks "$PID_t7_lowspin")
waitfor t7_futex t7_spin t7_lowspin
check 7 "futex reader received all 2000 in order" "[ '$(val t7_futex received)' = 2000 ] && [ '$(val t7_futex gaps)' = 0 ] && [ '$(val t7_futex corrupt)' = 0 ]"
FUT=$((a2-a1)); SPN=$((s2-s1)); LOW=$((l2-l1))
# with the default spinCount=20000 the reader spins ~0.2ms+ before each sleep, so at 1k msg/s
# it is not near 0; spinCount trades wake-up latency against CPU
check 7 "futex reader (default spinCount) CPU at 1k msg/s below half of spin reader (${FUT} vs ${SPN} ticks/s)" "[ $((FUT*2)) -lt $SPN ]"
check 7 "futex reader spinCount=200 CPU at 1k msg/s < 10% of a core (${LOW} ticks/s), all 2000 received" "[ $LOW -lt 10 ] && [ '$(val t7_lowspin received)' = 2000 ] && [ '$(val t7_lowspin gaps)' = 0 ]"
# idle: publisher alive but silent
start t7i_futex --mode=sub --channel="$C" --label=idle --idle-ms=100000 --max-wait-ms=100000
start t7i_pub --mode=pub --channel="$C" --count=1 --linger-ms=6000
sleep 2
a1=$(cpu_ticks "$PID_t7i_futex"); sleep 2; a2=$(cpu_ticks "$PID_t7i_futex")
kill -9 "$PID_t7i_futex" "$PID_t7i_pub" 2>/dev/null
check 7 "futex reader idle CPU near 0 ($((a2-a1)) ticks in 2s)" "[ $((a2-a1)) -le 4 ]"
waitfor t7_pub

# ---------------------------------------------------------------- 8: other MultiTransport entry points
C=$(ch t8 "")
start t8_typed --mode=sub --typed --channel="$C" --label=typed --count=5000
start t8_sync --mode=sub --sync --channel="$C" --label=sync --count=5000
start t8_bytes --mode=sub --channel="$C" --label=bytes --count=5000
sleep 0.3
start t8_pub --mode=pub --typed --channel="$C" --count=5000 --rate=50000 --heartbeat-channel="$(ch t8hb "")" --linger-ms=1500
sleep 0.4
start t8_fetch --mode=sub --fetch --channel="$C" --label=fetch
start t8_hb --mode=sub --raw --channel="$(ch t8hb "")" --label=heartbeat --count=3 --max-wait-ms=5000
waitfor t8_pub t8_typed t8_sync t8_bytes t8_fetch t8_hb
check 8 "typed listener (oneBroadcastListenerWithTopic) <- typed publisher (oneBroadcastPublisher)" "[ '$(val t8_typed received)' = 5000 ] && [ '$(val t8_typed gaps)' = 0 ] && [ '$(val t8_typed corrupt)' = 0 ]"
check 8 "byte-data listener (oneByteDataBroadcastListener) <- typed publisher" "[ '$(val t8_bytes received)' = 5000 ] && [ '$(val t8_bytes gaps)' = 0 ]"
check 8 "SynchronousRunner listener" "[ '$(val t8_sync received)' = 5000 ] && [ '$(val t8_sync gaps)' = 0 ] && [ '$(val t8_sync corrupt)' = 0 ]"
check 8 "fetchFirstUpdateAndDisconnect + typed variant" "[ '$(val t8_fetch ok)' = 1 ] && [ '$(val t8_fetch typedOk)' = 1 ]"
check 8 "heartbeats via initializeHeartbeatAndAlertComponent on shm_cursor" "[ '$(val t8_hb received)' -ge 3 ]"

echo
echo "SUMMARY pass=$PASS fail=$FAIL (logs in $W)"
[ "$FAIL" = 0 ]
