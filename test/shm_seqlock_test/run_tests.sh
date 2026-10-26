#!/bin/bash
#
# Functional tests for the shm_seqlock transport (multi-process, through the
# MultiTransport channel string). Not a latency measurement.
#
# Usage: run_tests.sh [path/to/shm_seqlock_test] [log dir]
#
# Every test uses its own segment name (prefix shmseqlock_t<pid>_) and the
# segments are removed on exit.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$HERE/../../builddir_release/test/shm_seqlock_test/shm_seqlock_test}
OUT=${2:-/tmp/shm_seqlock_test.$$}
mkdir -p "$OUT"
PFX=shmseqlock_t$$
PASS=0
FAIL=0
CLK_TCK=$(getconf CLK_TCK)

if [ ! -x "$BIN" ]; then
    echo "test binary $BIN not found"
    exit 2
fi

cleanup() {
    pkill -9 -P $$ 2>/dev/null
    rm -f /dev/shm/${PFX}_*
}
trap cleanup EXIT

result() { # name ok(1/0) detail
    if [ "$2" = 1 ]; then
        echo "PASS $1: $3"
        PASS=$((PASS+1))
    else
        echo "FAIL $1: $3"
        FAIL=$((FAIL+1))
    fi
}
# value of key=value on the last SUBRESULT/PUBRESULT line of a log
field() {
    grep -E '^(SUBRESULT|PUBRESULT)' "$1" | tail -1 | tr ' ' '\n' | grep "^$2=" | head -1 | cut -d= -f2
}
cputicks() {
    awk '{print $14+$15}' /proc/$1/stat 2>/dev/null || echo 0
}
summary() {
    grep -E '^(SUBRESULT|PUBRESULT)' "$1" | tail -1
}

# ---------------------------------------------------------------- stress (in-process)
for v in "spec spin" "classic spin" "spec futex" "classic futex"; do
    set -- $v
    log=$OUT/stress_$1_$2.log
    "$BIN" stress --segment=${PFX}_stress_$1_$2 --readers=3 --read=$1 --wait=$2 \
        --slotCount=64 --slotSize=512 --count=3000000 --readerSlowEvery=1000 > "$log" 2>&1
    rc=$?
    ok=0; [ $rc = 0 ] && grep -q '^STRESS PASS' "$log" && ok=1
    result "stress read=$1 wait=$2" $ok "$(grep -E '^  reader' "$log" | sed 's/^ *//' | tr '\n' ';')"
done

# ---------------------------------------------------------------- test 1
# 1 publisher, 3 readers, 100k messages, readers keep up
t1() { # tag pubProps subProps
    local tag=$1 pp=$2 sp=$3
    local seg=${PFX}_t1_$tag
    local pids=()
    for i in 1 2 3; do
        "$BIN" sub --channel="shm_seqlock://::::$seg[$sp]" --count=100000 --timeoutSec=30 \
            --payload=256 > "$OUT/t1_${tag}_sub$i.log" 2>&1 &
        pids+=($!)
    done
    sleep 0.3
    "$BIN" pub --channel="shm_seqlock://::::$seg[$pp]" --count=100000 --intervalNs=2000 \
        --payload=256 --startDelayMs=800 > "$OUT/t1_${tag}_pub.log" 2>&1
    wait "${pids[@]}"
    local ok=1 detail=""
    for i in 1 2 3; do
        local f=$OUT/t1_${tag}_sub$i.log
        [ "$(field $f received)" = 100000 ] && [ "$(field $f bad)" = 0 ] && [ "$(field $f ooo)" = 0 ] \
            && [ "$(field $f gaps)" = 0 ] && [ "$(field $f topicBad)" = 0 ] && [ "$(field $f first)" = 0 ] \
            && [ "$(field $f sizeMismatch)" = 0 ] && [ "$(field $f lost)" = 0 ] || ok=0
        detail="$detail sub$i{received=$(field $f received) gaps=$(field $f gaps) bad=$(field $f bad) first=$(field $f first)}"
    done
    result "1 (3 readers in order, $tag)" $ok "$detail"
}
t1 spec_spin "slotSize=1000,slotCount=4096" "read=spec"
t1 classic_spin "slotSize=1000,slotCount=4096" "read=classic"
t1 spec_futex "slotSize=1000,slotCount=4096,wait=futex" "read=spec,wait=futex"
# slotSize=1050: stride 1088 by default, 1152 with stride128
t1 spec_stride128 "slotSize=1050,slotCount=4096,stride128=true" "read=spec"

# ---------------------------------------------------------------- test 2
# topic filtering: exact string, regex (r/../ and rabbitmq style), NoTopicSelection
seg=${PFX}_t2
ch="shm_seqlock://::::$seg[slotSize=1000,slotCount=4096]"
"$BIN" sub --channel="$ch" --topic='a.x' --expectTopicRe='a\.x' --timeoutSec=20 --idleExitSec=2 > "$OUT/t2_exact.log" 2>&1 &
p1=$!
"$BIN" sub --channel="$ch" --topic='r/a\..*/' --expectTopicRe='a\..*' --timeoutSec=20 --idleExitSec=2 > "$OUT/t2_regex.log" 2>&1 &
p2=$!
"$BIN" sub --channel="$ch" --topic='a.*' --expectTopicRe='a\..*' --timeoutSec=20 --idleExitSec=2 > "$OUT/t2_rmqstyle.log" 2>&1 &
p3=$!
"$BIN" sub --channel="$ch" --timeoutSec=20 --idleExitSec=2 > "$OUT/t2_all.log" 2>&1 &
p4=$!
sleep 0.3
"$BIN" pub --channel="$ch" --count=30000 --intervalNs=2000 --topics=a.x,a.y,b.x --startDelayMs=800 > "$OUT/t2_pub.log" 2>&1
wait $p1 $p2 $p3 $p4
t2check() { # name log expected
    local ok=0
    [ "$(field $2 received)" = $3 ] && [ "$(field $2 topicBad)" = 0 ] && [ "$(field $2 bad)" = 0 ] && ok=1
    result "2 (topic $1)" $ok "$(summary $2)"
}
t2check "exact a.x" "$OUT/t2_exact.log" 10000
t2check "regex r/a\\..*/" "$OUT/t2_regex.log" 20000
t2check "rabbitmq-style a.*" "$OUT/t2_rmqstyle.log" 20000
t2check "NoTopicSelection" "$OUT/t2_all.log" 30000

# ---------------------------------------------------------------- test 3
# slow reader loses messages, publisher rate unaffected, other readers lose nothing
t3run() { # tag withSlow
    local seg=${PFX}_t3_$1
    local ch="shm_seqlock://::::$seg[slotSize=1000,slotCount=4096]"
    local pids=()
    for i in 1 2; do
        "$BIN" sub --channel="$ch" --count=200000 --timeoutSec=30 > "$OUT/t3_$1_fast$i.log" 2>&1 &
        pids+=($!)
    done
    if [ "$2" = 1 ]; then
        "$BIN" sub --channel="$ch" --sleepUsPerMsg=200 --timeoutSec=4 > "$OUT/t3_$1_slow.log" 2>&1 &
        pids+=($!)
    fi
    sleep 0.3
    "$BIN" pub --channel="$ch" --count=200000 --intervalNs=2000 --startDelayMs=800 > "$OUT/t3_$1_pub.log" 2>&1
    wait "${pids[@]}"
}
t3run base 0
t3run slow 1
ok=1
for i in 1 2; do
    f=$OUT/t3_slow_fast$i.log
    [ "$(field $f received)" = 200000 ] && [ "$(field $f gaps)" = 0 ] && [ "$(field $f bad)" = 0 ] || ok=0
done
slowLost=$(field "$OUT/t3_slow_slow.log" lost)
[ -n "$slowLost" ] && [ "$slowLost" -gt 0 ] && [ "$(field $OUT/t3_slow_slow.log bad)" = 0 ] \
    && [ "$(field $OUT/t3_slow_slow.log ooo)" = 0 ] || ok=0
baseNs=$(field "$OUT/t3_base_pub.log" publishNsAvg)
slowNs=$(field "$OUT/t3_slow_pub.log" publishNsAvg)
baseSec=$(field "$OUT/t3_base_pub.log" elapsedSec)
slowSec=$(field "$OUT/t3_slow_pub.log" elapsedSec)
# publish cost with a slow reader must stay within 1.5x (+50ns) of the baseline
awk -v a="$baseNs" -v b="$slowNs" 'BEGIN { exit !(b <= a*1.5+50) }' || ok=0
result "3 (slow reader)" $ok "publishNsAvg base=$baseNs withSlow=$slowNs elapsedSec base=$baseSec withSlow=$slowSec; slow: $(summary $OUT/t3_slow_slow.log); fast1: received=$(field $OUT/t3_slow_fast1.log received) gaps=$(field $OUT/t3_slow_fast1.log gaps); fast2: received=$(field $OUT/t3_slow_fast2.log received) gaps=$(field $OUT/t3_slow_fast2.log gaps)"

# ---------------------------------------------------------------- test 4
# reader SIGKILLed and restarted: first message received is the latest, not backlog
seg=${PFX}_t4
ch="shm_seqlock://::::$seg[slotSize=1000,slotCount=4096]"
"$BIN" pub --channel="$ch" --count=0 --intervalNs=1000000 --logEvery=1 > "$OUT/t4_pub.log" 2>&1 &
pp=$!
sleep 0.5
"$BIN" sub --channel="$ch" --timeoutSec=30 > "$OUT/t4_subA.log" 2>&1 &
pa=$!
sleep 1
kill -9 $pa
wait $pa 2>/dev/null
sleep 1.5
latestAtJoin=$(grep '^SENT' "$OUT/t4_pub.log" | tail -1 | sed 's/.*seq=//')
"$BIN" sub --channel="$ch" --count=200 --timeoutSec=10 > "$OUT/t4_subB.log" 2>&1
first=$(grep '^FIRST' "$OUT/t4_subB.log" | head -1 | sed 's/.*seq=//')
kill -9 $pp
wait $pp 2>/dev/null
ok=0
# upper bound allows ~2s of process start-up at 1 msg/ms; the backlog would start ~4095 earlier
[ -n "$first" ] && [ -n "$latestAtJoin" ] && [ "$first" -ge "$latestAtJoin" ] && [ "$first" -le $((latestAtJoin+2000)) ] \
    && [ "$(field $OUT/t4_subB.log gaps)" = 0 ] && [ "$(field $OUT/t4_subB.log bad)" = 0 ] && ok=1
result "4 (reader SIGKILL+restart starts at latest)" $ok "latest sent before restart=$latestAtJoin first received=$first (ring 4096 slots, backlog would start near $((latestAtJoin>4095 ? latestAtJoin-4095 : 0))); $(summary $OUT/t4_subB.log)"

# ---------------------------------------------------------------- test 5
# publisher SIGKILLed and restarted: readers resume receiving
t5() { # tag pub2Props
    local seg=${PFX}_t5_$1
    "$BIN" sub --channel="shm_seqlock://::::$seg" --timeoutSec=10 > "$OUT/t5_$1_sub.log" 2>&1 &
    local ps=$!
    sleep 0.3
    "$BIN" pub --channel="shm_seqlock://::::$seg[slotSize=1000,slotCount=4096]" --run=1 --count=0 \
        --intervalNs=1000000 --startDelayMs=500 > "$OUT/t5_$1_pub1.log" 2>&1 &
    local p1=$!
    sleep 2.5
    kill -9 $p1
    wait $p1 2>/dev/null
    sleep 1
    "$BIN" pub --channel="shm_seqlock://::::$seg[$2]" --run=2 --count=2000 --intervalNs=1000000 \
        --startDelayMs=500 > "$OUT/t5_$1_pub2.log" 2>&1
    wait $ps
    local f=$OUT/t5_$1_sub.log
    local line=$(summary $f)
    local r1=$(echo "$line" | tr ' ' '\n' | grep '^run1=' | cut -d= -f2)
    local r2=$(echo "$line" | tr ' ' '\n' | grep '^run2=' | cut -d= -f2)
    local ok=0
    [ -n "$r1" ] && [ "$r1" -gt 0 ] && [ "$r2" = 2000 ] && [ "$(field $f bad)" = 0 ] && [ "$(field $f ooo)" = 0 ] \
        && [ "$(field $f restarts)" -ge 1 ] && ok=1
    result "5 (publisher SIGKILL+restart, $1)" $ok "$line"
}
t5 same_layout "slotSize=1000,slotCount=4096"
t5 new_layout "slotSize=500,slotCount=1024"

# ---------------------------------------------------------------- test 6
# oversized message rejected without corruption; a message that exactly fills the slot passes
seg=${PFX}_t6
ch="shm_seqlock://::::$seg[slotSize=1000,slotCount=4096]"
"$BIN" sub --channel="$ch" --count=10000 --payload=997 --timeoutSec=20 > "$OUT/t6_sub.log" 2>&1 &
ps=$!
sleep 0.3
"$BIN" pub --channel="$ch" --count=10000 --intervalNs=2000 --payload=997 --oversizeEvery=100 \
    --oversizeLen=998 --startDelayMs=800 > "$OUT/t6_pub.log" 2>&1
wait $ps
ok=0
f=$OUT/t6_sub.log
[ "$(field $f received)" = 10000 ] && [ "$(field $f bad)" = 0 ] && [ "$(field $f sizeMismatch)" = 0 ] \
    && [ "$(field $f gaps)" = 0 ] && [ "$(field $f ooo)" = 0 ] \
    && [ "$(field $OUT/t6_pub.log rejected)" = 100 ] && [ "$(field $OUT/t6_pub.log oversizeSent)" = 100 ] && ok=1
result "6 (oversize rejected, 3+997B fits, 3+998B rejected)" $ok "$(summary $OUT/t6_pub.log); $(summary $f); stderr: $(grep -c 'rejected message' $OUT/t6_pub.log) log line(s)"

# ---------------------------------------------------------------- test 7
# low-CPU wait mode: reader CPU near 0 while messages trickle in
t7() { # tag pubProps subProps
    local seg=${PFX}_t7_$1
    "$BIN" pub --channel="shm_seqlock://::::$seg[$2]" --count=0 --intervalNs=200000000 > "$OUT/t7_$1_pub.log" 2>&1 &
    local pp=$!
    sleep 0.5
    "$BIN" sub --channel="shm_seqlock://::::$seg[$3]" --timeoutSec=7 > "$OUT/t7_$1_sub.log" 2>&1 &
    local ps=$!
    sleep 2
    local c0=$(cputicks $ps)
    sleep 4
    local c1=$(cputicks $ps)
    wait $ps
    kill -9 $pp
    wait $pp 2>/dev/null
    T7CPU=$(awk -v a=$c0 -v b=$c1 -v t=$CLK_TCK 'BEGIN { printf "%.1f", (b-a)/t/4*100 }')
    T7REC=$(field "$OUT/t7_$1_sub.log" received)
    T7GAPS=$(field "$OUT/t7_$1_sub.log" gaps)
}
t7 futex "slotSize=1000,slotCount=4096,wait=futex" "wait=futex"
futexCpu=$T7CPU; futexRec=$T7REC; futexGaps=$T7GAPS
t7 futex_reader_only "slotSize=1000,slotCount=4096" "wait=futex"
froCpu=$T7CPU; froRec=$T7REC
t7 spin "slotSize=1000,slotCount=4096" "wait=spin"
spinCpu=$T7CPU
ok=0
awk -v c=$futexCpu 'BEGIN { exit !(c < 5) }' && [ -n "$futexRec" ] && [ "$futexRec" -ge 25 ] && [ "$futexGaps" = 0 ] && ok=1
result "7 (futex wait mode idle CPU)" $ok "reader CPU: futex=${futexCpu}% (received $futexRec, gaps $futexGaps), futex reader + spin publisher=${froCpu}% (received $froRec), busy-spin control=${spinCpu}%"

# ---------------------------------------------------------------- test 8
# all pub/sub runs above go through oneByteDataBroadcastPublisher / oneByteDataBroadcastListener;
# additionally check fetchFirstUpdateAndDisconnect (component-only Env) via the channel string
ok=0
[ $FAIL = 0 ] && ok=1
result "8 (MultiTransport oneByteDataBroadcastPublisher/Listener with channel string)" $ok "covered by tests 1-7 (pub/sub modes use only the channel string)"
seg=${PFX}_t8
ch="shm_seqlock://::::$seg[slotSize=1000,slotCount=4096]"
"$BIN" pub --channel="$ch" --count=0 --intervalNs=1000000 --topics=a.x,a.y,b.x > "$OUT/t8_pub.log" 2>&1 &
pp=$!
sleep 0.5
"$BIN" fetch --channel="$ch" --topic='b.x' --timeoutSec=10 > "$OUT/t8_fetch.log" 2>&1
rc=$?
kill -9 $pp
wait $pp 2>/dev/null
ok=0
[ $rc = 0 ] && grep -q 'FETCHRESULT ok=1 topic=b.x' "$OUT/t8_fetch.log" && ok=1
result "8b (fetchFirstUpdateAndDisconnect)" $ok "$(grep FETCHRESULT $OUT/t8_fetch.log)"

echo "SUMMARY pass=$PASS fail=$FAIL logs=$OUT"
[ $FAIL = 0 ]
