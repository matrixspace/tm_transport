#!/usr/bin/env bash
set -u
B=${1:?usage: run_tests.sh <multitransport_lifecycle_test>}
ROOT=/tmp/tm-mt-life-$$; mkdir -p "$ROOT";
children=""
cleanup() {
  for pid in $children; do kill "$pid" 2>/dev/null || true; done
  rm -rf "$ROOT"
}
trap cleanup EXIT
fail=0
run_one() {
  local name=$1 channel=$2; local a="$ROOT/${name}_a"; local b="$ROOT/${name}_b"; local p="$ROOT/${name}_p"; local p2="$ROOT/${name}_p2"
  "$B" sub "$channel" "$a" 2048 0 >"$ROOT/${name}_a.log" 2>&1 & local ap=$!; children="$children $ap"
  for _ in $(seq 1 100); do [ -f "$a.ready" ] && break; sleep .05; done
  "$B" pub "$channel" "$a" 1024 >"$ROOT/${name}_p.log" 2>&1 & local pp=$!; children="$children $pp"
  wait "$pp" || true
  kill "$ap" 2>/dev/null || true; wait "$ap" 2>/dev/null || true
  "$B" sub "$channel" "$b" 1024 0 >"$ROOT/${name}_b.log" 2>&1 & local bp=$!; children="$children $bp"
  for _ in $(seq 1 100); do [ -f "$b.ready" ] && break; sleep .05; done
  "$B" pub "$channel" "$b" 1024 >"$ROOT/${name}_p2.log" 2>&1 & local pp2=$!; children="$children $pp2"
  wait "$pp2" || true; wait "$bp" || true
  grep -q 'bad=0' "$ROOT/${name}_b" || { echo "FAIL $name: ${name}_b"; cat "$ROOT/${name}_b"; fail=1; }
  echo "PASS $name reader leave/join and publisher restart"
}
run_one seqlock "shm_seqlock://::::mt-life-$$_s[slotSize=8192,slotCount=4096]"
run_one cursor "shm_cursor://::::mt-life-$$_c[slotSize=8192,slotCount=4096]"
run_one zeromq "zeromq://127.0.0.1:$((${TM_MT_ZMQ_PORT:-52000}+$$%1000))"
exit "$fail"
