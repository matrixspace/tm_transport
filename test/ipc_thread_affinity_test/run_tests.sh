#!/usr/bin/env bash
# Thread CPU affinity tests for the IPC transports.
#
#   run_tests.sh <path-to-ipc_thread_affinity_test>
#
# The test needs at least 4 CPUs in the process affinity mask (it binds its
# worker threads to CPUs the process is allowed to use); it self-checks the
# affinity and exits non-zero on failure. Set IPC_AFFINITY_CASE=<transport> to
# run one transport's matrix (shm_seqlock, shm_cursor, shared_memory_broadcast,
# zeromq, nng, multicast, singlecast, aeron_ipc), or IPC_AFFINITY_TAG to change
# the /dev/shm name prefix.
set -u

BIN=${1:?usage: run_tests.sh <path-to-ipc_thread_affinity_test>}
[ -x "$BIN" ] || { echo "missing $BIN"; exit 2; }

LOGDIR=${LOGDIR:-$(mktemp -d /tmp/ipc_thread_affinity_test.XXXXXX)}
mkdir -p "$LOGDIR"
export IPC_AFFINITY_TAG=${IPC_AFFINITY_TAG:-aff$$}

cleanup() {
    rm -f /dev/shm/${IPC_AFFINITY_TAG}_*
    rm -rf /dev/shm/aeron_${IPC_AFFINITY_TAG}_*
}
trap cleanup EXIT

echo "logs in $LOGDIR"
NBEFORE=$(ls /dev/shm | grep -c "^${IPC_AFFINITY_TAG}_" || true)
"$BIN" 2>&1 | tee "$LOGDIR/test.log"
RC=${PIPESTATUS[0]}
# shared_memory_broadcast segments are persistent by design; remove this run's
cleanup
NAFTER=$(ls /dev/shm | grep -c "^${IPC_AFFINITY_TAG}_" || true)
if [ "$NAFTER" != "$NBEFORE" ]; then
    echo "WARN: test left shared memory segments behind ($NBEFORE -> $NAFTER)" | tee -a "$LOGDIR/test.log"
fi
grep -E '^SUMMARY:' "$LOGDIR/test.log" || true
[ "$RC" -eq 0 ] && echo "PASS" || echo "FAIL (exit $RC)"
exit "$RC"
