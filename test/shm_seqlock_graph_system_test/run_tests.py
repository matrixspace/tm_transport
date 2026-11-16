#!/usr/bin/env python3
"""Per-case watchdog and exact-resource cleanup for the graph system suite."""
import argparse
import itertools
import json
import os
from pathlib import Path
import random
import signal
import subprocess
import time
import uuid

CASES = ("raw_graph", "typed_graph", "typed_explicit_hooks", "multitransport",
         "concurrent", "threaded_exporter", "shared_exporter", "control",
         "slow_sync", "slow_async", "fetch_raw", "fetch_typed", "multi_fetch_raw", "multi_fetch_typed", "synchronous", "dynamic_listener")


def diagnostics(pid, path):
    report = {}
    for name in ("status", "wchan", "maps"):
        try:
            report[name] = Path(f"/proc/{pid}/{name}").read_text()
        except OSError as exc:
            report[name] = str(exc)
    report["threads"] = {}
    for task in Path(f"/proc/{pid}/task").glob("*"):
        report["threads"][task.name] = {}
        for name in ("stack", "wchan", "status"):
            try:
                report["threads"][task.name][name] = (task / name).read_text()
            except OSError as exc:
                report["threads"][task.name][name] = str(exc)
    path.write_text(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--layout", choices=("inline", "split", "inline-premark"), default="inline")
    parser.add_argument("--read", choices=("classic", "spec"), default="classic")
    parser.add_argument("--wait", choices=("spin", "futex"), default="futex")
    parser.add_argument("--matrix", action="store_true", help="all 3 layouts x 2 readers x 2 matching wait modes")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--seed", type=int, default=20261113)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--output", type=Path, default=Path("/tmp/shm-graph-system"))
    args = parser.parse_args()
    if args.repeat < 1 or args.timeout <= 0:
        parser.error("repeat and timeout must be positive")
    binary = args.binary.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    matrix = list(itertools.product(("inline", "split", "inline-premark"), ("classic", "spec"), ("spin", "futex"))) if args.matrix else [(args.layout, args.read, args.wait)]
    specs = list(itertools.product(args.case or CASES, matrix, range(args.repeat)))
    random.Random(args.seed).shuffle(specs)
    results = []
    for idx, (case, (layout, read, wait), repeat) in enumerate(specs):
        segment = f"tm_graph_{os.getpid()}_{uuid.uuid4().hex[:12]}"
        command = [str(binary), case, segment, layout, read, wait]
        log = args.output / f"{idx:04d}_{case}_{layout}_{read}_{wait}.log"
        began = time.monotonic()
        timed_out = False
        with log.open("w") as stream:
            proc = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                rc = proc.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                diagnostics(proc.pid, log.with_suffix(".diagnostics.json"))
                os.killpg(proc.pid, signal.SIGKILL)
                rc = proc.wait()
            finally:
                # Only this case's segment and lock file, after its process and
                # all process-group members have exited, are eligible for unlink.
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                for suffix in ("", ".lock"):
                    Path("/dev/shm", segment + suffix).unlink(missing_ok=True)
        status = "PASS" if rc == 0 and not timed_out and "RESULT status=PASS" in log.read_text() else "FAIL"
        result = dict(case=case, layout=layout, read=read, wait=wait, repeat=repeat,
                      seed=args.seed, command=command, status=status, exit_code=rc,
                      timeout=timed_out, seconds=time.monotonic()-began, log=str(log))
        results.append(result)
        print(f"{status} {case} {layout}/{read}/{wait} rc={rc}", flush=True)
        (args.output / "results.json").write_text(json.dumps(dict(seed=args.seed, results=results), indent=2)+"\n")
    passed = sum(r["status"] == "PASS" for r in results)
    print(f"SUMMARY passed={passed} failed={len(results)-passed} output={args.output}", flush=True)
    return int(passed != len(results))


if __name__ == "__main__":
    raise SystemExit(main())
