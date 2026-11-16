#!/usr/bin/env python3
"""Resource/performance runner; raw evidence survives failures and watchdog kills.

smoke is a short implementation check, never a performance baseline.
full uses 5 s warmup, 30 s measurements, 5 shuffled repeats by default.
soak supports --hours=2 or --hours=24; no soak is started implicitly.
"""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import random
import signal
import subprocess
import time


def cpu_environment():
    values = {}
    for path in ("/proc/cpuinfo", "/sys/devices/system/cpu/online",
                 "/sys/devices/system/node/online",
                 "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                 "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"):
        try:
            values[path] = Path(path).read_text()
        except OSError as exc:
            values[path] = {"unavailable": str(exc)}
    values["affinity"] = sorted(os.sched_getaffinity(0))
    return values


def diagnostics(pid, destination):
    snapshot = {}
    for name in ("status", "stat", "maps", "smaps_rollup", "wchan", "stack"):
        try:
            snapshot[name] = Path(f"/proc/{pid}/{name}").read_text()
        except OSError as exc:
            snapshot[name] = {"unavailable": str(exc)}
    threads = {}
    try:
        for task in Path(f"/proc/{pid}/task").iterdir():
            threads[task.name] = {}
            for name in ("status", "wchan", "stack"):
                try:
                    threads[task.name][name] = (task / name).read_text()
                except OSError as exc:
                    threads[task.name][name] = {"unavailable": str(exc)}
    except OSError as exc:
        snapshot["thread_error"] = str(exc)
    snapshot["threads"] = threads
    destination.write_text(json.dumps(snapshot, indent=2))


def run_one(binary, out, number, options, timeout, seed):
    segment = f"shmsys_resource_{os.getpid()}_{time.monotonic_ns()}_{number}"
    command = [str(binary), f"--segment={segment}"] + [f"--{k}={v}" for k, v in options.items()]
    prefix = out / f"{number:04d}"
    started = time.monotonic()
    timed_out = False
    with prefix.with_suffix(".stdout").open("w") as stdout, prefix.with_suffix(".stderr").open("w") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            diagnostics(process.pid, prefix.with_suffix(".watchdog.json"))
            os.killpg(process.pid, signal.SIGKILL)
            code = process.wait(timeout=10)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=10)
            # Only this registered segment, after its writer is confirmed dead.
            for suffix in ("", ".lock"):
                try:
                    Path("/dev/shm", segment + suffix).unlink()
                except FileNotFoundError:
                    pass
    rows = []
    parse_errors = []
    for line in prefix.with_suffix(".stdout").read_text().splitlines():
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            parse_errors.append(line)
    final = rows[-1] if rows else {}
    valid = code == 0 and not timed_out and not parse_errors and final.get("status") == "PASS"
    result = {"number": number, "command": command, "options": options, "seed": seed,
              "elapsed_seconds": time.monotonic() - started, "exit_code": code,
              "timeout": timed_out, "status": "PASS" if valid else "FAIL", "rows": rows,
              "parse_errors": parse_errors, "stdout": str(prefix.with_suffix(".stdout")),
              "stderr": str(prefix.with_suffix(".stderr"))}
    prefix.with_suffix(".json").write_text(json.dumps(result, indent=2))
    print(f"{result['status']} {number} {options}", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--profile", choices=("smoke", "full", "churn", "soak"), default="smoke")
    parser.add_argument("--out", type=Path, default=Path(f"/tmp/shm-resource-{os.getpid()}"))
    parser.add_argument("--seed", type=int, default=20261113)
    parser.add_argument("--readers", default="0,1,2,4,8")
    parser.add_argument("--payloads", default="64,256,1024,4096")
    parser.add_argument("--layouts", default="inline,split,inline-premark")
    parser.add_argument("--reads", default="classic,spec")
    parser.add_argument("--waits", default="spin,futex")
    parser.add_argument("--rate", type=int, default=10000, help="offered messages/s; 0=saturation")
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--cycles", type=int, default=10000)
    parser.add_argument("--hours", type=float, default=2)
    parser.add_argument("--max-rounds", type=int, default=0, help="explicitly shortened matrix; never a full baseline")
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    if not args.binary.is_file():
        parser.error("test binary does not exist")
    if args.repeat < 1 or args.cycles < 1 or not 0 < args.hours <= 24 or args.rate < 0 or args.max_rounds < 0:
        parser.error("invalid repeat/cycles/hours/rate/max-rounds")
    args.out.mkdir(parents=True, exist_ok=False)
    env_process = subprocess.run([str(args.binary), "--mode=environment"], capture_output=True, text=True, timeout=10, check=True)
    binary_env = json.loads(env_process.stdout)
    if args.profile in ("full", "soak") and binary_env.get("build_type") != "release":
        parser.error("full/soak requires a release binary")
    try:
        commit = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, timeout=10, check=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--short"], capture_output=True, text=True, timeout=10, check=True).stdout
    except subprocess.SubprocessError as exc:
        commit, dirty = "unavailable", str(exc)
    environment = {"uname": list(platform.uname()), "cpu": cpu_environment(), "binary": str(args.binary),
                   "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(), "binary_environment": binary_env,
                   "commit": commit, "dirty_status": dirty, "seed": args.seed, "profile": args.profile,
                   "requested_arguments": vars(args) | {"binary": str(args.binary), "out": str(args.out)}}
    (args.out / "environment.json").write_text(json.dumps(environment, indent=2))
    rounds = [(dict(mode="oracle"), 30)]
    if args.profile == "smoke":
        for layout, read, wait in itertools.product(args.layouts.split(","), args.reads.split(","), args.waits.split(",")):
            rounds.append((dict(mode="perf", readers=1, payload=256, layout=layout, read=read, wait=wait,
                                rate=args.rate, warmupMs=100, durationMs=300, verify="true"), 15))
        for readers in (0, 2, 4, 8):
            rounds.append((dict(mode="perf", readers=readers, payload=64, wait="futex", rate=args.rate,
                                warmupMs=50, durationMs=100), 15))
        rounds.extend((dict(mode="churn", cycles=20, wait="futex", selfRemove=v), 30) for v in ("false", "true"))
    elif args.profile == "churn":
        rounds.extend((dict(mode="churn", cycles=args.cycles, wait="futex", selfRemove=v), max(60, args.cycles / 20)) for v in ("false", "true"))
    elif args.profile == "full":
        matrix = []
        for repeat, readers, payload, layout, read, wait in itertools.product(range(args.repeat), args.readers.split(","), args.payloads.split(","), args.layouts.split(","), args.reads.split(","), args.waits.split(",")):
            matrix.append((dict(mode="perf", readers=int(readers), payload=int(payload), layout=layout, read=read,
                                wait=wait, rate=args.rate, warmupMs=5000, durationMs=30000), 60))
        random.Random(args.seed).shuffle(matrix)
        rounds += matrix[:args.max_rounds] if args.max_rounds else matrix
    else:
        # One persistent writer/component set throughout the entire long run.
        # Progress resource snapshots are emitted each minute without restarting.
        rounds.append((dict(mode="perf", readers=int(args.readers.split(",")[-1]),
                            payload=int(args.payloads.split(",")[-1]), layout=args.layouts.split(",")[-1],
                            read=args.reads.split(",")[-1], wait=args.waits.split(",")[-1], rate=args.rate,
                            warmupMs=5000, durationMs=int(args.hours*3600000), verify="true", progress="true"),
                       args.hours*3600+30))
    results = [run_one(args.binary, args.out, number, options, timeout, args.seed)
               for number, (options, timeout) in enumerate(rounds)]
    def same(value, expected):
        return [x.strip() for x in value.split(",")] == expected.split(",")
    full_dimensions = (same(args.readers, "0,1,2,4,8") and same(args.payloads, "64,256,1024,4096")
                       and same(args.layouts, "inline,split,inline-premark") and same(args.reads, "classic,spec")
                       and same(args.waits, "spin,futex") and args.rate == 10000)
    return summarize(args.out, args.profile, results, bool(args.max_rounds), args.repeat, full_dimensions)


def summarize(out, profile, results, shortened, repeat, full_dimensions):
    failed = [r["number"] for r in results if r["status"] != "PASS"]
    summary = {"profile": profile, "shortened": shortened, "full_dimensions": full_dimensions,
               "pass": len(results)-len(failed),
               "fail": len(failed), "failed_rounds": failed, "results": results,
               "performance_baseline": profile == "full" and repeat >= 5 and not shortened and not failed and full_dimensions}
    (out / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps({k: v for k, v in summary.items() if k != "results"}), flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
