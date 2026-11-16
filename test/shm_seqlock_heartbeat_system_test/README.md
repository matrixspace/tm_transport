# ShmSeqlock real Heartbeat system tests

The worker uses the real HeartbeatAndAlertComponent and transport, graph timer,
MultiTransport discovery, and HeartbeatMessageToRemoteFacilityCommandConverter.
Python supplies the independent control channel, phase deadlines, 30-second case
watchdog, exact byte checks, process exit accounting, and registered-resource cleanup.

Build from this worktree (standalone avoids unrelated transport dependencies):

```sh
source /data/hxli/codes/tm_kit/build-common/build-env-setting.sh
meson setup builddir_heartbeat_debug test/shm_seqlock_heartbeat_system_test/standalone \
  --buildtype=debug --prefix="$INSTALL_PREFIX"
ninja -j1 -C builddir_heartbeat_debug
python3 test/shm_seqlock_heartbeat_system_test/run_tests.py \
  builddir_heartbeat_debug/shm_seqlock_heartbeat_system_worker \
  --output /tmp/shm-heartbeat-debug
```

The root Meson build can include this directory and build target
`test/shm_seqlock_heartbeat_system_test/shm_seqlock_heartbeat_system_worker`.
Release uses a separate build directory and `--buildtype=release`.
`--case` and `--layout` can repeat; default is ten supported cases plus one isolated risk probe, each in three layouts.
Output includes per-worker logs, invocation parameters, JSON results, and watchdog
snapshots on timeout. A failed case exits nonzero even if later cases pass.

| Case | Plan | Actual assertions |
|---|---|---|
| direct | H01/H02/H05 | UUID/PID/service/time/host/status/channel fields; actual discovery delivers binary business; alert/custom decoding; repeated heartbeat does not duplicate subscriptions |
| ttl | H06 | Actual facility converter Register, timeout Deregister exactly once, new heartbeat Register |
| shared | H02/H03 | Explicit heartbeat/data calls on one segment; exact topic/type isolation |
| timer | H01/H03/H09 | Real graph timer overlaps 400 data sends on same segment with first writerLock; exact content; short furthestPoint stops; converter expires; explicit heartbeat restores it without restarting the timer |
| timer_separate | H03/H09 | Same timer/data workload on independent segments |
| growth | H07 | Grow status to exactly fill 1024B, 1025B rejected with exact stats delta, legal heartbeat resumes |
| restart | H05/H06 | SIGKILL, timeout, writer takeover, new UUID, continued business delivery; dead-writer residual latest is not delivered to late client |
| address | H05 | Real discovery adds new locator and retains old locator according to append-only implementation |
| lock | H08 | SIGSTOP service retains lock; second process fails explicitly; independent segments discover independently |
| slow_reader | H06 | Gate a real graph callback beyond TTL; CHECK expires prior acceptance; releasing the delayed heartbeat restores registration using acceptance time; business resumes |
| risk_first_unlocked | H04 | Isolated concurrent data/heartbeat burst after first sender creation without a lock and later writerLock=true; report RISK_OBSERVED / RISK_NOT_OBSERVED / RISK_INCONCLUSIVE, never SAFE |

Broadcast discovery itself is append-only and has no TTL de-subscription. TTL
assertions above test the real facility command converter using a socket_rpc
locator in actual heartbeat metadata; no RPC transport connection is claimed.
The timer uses explicit finite lifetime; it is not a 24-hour soak.
Saturated heartbeat starvation, very long runs, and performance distributions
remain separate coverage; this suite
must not be reported as full PLAN completion.

The worker's final drain checks records emitted after the last expected callback,
including malformed heartbeats that the typed importer would otherwise drop,
extra data deliveries and unconsumed registration events. Risk cases deliberately
report observations separately and do not claim a supported-usage pass.
