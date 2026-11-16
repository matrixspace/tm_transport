#!/usr/bin/env python3
"""Run the graph/MultiTransport performance benchmark.

Each config is the same graph shape (trigger -> publisher -> shared memory ->
importer/listener -> sink), varying:
  raw   : ShmSeqlockImporterExporter directly
  multi : MultiTransport publisher and listener
  listeners: 1 or 4
  wait: spin/futex at 10k/s, plus spin at rate=0 (saturation)

5 rounds x (5 s warmup + 30 s measurement) per config, randomized order.
"""
import argparse
import hashlib
import json
import os
import random
import subprocess
import sys
import time
import uuid
from pathlib import Path


def configs():
    out = []
    for mode in ('raw', 'multi'):
        for listeners in (1, 4):
            for wait, rate in (('spin', 10000), ('futex', 10000), ('spin', 0)):
                out.append(dict(mode=mode, listeners=listeners, wait=wait, rate=rate,
                                payload_bytes=256))
    return out


def label(cfg):
    return f"{cfg['mode']}-l{cfg['listeners']}-{cfg['wait']}-rate{cfg['rate']}"


def run_one(binary, cfg, out_dir, duration_ms, warmup_ms, seed):
    segment = 'shmbench_' + uuid.uuid4().hex
    command = [str(binary), cfg['mode'], segment, str(cfg['listeners']), str(cfg['rate']),
               str(cfg['payload_bytes']), str(duration_ms), str(warmup_ms),
               'inline', 'classic', cfg['wait']]
    started = time.monotonic()
    completed = subprocess.run(command, capture_output=True, text=True,
                               timeout=duration_ms / 1000 + 60)
    elapsed = time.monotonic() - started
    for suffix in ('', '.lock'):
        try:
            Path('/dev/shm', segment + suffix).unlink()
        except FileNotFoundError:
            pass
    record = {'config': label(cfg), 'command': command, 'exit_code': completed.returncode,
              'elapsed_seconds': elapsed}
    if completed.returncode != 0:
        record.update(status='FAIL', error=completed.stderr.strip()[-800:])
        return record
    rows = [line for line in completed.stdout.splitlines() if line.startswith('{')]
    if not rows:
        record.update(status='FAIL', error='no JSON result: ' + completed.stdout[-500:])
        return record
    try:
        result = json.loads(rows[-1])
    except json.JSONDecodeError as exc:
        record.update(status='FAIL', error=f'bad JSON: {exc}')
        return record
    if result.get('kind') != 'graph-bench':
        record.update(status='FAIL', error='unexpected result kind')
        return record
    record['result'] = result
    record['status'] = 'PASS'
    return record


def mean(values):
    return sum(values) / len(values) if values else 0


def aggregate(cfg, rounds, duration_ms):
    ok = [r for r in rounds if r.get('status') == 'PASS']
    measured_seconds = duration_ms / 1000.0
    entry = {'mode': cfg['mode'], 'listeners': cfg['listeners'], 'wait': cfg['wait'],
             'rate': cfg['rate'], 'payload_bytes': cfg['payload_bytes'],
             'requested_rounds': len(rounds), 'passed_rounds': len(ok)}
    if not ok:
        return entry
    offered = [r['result']['offered'] for r in ok]
    entry['offered_per_second_avg'] = mean(offered) / measured_seconds
    entry['offered_per_second_min'] = min(offered) / measured_seconds
    entry['offered_per_second_max'] = max(offered) / measured_seconds
    sinks = []
    for index in range(cfg['listeners']):
        rows = [r['result']['sinks'][index] for r in ok]
        sinks.append({
            'received_total': sum(r['received'] for r in rows),
            'lost_total': sum(r['lost'] for r in rows),
            'dup_total': sum(r['dup'] for r in rows),
            'ooo_total': sum(r['ooo'] for r in rows),
            'corrupt_total': sum(r['corrupt'] for r in rows),
            'bad_total': sum(r['bad'] for r in rows),
            'p50_ns_avg': mean([r['p50_ns'] for r in rows]),
            'p90_ns_avg': mean([r['p90_ns'] for r in rows]),
            'p99_ns_avg': mean([r['p99_ns'] for r in rows]),
            'p99_ns_max': max(r['p99_ns'] for r in rows),
            'max_ns_max': max(r['max_ns'] for r in rows),
        })
    entry['sink_metrics'] = sinks
    entry['lost_total'] = sum(s['lost_total'] for s in sinks)
    entry['reader_p99_ns_max'] = max(s['p99_ns_max'] for s in sinks)
    return entry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--repeat', type=int, default=5)
    parser.add_argument('--duration-ms', type=int, default=30000)
    parser.add_argument('--warmup-ms', type=int, default=5000)
    parser.add_argument('--seed', type=int, default=20261116)
    args = parser.parse_args()
    if args.out.exists():
        parser.error('output directory must not already exist')
    if not args.binary.is_file():
        parser.error('binary does not exist')
    args.out.mkdir(parents=True)
    binary = args.binary.resolve()
    try:
        commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
        dirty = subprocess.check_output(['git', 'status', '--short'], text=True).strip()
    except subprocess.SubprocessError:
        commit, dirty = 'unavailable', ''
    environment = {
        'uname': list(os.uname()),
        'cpu_model': next((line.split(':', 1)[1].strip() for line in open('/proc/cpuinfo')
                           if line.startswith('model name')), 'unknown'),
        'binary': str(binary),
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'command': sys.argv, 'commit': commit, 'dirty_status': dirty,
        'affinity': sorted(os.sched_getaffinity(0)),
        'duration_ms': args.duration_ms, 'warmup_ms': args.warmup_ms, 'repeat': args.repeat,
        'method': 'RealTimeApp graph trigger -> publisher -> shm_seqlock -> importer/listener '
                  '-> sinks; message rate paced by a sender thread; final stats are post-warmup.',
    }
    (args.out / 'environment.json').write_text(json.dumps(environment, indent=2))
    cfgs = configs()
    jobs = [(cfg, r) for cfg in cfgs for r in range(args.repeat)]
    random.Random(args.seed).shuffle(jobs)
    results = []
    started = time.time()
    for number, (cfg, round_index) in enumerate(jobs):
        try:
            record = run_one(binary, cfg, args.out, args.duration_ms, args.warmup_ms,
                             args.seed + number * 13)
        except subprocess.TimeoutExpired:
            record = {'config': label(cfg), 'status': 'FAIL', 'error': 'timeout'}
        record['round'] = round_index
        results.append(record)
        print(f"{record['status']} {label(cfg)} round={round_index} "
              f"offered/s={record.get('result', {}).get('offered', 0) / (args.duration_ms / 1000):,.0f}"
              if record['status'] == 'PASS' else
              f"FAIL {label(cfg)} round={round_index} {record.get('error', '')[:120]}",
              flush=True)
    summary = {
        'date': '2026-11-16',
        'scope': 'graph and MultiTransport performance for shm_seqlock',
        'environment': environment,
        'configs': {label(cfg): aggregate(cfg, [r for r in results if r['config'] == label(cfg)],
                                          args.duration_ms) for cfg in cfgs},
        'rounds': results,
        'duration_seconds': time.time() - started,
        'limits': [
            'Publisher and listeners run in one process (loopback graph); cross-process graph not measured.',
            'raw mode uses ShmSeqlockImporterExporter; multi uses MultiTransport publisher/listener '
            'with shm_seqlock channels.',
            'payload 256 bytes; layouts inline, read classic; wait spin/futex; 1 or 4 sinks.',
            'Rate=0 is saturation; sinks may lose messages there, which is reported separately.',
            'Percentiles are per-round histogram quantiles averaged over rounds (<=1.6% bucket error).',
            'The host is shared; affinity is not pinned for this run.',
        ],
    }
    with open(args.out / 'summary.json', 'w') as f:
        json.dump(summary, f, indent=2)
        f.write('\n')
    failed = sum(1 for r in results if r['status'] != 'PASS')
    print(json.dumps({'rounds': len(results), 'failed_rounds': failed,
                      'output': str(args.out)}))
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
