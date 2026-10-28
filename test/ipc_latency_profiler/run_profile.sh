#!/usr/bin/env bash
# Orchestrates one IPC latency profiling run (floor and/or e2e).
# Every profiling run, for every candidate transport, must go through this
# script: it serialises runs across worktrees, checks that the box is quiet,
# pins processes, starts readers before the publisher and cleans up on
# timeout or Ctrl-C.
#
# Usage: run_profile.sh [options]
#   --mode e2e|floor        e2e (default) runs floor first unless --no-floor
#   --channel SPEC          MultiTransport channel; "{run_id}" is replaced by a
#                           per-run id so that every run gets fresh segments,
#                           e.g. 'shared_memory_broadcast://::::ipcprof_{run_id}[size=268435456,busyLoop=true]'
#                           (identifier is the 5th ":" field; all properties go in ONE [..] separated by ",")
#   --readers 1|2|4         number of reader processes (default 1)
#   --clock default|simplified|tsc (default default)
#   --sink simple|custom          (default simple)
#   --payload N             bytes (default 256)
#   --interval-us X         (default 20)
#   --count N               measured messages (default 1000000)
#   --warmup N              warmup messages (default 100000)
#   --topic S               (default ipc.profile)
#   --results PATH          JSON lines output (default $IPC_PROFILE_RESULTS or
#                           /data/hxli/codes/tm_kit/worktrees/ipc_profiling_results.jsonl)
#   --raw-dir DIR           dump raw samples per reader into DIR
#   --pub-cpu N             (default 2)
#   --reader-cpus a,b,c,d   (default 4,6,8,10)
#   --housekeeping-cpu N    core for non hot path main threads and the setup
#                           command (default 12)
#   --setup-cmd "CMD"       helper started (via bash -c, own process group,
#                           pinned to the housekeeping core) before the e2e
#                           readers, e.g. a broker daemon; killed on exit
#   --setup-wait-sec N      seconds to wait after starting it (default 2); it
#                           must still be alive afterwards
#   --extra-env KEY=VAL     extra environment for every child (repeatable)
#   --reader-ready-delay-ms N  readers wait this long after their subscription
#                           call returns before signalling ready, for
#                           transports that connect asynchronously (default 0)
#   --no-floor              skip the floor run before e2e
#   --floor-count N         measured floor messages per pair (default 200000)
#   --floor-warmup N        (default 20000)
#   --load-threshold X      max 1-min loadavg (default 1.0)
#   --load-wait-s N         wait up to N s for loadavg to drop below the
#                           threshold before failing the check (default 180;
#                           a previous profiling run raises loadavg itself)
#   --idle-threshold X      min idle percent on target cores+siblings (default 98)
#   --timeout-s N           overall timeout per phase (default derived from counts)
#   --force                 run even if checks fail (recorded in results)
#   --bin PATH              profiler binary (default <worktree>/builddir_release/test/ipc_latency_profiler/ipc_latency_profiler)
#   -h|--help

set -u
set -o pipefail

LOCK_FILE=/data/hxli/codes/tm_kit/worktrees/.ipc_profiling.lock

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKTREE_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel 2>/dev/null || (cd "${SCRIPT_DIR}/../.." && pwd))"

MODE=e2e
CHANNEL=""
READERS=1
CLOCK=default
SINK=simple
PAYLOAD=256
INTERVAL_US=20
COUNT=1000000
WARMUP=100000
TOPIC=ipc.profile
RESULTS="${IPC_PROFILE_RESULTS:-/data/hxli/codes/tm_kit/worktrees/ipc_profiling_results.jsonl}"
RAW_DIR=""
PUB_CPU=2
READER_CPUS="4,6,8,10"
HOUSEKEEPING_CPU=12
SETUP_CMD=""
SETUP_WAIT_SEC=2
EXTRA_ENV=()
READER_READY_DELAY_MS=0
RUN_FLOOR=1
FLOOR_COUNT=200000
FLOOR_WARMUP=20000
LOAD_THRESHOLD=1.0
LOAD_WAIT_S=180
IDLE_THRESHOLD=98
TIMEOUT_S=""
FORCE=0
BIN="${WORKTREE_ROOT}/builddir_release/test/ipc_latency_profiler/ipc_latency_profiler"

usage() {
  sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

die() {
  echo "run_profile.sh: $*" >&2
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --mode) MODE="$2"; shift 2 ;;
    --channel) CHANNEL="$2"; shift 2 ;;
    --readers) READERS="$2"; shift 2 ;;
    --clock) CLOCK="$2"; shift 2 ;;
    --sink) SINK="$2"; shift 2 ;;
    --payload) PAYLOAD="$2"; shift 2 ;;
    --interval-us) INTERVAL_US="$2"; shift 2 ;;
    --count) COUNT="$2"; shift 2 ;;
    --warmup) WARMUP="$2"; shift 2 ;;
    --topic) TOPIC="$2"; shift 2 ;;
    --results) RESULTS="$2"; shift 2 ;;
    --raw-dir) RAW_DIR="$2"; shift 2 ;;
    --pub-cpu) PUB_CPU="$2"; shift 2 ;;
    --reader-cpus) READER_CPUS="$2"; shift 2 ;;
    --housekeeping-cpu) HOUSEKEEPING_CPU="$2"; shift 2 ;;
    --setup-cmd) SETUP_CMD="$2"; shift 2 ;;
    --setup-wait-sec) SETUP_WAIT_SEC="$2"; shift 2 ;;
    --extra-env)
      [[ "$2" =~ ^[A-Za-z_][A-Za-z0-9_]*=.*$ ]] || die "--extra-env expects KEY=VAL, got '$2'"
      EXTRA_ENV+=("$2"); shift 2 ;;
    --reader-ready-delay-ms) READER_READY_DELAY_MS="$2"; shift 2 ;;
    --no-floor) RUN_FLOOR=0; shift ;;
    --floor-count) FLOOR_COUNT="$2"; shift 2 ;;
    --floor-warmup) FLOOR_WARMUP="$2"; shift 2 ;;
    --load-threshold) LOAD_THRESHOLD="$2"; shift 2 ;;
    --load-wait-s) LOAD_WAIT_S="$2"; shift 2 ;;
    --idle-threshold) IDLE_THRESHOLD="$2"; shift 2 ;;
    --timeout-s) TIMEOUT_S="$2"; shift 2 ;;
    --force) FORCE=1; shift ;;
    --bin) BIN="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option '$1' (see --help)" ;;
  esac
done

# ------------------------------------------------------------- validation ---
case "${MODE}" in e2e|floor) ;; *) die "--mode must be e2e or floor" ;; esac
case "${READERS}" in 1|2|4) ;; *) die "--readers must be 1, 2 or 4" ;; esac
case "${CLOCK}" in default|simplified|tsc) ;; *) die "--clock must be default, simplified or tsc" ;; esac
case "${SINK}" in simple|custom) ;; *) die "--sink must be simple or custom" ;; esac
for v in PAYLOAD COUNT WARMUP FLOOR_COUNT FLOOR_WARMUP PUB_CPU HOUSEKEEPING_CPU LOAD_WAIT_S SETUP_WAIT_SEC READER_READY_DELAY_MS; do
  [[ "${!v}" =~ ^[0-9]+$ ]] || die "${v} must be a non-negative integer, got '${!v}'"
done
[[ "${INTERVAL_US}" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "--interval-us must be a number"
[[ "${READER_CPUS}" =~ ^[0-9]+(,[0-9]+)*$ ]] || die "--reader-cpus must be a comma separated list"
if [ "${MODE}" = "e2e" ] && [ -z "${CHANNEL}" ]; then
  die "--channel is required for e2e mode"
fi
[ -x "${BIN}" ] || die "profiler binary not found: ${BIN} (build it with ninja -C builddir_release test/ipc_latency_profiler/ipc_latency_profiler)"

IFS=',' read -r -a ALL_READER_CPUS <<< "${READER_CPUS}"
[ "${#ALL_READER_CPUS[@]}" -ge "${READERS}" ] || die "--reader-cpus lists ${#ALL_READER_CPUS[@]} cores but --readers is ${READERS}"
USED_READER_CPUS=("${ALL_READER_CPUS[@]:0:${READERS}}")
USED_READER_CPUS_CSV="$(IFS=','; echo "${USED_READER_CPUS[*]}")"

NCPU="$(nproc --all)"
for c in "${PUB_CPU}" "${USED_READER_CPUS[@]}" "${HOUSEKEEPING_CPU}"; do
  [ "${c}" -lt "${NCPU}" ] || die "cpu ${c} does not exist (nproc=${NCPU})"
done

# ------------------------------------------------------------------ lock ----
exec 9>>"${LOCK_FILE}" || die "cannot open lock file ${LOCK_FILE}"
if ! flock -n 9; then
  die "another profiling run holds ${LOCK_FILE}; refusing to run concurrently (retry when it finishes)"
fi

# -------------------------------------------------------------- run setup ---
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)_$$"
RUN_DIR="${WORKTREE_ROOT}/builddir_release/ipc_profile_runs/${RUN_ID}"
mkdir -p "${RUN_DIR}" || die "cannot create run dir ${RUN_DIR}"
mkdir -p "$(dirname "${RESULTS}")" || die "cannot create results dir"
if [ -n "${RAW_DIR}" ]; then
  mkdir -p "${RAW_DIR}" || die "cannot create raw dir ${RAW_DIR}"
fi
CHANNEL_RESOLVED="${CHANNEL//\{run_id\}/${RUN_ID}}"

GIT_BRANCH="$(git -C "${WORKTREE_ROOT}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
GIT_COMMIT="$(git -C "${WORKTREE_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
if [ -n "$(git -C "${WORKTREE_ROOT}" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
  GIT_COMMIT="${GIT_COMMIT}-dirty"
fi

echo "=== ipc latency profile ${RUN_ID}"
echo "worktree : ${WORKTREE_ROOT} (${GIT_BRANCH} @ ${GIT_COMMIT})"
echo "mode     : ${MODE}  readers=${READERS}  clock=${CLOCK}  sink=${SINK}  payload=${PAYLOAD}B  interval=${INTERVAL_US}us"
echo "counts   : warmup=${WARMUP} measured=${COUNT}  floor: warmup=${FLOOR_WARMUP} measured=${FLOOR_COUNT} (enabled=${RUN_FLOOR})"
echo "cores    : publisher=${PUB_CPU} readers=${USED_READER_CPUS_CSV} housekeeping=${HOUSEKEEPING_CPU}"
[ "${MODE}" = "e2e" ] && echo "channel  : ${CHANNEL_RESOLVED}"
[ -n "${SETUP_CMD}" ] && echo "setup    : ${SETUP_CMD} (cpu ${HOUSEKEEPING_CPU}, wait ${SETUP_WAIT_SEC}s)"
[ "${#EXTRA_ENV[@]}" -gt 0 ] && echo "env      : ${EXTRA_ENV[*]}"
echo "results  : ${RESULTS}"
echo "run dir  : ${RUN_DIR}"

# --------------------------------------------------------- system checks ----
FAIL_REASONS=()
WARNINGS=()

LOAD1="$(cut -d' ' -f1 /proc/loadavg)"
LOAD_WAIT_DEADLINE=$(( $(date +%s) + LOAD_WAIT_S ))
while awk -v l="${LOAD1}" -v t="${LOAD_THRESHOLD}" 'BEGIN{exit !(l >= t)}' && [ "$(date +%s)" -lt "${LOAD_WAIT_DEADLINE}" ]; do
  echo "load1    : ${LOAD1} >= ${LOAD_THRESHOLD}, waiting (up to ${LOAD_WAIT_S}s total)"
  sleep 10
  LOAD1="$(cut -d' ' -f1 /proc/loadavg)"
done
if awk -v l="${LOAD1}" -v t="${LOAD_THRESHOLD}" 'BEGIN{exit !(l >= t)}'; then
  FAIL_REASONS+=("loadavg ${LOAD1} >= ${LOAD_THRESHOLD}")
fi

# Target cores plus their SMT siblings, from sysfs.
declare -A CHECK_SET=()
for c in "${PUB_CPU}" "${USED_READER_CPUS[@]}" "${HOUSEKEEPING_CPU}"; do
  CHECK_SET["${c}"]=1
  sib_file="/sys/devices/system/cpu/cpu${c}/topology/thread_siblings_list"
  if [ -r "${sib_file}" ]; then
    for item in $(tr ',' ' ' < "${sib_file}"); do
      if [[ "${item}" == *-* ]]; then
        for s in $(seq "${item%-*}" "${item#*-}"); do CHECK_SET["${s}"]=1; done
      else
        CHECK_SET["${item}"]=1
      fi
    done
  fi
  # Box topology: sibling of N is N+28.
  if [ $((c + 28)) -lt "${NCPU}" ]; then CHECK_SET["$((c + 28))"]=1; fi
done
CHECK_CPUS="$(printf '%s\n' "${!CHECK_SET[@]}" | sort -n | tr '\n' ' ')"

read_stat() {
  # Prints "cpuN total idle" for every cpu line.
  awk '/^cpu[0-9]+ /{t=0; for(i=2;i<=NF;i++) t+=$i; print $1, t, $5+$6}' /proc/stat
}
STAT0="$(read_stat)"
sleep 1
STAT1="$(read_stat)"
IDLE_REPORT=""
MIN_IDLE=100
for c in ${CHECK_CPUS}; do
  t0=$(echo "${STAT0}" | awk -v k="cpu${c}" '$1==k{print $2}')
  i0=$(echo "${STAT0}" | awk -v k="cpu${c}" '$1==k{print $3}')
  t1=$(echo "${STAT1}" | awk -v k="cpu${c}" '$1==k{print $2}')
  i1=$(echo "${STAT1}" | awk -v k="cpu${c}" '$1==k{print $3}')
  idle=$(awk -v t0="${t0}" -v i0="${i0}" -v t1="${t1}" -v i1="${i1}" 'BEGIN{d=t1-t0; if (d<=0) {print 100} else {printf "%.1f", 100*(i1-i0)/d}}')
  IDLE_REPORT="${IDLE_REPORT}cpu${c}=${idle}% "
  if awk -v a="${idle}" -v b="${MIN_IDLE}" 'BEGIN{exit !(a < b)}'; then MIN_IDLE="${idle}"; fi
  if awk -v a="${idle}" -v b="${IDLE_THRESHOLD}" 'BEGIN{exit !(a < b)}'; then
    FAIL_REASONS+=("cpu${c} only ${idle}% idle (< ${IDLE_THRESHOLD}%)")
  fi
done
echo "load1    : ${LOAD1} (threshold ${LOAD_THRESHOLD})"
echo "idle     : ${IDLE_REPORT}"

BUILD_PROCS="$(ps -eo pid=,comm= | awk '$2=="ninja"||$2=="cc1plus"||$2=="cc1"||$2=="ld"||$2=="ld.bfd"||$2=="ld.gold"||$2=="collect2"{print $2"("$1")"}' | tr '\n' ' ')"
if [ -n "${BUILD_PROCS}" ]; then
  WARNINGS+=("build processes running: ${BUILD_PROCS}")
fi

NUMA_PREFIX=()
if command -v numactl >/dev/null 2>&1; then
  NUMA_PREFIX=(numactl --membind=0)
else
  WARNINGS+=("numactl not found; memory is not bound (first-touch on pinned cores still applies)")
fi

for w in ${WARNINGS[@]+"${WARNINGS[@]}"}; do
  echo "WARNING  : ${w}"
done

FORCED=0
FORCED_REASON=""
if [ "${#FAIL_REASONS[@]}" -gt 0 ]; then
  for r in "${FAIL_REASONS[@]}"; do
    echo "CHECK    : FAILED ${r}"
  done
  if [ "${FORCE}" -ne 1 ]; then
    echo "run_profile.sh: system checks failed, aborting (use --force to override; forced runs are recorded)" >&2
    rm -rf "${RUN_DIR}"
    exit 3
  fi
  FORCED=1
  FORCED_REASON="$(IFS='; '; echo "${FAIL_REASONS[*]}")"
  echo "CHECK    : FORCED despite failures"
else
  echo "CHECK    : ok"
fi

# ----------------------------------------------------- child management ----
CHILD_PIDS=()
SETUP_PID=""
stop_setup() {
  if [ -n "${SETUP_PID}" ]; then
    kill -TERM -- "-${SETUP_PID}" 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      kill -0 "${SETUP_PID}" 2>/dev/null || break
      sleep 0.2
    done
    kill -KILL -- "-${SETUP_PID}" 2>/dev/null
    wait "${SETUP_PID}" 2>/dev/null
    SETUP_PID=""
  fi
}
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  if [ "${#CHILD_PIDS[@]}" -gt 0 ]; then
    for p in "${CHILD_PIDS[@]}"; do kill -TERM "${p}" 2>/dev/null; done
    sleep 0.5
    for p in "${CHILD_PIDS[@]}"; do kill -KILL "${p}" 2>/dev/null; done
    wait 2>/dev/null
  fi
  stop_setup
  # Remove shared memory created for this run only (floor pairs and any
  # channel that used the {run_id} placeholder).
  rm -f /dev/shm/*"${RUN_ID}"* 2>/dev/null
  exit "${rc}"
}
trap cleanup EXIT
trap 'echo "run_profile.sh: interrupted" >&2; exit 130' INT TERM

COMMON_ARGS=(
  --payload "${PAYLOAD}" --interval-us "${INTERVAL_US}"
  --results "${RESULTS}"
  --pub-cpu "${PUB_CPU}" --reader-cpus "${USED_READER_CPUS_CSV}"
  --housekeeping-cpu "${HOUSEKEEPING_CPU}"
  --git-branch "${GIT_BRANCH}" --git-commit "${GIT_COMMIT}" --run-id "${RUN_ID}"
  --forced "${FORCED}" --forced-reason "${FORCED_REASON}"
)

# launch <logfile> <cpu> args...   -> sets LAUNCHED_PID
launch() {
  local log="$1" cpu="$2"
  shift 2
  # TM_PUB_EXTRA_CPU: give the publisher process one more core so transport
  # background threads are not starved by the busy-wait pacing thread.
  local cpu_list="${cpu}"
  if [ -n "${TM_PUB_EXTRA_CPU:-}" ] && [ "${cpu}" = "${PUB_CPU}" ]; then
    cpu_list="${cpu},${TM_PUB_EXTRA_CPU}"
  fi
  env ${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"} ${NUMA_PREFIX[@]+"${NUMA_PREFIX[@]}"} taskset -c "${cpu_list}" "${BIN}" --cpu "${cpu}" "$@" > "${log}" 2>&1 &
  LAUNCHED_PID=$!
  CHILD_PIDS+=("${LAUNCHED_PID}")
}

# wait_ready <deadline_epoch> files...
wait_ready() {
  local deadline="$1"
  shift
  local f
  while true; do
    local all=1
    for f in "$@"; do
      [ -f "${f}" ] || all=0
    done
    [ "${all}" -eq 1 ] && return 0
    for p in "${CHILD_PIDS[@]}"; do
      if ! kill -0 "${p}" 2>/dev/null; then
        echo "run_profile.sh: a reader exited before signalling ready" >&2
        return 1
      fi
    done
    [ "$(date +%s)" -lt "${deadline}" ] || { echo "run_profile.sh: readers not ready in time" >&2; return 1; }
    sleep 0.05
  done
}

# wait_all <deadline_epoch>: waits for CHILD_PIDS, returns non-zero if any
# failed or the deadline passed (children are then killed by cleanup).
wait_all() {
  local deadline="$1"
  local rc=0
  local p
  while true; do
    local alive=0
    for p in "${CHILD_PIDS[@]}"; do
      kill -0 "${p}" 2>/dev/null && alive=1
    done
    [ "${alive}" -eq 0 ] && break
    if [ "$(date +%s)" -ge "${deadline}" ]; then
      echo "run_profile.sh: timeout, killing children" >&2
      for p in "${CHILD_PIDS[@]}"; do kill -KILL "${p}" 2>/dev/null; done
      rc=124
      break
    fi
    sleep 0.2
  done
  for p in "${CHILD_PIDS[@]}"; do
    wait "${p}" 2>/dev/null
    local prc=$?
    if [ "${prc}" -ne 0 ] && [ "${rc}" -eq 0 ]; then rc="${prc}"; fi
  done
  CHILD_PIDS=()
  return "${rc}"
}

start_setup() {
  [ -n "${SETUP_CMD}" ] || return 0
  echo "--- setup: ${SETUP_CMD}"
  setsid env ${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"} taskset -c "${HOUSEKEEPING_CPU}" bash -c "${SETUP_CMD}" > "${RUN_DIR}/setup.log" 2>&1 &
  SETUP_PID=$!
  sleep "${SETUP_WAIT_SEC}"
  if ! kill -0 "${SETUP_PID}" 2>/dev/null; then
    echo "run_profile.sh: setup command exited during the ${SETUP_WAIT_SEC}s wait" >&2
    cat "${RUN_DIR}/setup.log" >&2
    SETUP_PID=""
    return 1
  fi
  return 0
}

phase_timeout() {
  # 3x the nominal send time plus fixed slack for calibration and shutdown.
  local msgs="$1" phases="$2"
  if [ -n "${TIMEOUT_S}" ]; then echo "${TIMEOUT_S}"; return; fi
  awk -v m="${msgs}" -v p="${phases}" -v i="${INTERVAL_US}" 'BEGIN{printf "%d", 3*m*p*i/1e6 + 60}'
}

OVERALL_RC=0
FLOOR_FILE="${RUN_DIR}/floor.jsonl"

run_floor() {
  local idx=0
  for rc_cpu in "${USED_READER_CPUS[@]}"; do
    local shm="ipcprof_floor_${RUN_ID}_${idx}"
    local ready="${RUN_DIR}/floor_ready_${idx}"
    local to
    to="$(phase_timeout $((FLOOR_WARMUP + FLOOR_COUNT)) 2)"
    echo "--- floor pair cpu${PUB_CPU} <-> cpu${rc_cpu} (timeout ${to}s)"
    launch "${RUN_DIR}/floor_sub_${idx}.log" "${rc_cpu}" --mode floor --role sub --floor-shm "${shm}" \
      --ready-file "${ready}" --warmup "${FLOOR_WARMUP}" --count "${FLOOR_COUNT}" \
      --n-readers 1 --reader-index "${idx}" "${COMMON_ARGS[@]}" --reader-cpus "${rc_cpu}"
    local deadline=$(( $(date +%s) + to ))
    if ! wait_ready "${deadline}" "${ready}"; then
      wait_all "$(date +%s)"
      cat "${RUN_DIR}/floor_sub_${idx}.log" >&2
      return 1
    fi
    launch "${RUN_DIR}/floor_pub_${idx}.log" "${PUB_CPU}" --mode floor --role pub --floor-shm "${shm}" \
      --floor-file "${FLOOR_FILE}" --warmup "${FLOOR_WARMUP}" --count "${FLOOR_COUNT}" \
      --n-readers 1 --reader-index "${idx}" "${COMMON_ARGS[@]}" --reader-cpus "${rc_cpu}"
    wait_all "${deadline}"
    local rc=$?
    cat "${RUN_DIR}/floor_pub_${idx}.log"
    if [ "${rc}" -ne 0 ]; then
      echo "run_profile.sh: floor pair ${idx} failed (rc=${rc})" >&2
      cat "${RUN_DIR}/floor_sub_${idx}.log" >&2
      return 1
    fi
    idx=$((idx + 1))
  done
  return 0
}

run_e2e() {
  local to
  to="$(phase_timeout $((WARMUP + COUNT)) 1)"
  echo "--- e2e (${READERS} reader(s), timeout ${to}s)"
  start_setup || return 1
  local deadline=$(( $(date +%s) + to ))
  local readies=()
  local idx=0
  for rc_cpu in "${USED_READER_CPUS[@]}"; do
    local ready="${RUN_DIR}/e2e_ready_${idx}"
    readies+=("${ready}")
    local raw_args=()
    if [ -n "${RAW_DIR}" ]; then
      raw_args=(--raw "${RAW_DIR}/${RUN_ID}_reader${idx}.bin")
    fi
    launch "${RUN_DIR}/e2e_sub_${idx}.log" "${rc_cpu}" --mode e2e --role sub \
      --channel "${CHANNEL_RESOLVED}" --topic "${TOPIC}" --clock "${CLOCK}" --sink "${SINK}" \
      --warmup "${WARMUP}" --count "${COUNT}" --ready-file "${ready}" --floor-file "${FLOOR_FILE}" \
      --reader-ready-delay-ms "${READER_READY_DELAY_MS}" \
      --n-readers "${READERS}" --reader-index "${idx}" ${raw_args[@]+"${raw_args[@]}"} "${COMMON_ARGS[@]}"
    idx=$((idx + 1))
  done
  if ! wait_ready "${deadline}" "${readies[@]}"; then
    wait_all "$(date +%s)"
    for ((ii = 0; ii < READERS; ++ii)); do cat "${RUN_DIR}/e2e_sub_${ii}.log" >&2; done
    return 1
  fi
  launch "${RUN_DIR}/e2e_pub.log" "${PUB_CPU}" --mode e2e --role pub \
    --channel "${CHANNEL_RESOLVED}" --topic "${TOPIC}" --clock "${CLOCK}" --sink "${SINK}" \
    --warmup "${WARMUP}" --count "${COUNT}" --n-readers "${READERS}" "${COMMON_ARGS[@]}"
  wait_all "${deadline}"
  local rc=$?
  stop_setup
  cat "${RUN_DIR}/e2e_pub.log"
  for ((ii = 0; ii < READERS; ++ii)); do cat "${RUN_DIR}/e2e_sub_${ii}.log"; done
  if [ "${rc}" -ne 0 ]; then
    echo "run_profile.sh: e2e failed (rc=${rc})" >&2
  fi
  return "${rc}"
}

if [ "${MODE}" = "floor" ] || [ "${RUN_FLOOR}" -eq 1 ]; then
  run_floor || OVERALL_RC=1
fi
if [ "${MODE}" = "e2e" ] && [ "${OVERALL_RC}" -eq 0 ]; then
  run_e2e || OVERALL_RC=1
fi

json_esc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
SETUP_JSON="$(json_esc "${SETUP_CMD}")"
ENV_JSON="$(json_esc "${EXTRA_ENV[*]+"${EXTRA_ENV[*]}"}")"
WARN_JSON="$(printf '%s' "${WARNINGS[*]+"${WARNINGS[*]}"}" | sed 's/\\/\\\\/g; s/"/\\"/g')"
FORCED_JSON="$(printf '%s' "${FORCED_REASON}" | sed 's/\\/\\\\/g; s/"/\\"/g')"
CHANNEL_JSON="$(printf '%s' "${CHANNEL_RESOLVED}" | sed 's/\\/\\\\/g; s/"/\\"/g')"
printf '{"record_type":"run_meta","run_id":"%s","timestamp":"%s","git":{"branch":"%s","commit":"%s"},"mode":"%s","channel":"%s","n_readers":%s,"variant":{"clock":"%s","sink":"%s"},"payload_bytes":%s,"interval_us":%s,"cores":{"publisher":%s,"readers":[%s],"housekeeping":%s},"checks":{"load1":%s,"load_threshold":%s,"min_idle_pct":%s,"idle_threshold":%s,"idle":"%s"},"warnings":"%s","forced":%s,"forced_reason":"%s","numactl":%s,"floor_enabled":%s,"setup_cmd":"%s","extra_env":"%s","reader_ready_delay_ms":%s,"rc":%s}\n' \
  "${RUN_ID}" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${GIT_BRANCH}" "${GIT_COMMIT}" "${MODE}" "${CHANNEL_JSON}" "${READERS}" \
  "${CLOCK}" "${SINK}" "${PAYLOAD}" "${INTERVAL_US}" "${PUB_CPU}" "${USED_READER_CPUS_CSV}" "${HOUSEKEEPING_CPU}" \
  "${LOAD1}" "${LOAD_THRESHOLD}" "${MIN_IDLE}" "${IDLE_THRESHOLD}" "${IDLE_REPORT% }" "${WARN_JSON}" \
  "$([ "${FORCED}" -eq 1 ] && echo true || echo false)" "${FORCED_JSON}" \
  "$([ "${#NUMA_PREFIX[@]}" -gt 0 ] && echo true || echo false)" "${RUN_FLOOR}" "${SETUP_JSON}" "${ENV_JSON}" \
  "${READER_READY_DELAY_MS}" "${OVERALL_RC}" >> "${RESULTS}"

echo "=== done ${RUN_ID} rc=${OVERALL_RC} (logs in ${RUN_DIR})"
exit "${OVERALL_RC}"
