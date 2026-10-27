#!/usr/bin/env bash
# Multi-process functional tests for aeron_ipc (spec tests 1-8).
#
#   run_tests.sh <builddir> [test numbers...]     e.g. run_tests.sh ../../builddir_release 1 3
#
# Each test uses its own aeron dir under /dev/shm and its own stream, prints one
# RESULT line, and keeps all process output in $LOGDIR (default: a temp dir).
# The publisher embeds the media driver (driver=auto) unless a test says otherwise.
set -u

BUILDDIR=${1:?usage: run_tests.sh <builddir> [tests...]}
shift
TESTS=${*:-1 2 3 4 5 6 7 8}
BIN=$BUILDDIR/test/aeron_ipc_test/aeron_ipc_test
GRAPH=$BUILDDIR/test/aeron_ipc_test/aeron_ipc_graph_test
LOGDIR=${LOGDIR:-$(mktemp -d)}
DRIVER_CPU=${TM_AERON_IPC_DRIVER_CPU:-}
PASS=0
FAIL=0

for b in "$BIN" "$GRAPH"; do
    [ -x "$b" ] || { echo "missing $b"; exit 2; }
done
echo "logs in $LOGDIR"

dir_for() { echo "/dev/shm/aeron-tmtest-$USER-$1"; }
loc() {
    # loc <test> <extra props>
    local props="dir=$(dir_for "$1"),slotSize=256"
    [ -n "$DRIVER_CPU" ] && props="$props,driverCpu=$DRIVER_CPU"
    [ -n "${2:-}" ] && props="$props,$2"
    echo "aeron_ipc://::::t$1[$props]"
}
field() {
    # field <file> <TAG> <key> -> value from the last TAG line
    grep "^$2 " "$1" | tail -1 | tr ' ' '\n' | grep "^$3=" | cut -d= -f2
}
result() {
    # result <test> <ok 0/1> <message>
    if [ "$2" = 0 ]; then
        echo "RESULT test$1 PASS $3"
        PASS=$((PASS+1))
    else
        echo "RESULT test$1 FAIL $3"
        FAIL=$((FAIL+1))
    fi
}
cleanup_dir() { rm -rf "$(dir_for "$1")"; }

# 1. one publisher, three readers that keep up: everything in order with correct content
test1() {
    cleanup_dir 1
    local L; L=$(loc 1)
    for i in 1 2 3; do
        "$BIN" sub "$L" 12 - > "$LOGDIR/t1_sub$i.log" 2>&1 &
    done
    sleep 1
    # the subscribers start before the driver: they retry until it is live
    "$BIN" pub "$L" 100000 50000 64 t 1000 > "$LOGDIR/t1_pub.log" 2>&1
    wait
    local ok=0 msg=""
    for i in 1 2 3; do
        local f=$LOGDIR/t1_sub$i.log
        local r g ro b first
        r=$(field "$f" SUBDONE received); g=$(field "$f" SUBDONE gaps)
        ro=$(field "$f" SUBDONE reorders); b=$(field "$f" SUBDONE badContent)
        first=$(field "$f" SUBDONE firstSeq)
        msg="$msg sub$i:received=$r,first=$first,gaps=$g,reorders=$ro,bad=$b"
        [ "$r" = 100000 ] && [ "$first" = 0 ] && [ "$g" = 0 ] && [ "$ro" = 0 ] && [ "$b" = 0 ] || ok=1
    done
    result 1 $ok "$msg $(grep ^PUBSTATS "$LOGDIR/t1_pub.log")"
}

# 2. topic filtering: exact, regex and no selection see the right subsets
test2() {
    cleanup_dir 2
    local L; L=$(loc 2)
    "$BIN" sub "$L" 8 a.x > "$LOGDIR/t2_exact.log" 2>&1 &
    "$BIN" sub "$L" 8 'r/a\..*/' > "$LOGDIR/t2_regex.log" 2>&1 &
    "$BIN" sub "$L" 8 - > "$LOGDIR/t2_all.log" 2>&1 &
    sleep 1
    "$BIN" pub "$L" 30000 20000 64 a.x,a.y,b.z 1000 > "$LOGDIR/t2_pub.log" 2>&1
    wait
    local e r a eb rb
    e=$(field "$LOGDIR/t2_exact.log" SUBDONE received); eb=$(field "$LOGDIR/t2_exact.log" SUBDONE badTopic)
    r=$(field "$LOGDIR/t2_regex.log" SUBDONE received); rb=$(field "$LOGDIR/t2_regex.log" SUBDONE badTopic)
    a=$(field "$LOGDIR/t2_all.log" SUBDONE received)
    local ok=1
    [ "$e" = 10000 ] && [ "$r" = 20000 ] && [ "$a" = 30000 ] && [ "$eb" = 0 ] && [ "$rb" = 0 ] && ok=0
    result 2 $ok "exact=$e(bad $eb) regex=$r(bad $rb) all=$a"
}

# 3. slow reader: the publisher and a fast reader must not be held back.
#    Aeron gap: an untethered slow reader is ejected (linger, rest, rejoin at the
#    publisher's position), it does not lose individual messages one by one.
test3() {
    cleanup_dir 3
    local L; L=$(loc 3)
    "$BIN" sub "$L" 14 - > "$LOGDIR/t3_fast.log" 2>&1 &
    "$BIN" sub "$L" 14 - 200 > "$LOGDIR/t3_slow.log" 2>&1 &
    sleep 1
    "$BIN" pub "$L" 500000 50000 64 t 1000 > "$LOGDIR/t3_pub.log" 2>&1
    wait
    local fr fg sr sg sm srb bp hz mc sc
    fr=$(field "$LOGDIR/t3_fast.log" SUBDONE received); fg=$(field "$LOGDIR/t3_fast.log" SUBDONE gaps)
    sr=$(field "$LOGDIR/t3_slow.log" SUBDONE received); sg=$(field "$LOGDIR/t3_slow.log" SUBDONE gaps)
    sm=$(field "$LOGDIR/t3_slow.log" SUBDONE missing); srb=$(field "$LOGDIR/t3_slow.log" SUBDONE imagesJoined)
    bp=$(field "$LOGDIR/t3_pub.log" PUBSTATS backPressured)
    hz=$(field "$LOGDIR/t3_pub.log" PUBDONE achievedHz); mc=$(field "$LOGDIR/t3_pub.log" PUBDONE maxCallUs)
    sc=$(field "$LOGDIR/t3_pub.log" PUBDONE slowCalls)
    local ok=1
    [ "$fr" = 500000 ] && [ "$fg" = 0 ] && ok=0
    result 3 $ok "fast:received=$fr,gaps=$fg slow:received=$sr,gaps=$sg,missing=$sm,imagesJoined=$srb pub:achievedHz=$hz,maxCallUs=$mc,slowCalls=$sc,backPressured=$bp"
}

# 4. reader crash and restart: publisher unaffected, restarted reader starts at the latest message
test4() {
    cleanup_dir 4
    local L; L=$(loc 4)
    "$BIN" pub "$L" 400000 20000 64 t 500 > "$LOGDIR/t4_pub.log" 2>&1 &
    local pub=$!
    sleep 2
    "$BIN" sub "$L" 30 - > "$LOGDIR/t4_victim.log" 2>&1 &
    local victim=$!
    sleep 3
    kill -9 $victim; wait $victim 2>/dev/null
    sleep 3
    # restarted reader: first message must be close to the publisher's current position
    "$BIN" sub "$L" 4 - > "$LOGDIR/t4_restart.log" 2>&1
    local first lat skipped
    first=$(field "$LOGDIR/t4_restart.log" SUBDONE firstSeq)
    lat=$(field "$LOGDIR/t4_restart.log" SUBDONE firstLatUs)
    skipped=$(field "$LOGDIR/t4_restart.log" SUBDONE skippedOnJoin)
    wait $pub
    local hz ex
    hz=$(field "$LOGDIR/t4_pub.log" PUBDONE achievedHz)
    ex=$(grep -c "ERROR\|error" "$LOGDIR/t4_pub.log")
    local ok=1
    # 8s after the publisher started at 20kHz it is around seq 160000; join-from-latest
    # means the first delivered seq is beyond what was published before the reader started
    [ -n "$first" ] && [ "$first" -gt 150000 ] && [ "$lat" -lt 1000 ] && [ "$ex" = 0 ] && ok=0
    result 4 $ok "restart:firstSeq=$first,firstLatUs=$lat,skippedOnJoin=$skipped pub:achievedHz=$hz,errors=$ex"
}

# 5. publisher restart (embedded driver dies with it): readers recover onto the new publisher
test5() {
    cleanup_dir 5
    local L; L=$(loc 5 "driverTimeoutMs=2000")
    "$BIN" sub "$L" 20 - > "$LOGDIR/t5_sub.log" 2>&1 &
    local sub=$!
    sleep 1
    "$BIN" pub "$L" 1000000 20000 64 t 0 > "$LOGDIR/t5_pub1.log" 2>&1 &
    local pub=$!
    sleep 4
    kill -9 $pub; wait $pub 2>/dev/null
    local killed_at; killed_at=$(date +%s.%N)
    sleep 1
    "$BIN" pub "$L" 100000 20000 64 t 1000 > "$LOGDIR/t5_pub2.log" 2>&1
    wait $sub
    local r ro rb ij
    r=$(field "$LOGDIR/t5_sub.log" SUBDONE received); ro=$(field "$LOGDIR/t5_sub.log" SUBDONE reorders)
    rb=$(field "$LOGDIR/t5_sub.log" SUBDONE clientRebuilds); ij=$(field "$LOGDIR/t5_sub.log" SUBDONE imagesJoined)
    local last; last=$(field "$LOGDIR/t5_sub.log" SUBDONE lastSeq)
    local ok=1
    # recovered if the reader saw the second publisher's sequence restart and reached its end
    [ "$ro" -ge 1 ] && [ "$last" -ge 99000 ] && ok=0
    result 5 $ok "received=$r,reorders=$ro,lastSeq=$last,clientRebuilds=$rb,imagesJoined=$ij"
}

# 6. oversized message is rejected, normal traffic continues
test6() {
    cleanup_dir 6
    local L; L=$(loc 6)
    "$BIN" sub "$L" 5 - > "$LOGDIR/t6_sub.log" 2>&1 &
    sleep 0.5
    "$BIN" oversized "$L" > "$LOGDIR/t6_pub.log" 2>&1
    local rc=$?
    wait
    local r; r=$(field "$LOGDIR/t6_sub.log" SUBDONE received)
    local ok=1
    [ $rc = 0 ] && [ "$r" = 1000 ] && ok=0
    result 6 $ok "$(grep ^OVERSIZED "$LOGDIR/t6_pub.log") received=$r $(grep ^PUBSTATS "$LOGDIR/t6_pub.log")"
}

# 7. low-CPU wait strategy: same correctness, CPU use well below a spinning reader
test7() {
    cleanup_dir 7
    local L; L=$(loc 7)
    "$BIN" sub "$L" 10 - > "$LOGDIR/t7_spin.log" 2>&1 &
    "$BIN" sub "${L%]},wait=lowcpu]" 10 - > "$LOGDIR/t7_low.log" 2>&1 &
    sleep 1
    "$BIN" pub "$L" 50000 10000 64 t 1000 > "$LOGDIR/t7_pub.log" 2>&1
    wait
    local sr sc lr lc lg lm
    sr=$(field "$LOGDIR/t7_spin.log" SUBDONE received); sc=$(field "$LOGDIR/t7_spin.log" SUBDONE cpuPct)
    lr=$(field "$LOGDIR/t7_low.log" SUBDONE received); lc=$(field "$LOGDIR/t7_low.log" SUBDONE cpuPct)
    lg=$(field "$LOGDIR/t7_low.log" SUBDONE gaps); lm=$(field "$LOGDIR/t7_low.log" SUBDONE maxLatUs)
    local ok=1
    [ "$lr" = 50000 ] && [ "$lg" = 0 ] && [ "$lc" -lt 50 ] && ok=0
    result 7 $ok "spin:received=$sr,cpuPct=$sc lowcpu:received=$lr,gaps=$lg,cpuPct=$lc,maxLatUs=$lm"
}

# 8. MultiTransport channel strings inside a TM Kit graph (component-only environment)
test8() {
    cleanup_dir 8
    local C="aeron_ipc://::::t8[dir=$(dir_for 8),slotSize=256]"
    "$GRAPH" sub "$C" g.a 8 > "$LOGDIR/t8_exact.log" 2>&1 &
    "$GRAPH" sub "$C" 'r/g\..*/' 8 > "$LOGDIR/t8_regex.log" 2>&1 &
    "$GRAPH" sub "$C" "" 8 > "$LOGDIR/t8_all.log" 2>&1 &
    sleep 1
    "$GRAPH" pub "$C" 3000 g.a,g.b,h.c > "$LOGDIR/t8_pub.log" 2>&1
    wait
    local e r a
    e=$(field "$LOGDIR/t8_exact.log" GRAPHSUB received)
    r=$(field "$LOGDIR/t8_regex.log" GRAPHSUB received)
    a=$(field "$LOGDIR/t8_all.log" GRAPHSUB received)
    local ok=1
    [ "$e" = 1000 ] && [ "$r" = 2000 ] && [ "$a" = 3000 ] && ok=0
    result 8 $ok "exact=$e regex=$r all=$a $(grep ^GRAPHSUB "$LOGDIR/t8_all.log" | sed 's/.*topics=/topics=/')"
}

for t in $TESTS; do
    "test$t"
    cleanup_dir "$t"
done
echo "SUMMARY pass=$PASS fail=$FAIL logs=$LOGDIR"
[ $FAIL = 0 ]
