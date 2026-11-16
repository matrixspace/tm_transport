#!/usr/bin/env python3
"""Leader entry point; preserves every suite's exit code and raw results.

A FAIL in resource churn remains FAIL. Nightly/performance/24h runs are explicit;
this runner does not turn their absence into a PASS.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('build_dir', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--matrix', action='store_true')
    p.add_argument('--baseline', action='store_true', help='also run existing three-layout shm and IPC concurrency suites')
    p.add_argument('--only', action='append', choices=('core', 'graph', 'heartbeat', 'resource', 'baseline'))
    p.add_argument('--seed', type=int, default=20261113)
    args = p.parse_args()
    if args.out.exists(): p.error('output directory must not already exist')
    args.out.mkdir(parents=True)
    root = Path(__file__).resolve().parents[2]
    build = args.build_dir.resolve()
    python = sys.executable
    wanted = set(args.only or ('core', 'graph', 'heartbeat', 'resource'))
    if args.baseline: wanted.add('baseline')
    commands = []
    if 'core' in wanted:
        cmd = [python, str(root/'test/shm_seqlock_system_test/run_tests.py'), '--binary', str(build/'shm_seqlock_system_worker'), '--seed', str(args.seed), '--log-dir', str(args.out/'core')]
        if args.matrix: cmd.append('--matrix')
        commands.append(('core', cmd, 1800))
    if 'graph' in wanted:
        cmd = [python, str(root/'test/shm_seqlock_graph_system_test/run_tests.py'), str(build/'shm_seqlock_graph_system_test'), '--seed', str(args.seed), '--output', str(args.out/'graph')]
        if args.matrix: cmd.append('--matrix')
        commands.append(('graph', cmd, 1800))
    if 'heartbeat' in wanted:
        commands.append(('heartbeat', [python, str(root/'test/shm_seqlock_heartbeat_system_test/run_tests.py'), str(build/'shm_seqlock_heartbeat_system_worker'), '--output', str(args.out/'heartbeat')], 1800))
    if 'resource' in wanted:
        commands.append(('resource', [python, str(root/'test/shm_seqlock_resource_system_test/run_tests.py'), str(build/'shm_seqlock_resource_system_test'), '--profile=smoke', '--out='+str(args.out/'resource')], 300))
    if 'baseline' in wanted:
        for layout in ('inline', 'split', 'inline-premark'):
            commands.append(('baseline-'+layout, ['bash', str(root/'test/shm_seqlock_test/run_tests.sh'), str(build/'shm_seqlock_test'), str(args.out/('baseline-'+layout)), layout], 900))
        commands.append(('baseline-concurrency', ['bash', str(root/'test/ipc_concurrency_test/run_tests.sh'), str(build/'ipc_concurrency_test'), 'shm_seqlock'], 900))
    manifest = dict(argv=sys.argv, uname=list(os.uname()), seed=args.seed, affinity=sorted(os.sched_getaffinity(0)), binaries={})
    for file in build.iterdir():
        if file.is_file() and os.access(file, os.X_OK):
            manifest['binaries'][file.name] = hashlib.sha256(file.read_bytes()).hexdigest()
    (args.out/'environment.json').write_text(json.dumps(manifest, indent=2))
    results = []
    for name, command, timeout in commands:
        began = time.monotonic()
        with (args.out/(name+'.log')).open('w') as stream:
            env = dict(os.environ, LOGDIR=str(args.out/name))
            child = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, env=env, start_new_session=True)
            expired = False
            try:
                code = child.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                expired = True
                os.killpg(child.pid, signal.SIGKILL)
                code = child.wait()
        result = dict(name=name, command=command, exit_code=code, timeout=expired,
                      elapsed_seconds=time.monotonic()-began, status='PASS' if code == 0 and not expired else 'FAIL')
        results.append(result)
        (args.out/'summary.json').write_text(json.dumps(results, indent=2))
        print(f"{result['status']} {name} exit={code} log={args.out/(name+'.log')}", flush=True)
    return int(any(r['status'] != 'PASS' for r in results))


if __name__ == '__main__':
    raise SystemExit(main())
