#!/usr/bin/env python3
"""Cross-NUMA benchmark for shm_seqlock using the stability worker.

Each round starts one writer process and N reader processes pinned to selected
physical cores, runs 35 s (5 s warmup + 30 s measured) and collects the
worker's final STAT line. Placements:

  same0  : writer and readers on NUMA0
  cross01: writer on NUMA0, readers on NUMA1 (segment pages live on NUMA0)
  same1  : writer and readers on NUMA1
  cross10: writer on NUMA1, readers on NUMA0

Physical cores on this host: NUMA0 = 12,14,...,26; NUMA1 = 13,15,...,27
(siblings are +28, so those lists use one thread per core).
"""
import argparse
import hashlib
import json
import os
import random
import shutil
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

NUMA0 = {'writer': '12', 'readers': ['14', '16', '18', '20', '22', '24', '26', '2']}
NUMA1 = {'writer': '13', 'readers': ['15', '17', '19', '21', '23', '25', '27', '3']}
PLACEMENTS = {
    'same0': (NUMA0, NUMA0),
    'cross01': (NUMA0, NUMA1),
    'same1': (NUMA1, NUMA1),
    'cross10': (NUMA1, NUMA0),
}


def worker_props(wait):
    return ('mlock=false,layout=inline,slotSize=512,slotCount=4096,wait=' + wait
            + ',read=classic,spinCount=1,pauseAfter=1,join=latest')


class Process:
    def __init__(self, command, log):
        self.lines = []
        self.ready = threading.Event()
        self.log = log
        self.proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=log,
                                     stdin=subprocess.DEVNULL, text=True, bufsize=1,
                                     start_new_session=True)
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self):
        for line in self.proc.stdout:
            line = line.strip()
            self.log.write(line + '\n')
            if line == 'READY':
                self.ready.set()
            if line.startswith('STAT '):
                self.lines.append(line)

    def wait_ready(self, timeout):
        return self.ready.wait(timeout)

    def finish(self, timeout):
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(self.proc.pid, 9)
            code = self.proc.wait()
        self.thread.join(timeout=3)
        return code

    def alive(self):
        return self.proc.poll() is None


def last_stat(lines, kind):
    found = None
    for line in lines:
        if line.startswith('STAT ' + kind):
            fields = {}
            for item in line.split()[1:]:
                key, _, value = item.partition('=')
                try:
                    fields[key] = int(value)
                except ValueError:
                    fields[key] = value
            if fields.get('final') == 1:
                found = fields
    return found


def run_round(binary, cfg, round_index, out_dir, duration_ms, warmup_ms, seed):
    segment = 'shmnuma_' + uuid.uuid4().hex
    writer_place, reader_place = PLACEMENTS[cfg['placement']]
    props = worker_props(cfg['wait'])
    log_path = out_dir / (f"{cfg['placement']}-r{cfg['readers']}-{cfg['wait']}-rate{cfg['rate']}-{round_index}.log")
    log = open(log_path, 'w', buffering=1)
    procs = []
    try:
        writer_cmd = ['taskset', '-c', writer_place['writer'], str(binary), 'writer', segment,
                      props, str(cfg['rate']), cfg['payload'], str(seed), '0',
                      str(duration_ms), str(warmup_ms)]
        log.write('$ ' + ' '.join(writer_cmd) + '\n')
        writer = Process(writer_cmd, log)
        procs.append(writer)
        if not writer.wait_ready(10):
            raise RuntimeError('writer not ready')
        readers = []
        for index in range(cfg['readers']):
            cpus = reader_place['readers'][index % len(reader_place['readers'])]
            cmd = ['taskset', '-c', cpus, str(binary), 'reader', segment, props,
                   str(cfg['rate']), cfg['payload'], str(seed + 1000 + index), '0',
                   str(duration_ms), str(warmup_ms)]
            log.write('$ ' + ' '.join(cmd) + '\n')
            reader = Process(cmd, log)
            procs.append(reader)
            readers.append(reader)
        for index, reader in enumerate(readers):
            if not reader.wait_ready(30):
                raise RuntimeError(f'reader-{index} not ready')
        codes = [p.finish(duration_ms / 1000 + 30) for p in procs]
        if any(code != 0 for code in codes):
            raise RuntimeError(f'non-zero exit codes {codes}')
        writer_stats = last_stat(writer.lines, 'W')
        reader_stats = [last_stat(reader.lines, 'R') for reader in readers]
        if writer_stats is None or any(stats is None for stats in reader_stats):
            raise RuntimeError('missing final STAT line')
        measured = (duration_ms - warmup_ms) / 1000.0
        return {
            'round': round_index,
            'offered': writer_stats['sent'],
            'offered_per_second': writer_stats['sent'] / measured,
            'late_max_us': writer_stats.get('lateMaxUs', 0),
            'writer_rejected': writer_stats.get('rejected', 0),
            'readers': reader_stats,
            'log': str(log_path),
            'status': 'PASS',
        }
    finally:
        for p in procs:
            if p.alive():
                try:
                    os.killpg(p.proc.pid, 9)
                except ProcessLookupError:
                    pass
                p.proc.wait()
        log.close()
        for suffix in ('', '.lock'):
            try:
                Path('/dev/shm', segment + suffix).unlink()
            except FileNotFoundError:
                pass


def mean(values):
    return sum(values) / len(values) if values else 0


def aggregate(cfg, rounds):
    ok = [r for r in rounds if r.get('status') == 'PASS']
    out = {
        'placement': cfg['placement'], 'readers': cfg['readers'], 'wait': cfg['wait'],
        'rate': cfg['rate'], 'payload': cfg['payload'], 'requested_rounds': len(rounds),
        'passed_rounds': len(ok),
        'writer_cpu': PLACEMENTS[cfg['placement']][0]['writer'],
        'reader_cpus': PLACEMENTS[cfg['placement']][1]['readers'][:cfg['readers']],
    }
    if not ok:
        return out
    out['offered_per_second_avg'] = mean([r['offered_per_second'] for r in ok])
    out['offered_per_second_min'] = min(r['offered_per_second'] for r in ok)
    out['offered_per_second_max'] = max(r['offered_per_second'] for r in ok)
    out['late_max_us_max'] = max(r['late_max_us'] for r in ok)
    out['writer_rejected_total'] = sum(r['writer_rejected'] for r in ok)
    metrics = []
    for index in range(cfg['readers']):
        rows = [r['readers'][index] for r in ok]
        metrics.append({
            'received_total': sum(r['received'] for r in rows),
            'lost_total': sum(r['lost'] for r in rows),
            'dup_total': sum(r['dup'] for r in rows),
            'ooo_total': sum(r['ooo'] for r in rows),
            'corrupt_total': sum(r['corrupt'] for r in rows),
            'bad_total': sum(r['bad'] for r in rows),
            'p50_ns_avg': mean([r['p50ns'] for r in rows]),
            'p90_ns_avg': mean([r['p90ns'] for r in rows]),
            'p99_ns_avg': mean([r['p99ns'] for r in rows]),
            'p99_ns_max': max(r['p99ns'] for r in rows),
            'max_ns_max': max(r['maxns'] for r in rows),
        })
    out['reader_metrics'] = metrics
    out['reader_p99_ns_max'] = max(m['p99_ns_max'] for m in metrics)
    out['reader_p50_ns_min'] = min(m['p50_ns_avg'] for m in metrics)
    out['lost_total'] = sum(m['lost_total'] for m in metrics)
    return out


def default_configs():
    configs = []
    for readers in (1, 8):
        for wait in ('spin', 'futex'):
            for placement_name in ('same0', 'cross01'):
                configs.append(dict(placement=placement_name, readers=readers, wait=wait,
                                    rate=10000, payload='fixed:256'))
    for wait in ('spin', 'futex'):
        for placement_name in ('same1', 'cross10'):
            configs.append(dict(placement=placement_name, readers=8, wait=wait,
                                rate=10000, payload='fixed:256'))
    for placement_name in ('same0', 'cross01', 'same1', 'cross10'):
        configs.append(dict(placement=placement_name, readers=8, wait='spin',
                            rate=0, payload='fixed:256'))
    return configs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--repeat', type=int, default=5)
    parser.add_argument('--duration-ms', type=int, default=35000)
    parser.add_argument('--warmup-ms', type=int, default=5000)
    parser.add_argument('--seed', type=int, default=20261116)
    args = parser.parse_args()
    if args.out.exists():
        parser.error('output directory must not already exist')
    if not args.binary.is_file():
        parser.error('binary does not exist')
    if shutil.which('taskset') is None:
        parser.error('taskset is required')
    if args.warmup_ms >= args.duration_ms:
        parser.error('warmup must be shorter than duration')
    args.out.mkdir(parents=True)
    binary = args.binary.resolve()
    configs = default_configs()
    jobs = [(cfg, r) for cfg in configs for r in range(args.repeat)]
    random.Random(args.seed).shuffle(jobs)
    try:
        commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
        dirty = subprocess.check_output(['git', 'status', '--short'], text=True).strip()
    except subprocess.SubprocessError:
        commit, dirty = 'unavailable', ''
    environment = {
        'uname': list(os.uname()),
        'cpu_model': next((line.split(':', 1)[1].strip() for line in open('/proc/cpuinfo')
                           if line.startswith('model name')), 'unknown'),
        'binary': str(binary), 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'numa_cpu_sets': {'NUMA0': NUMA0, 'NUMA1': NUMA1},
        'placements': {name: {'writer': w['writer'], 'readers': r['readers']}
                       for name, (w, r) in PLACEMENTS.items()},
        'command': sys.argv, 'commit': commit, 'dirty_status': dirty,
        'host_affinity': sorted(os.sched_getaffinity(0)),
        'duration_ms': args.duration_ms, 'warmup_ms': args.warmup_ms, 'repeat': args.repeat,
        'method': 'One writer process and N reader processes per round, each pinned with '
                  'taskset to one physical core; 5 s warmup then 30 s measured; the final '
                  'STAT line reports post-warmup counters and a <=1.6% log-linear latency histogram.',
    }
    (args.out / 'environment.json').write_text(json.dumps(environment, indent=2))
    results = []
    started = time.time()
    for number, (cfg, round_index) in enumerate(jobs):
        label = f"{cfg['placement']}-r{cfg['readers']}-{cfg['wait']}-rate{cfg['rate']}"
        try:
            record = run_round(binary, cfg, round_index, args.out, args.duration_ms, args.warmup_ms,
                               args.seed + number * 7)
        except Exception as exc:
            record = {'round': round_index, 'status': 'FAIL', 'error': str(exc)}
        record.update({'config': label, 'placement': cfg['placement'],
                       'readers_count': cfg['readers'], 'wait': cfg['wait'],
                       'rate': cfg['rate'], 'payload': cfg['payload']})
        results.append(record)
        print(f"{record['status']} {label} round={round_index} "
              f"offered/s={record.get('offered_per_second', 0):,.0f}", flush=True)
    by_config = {}
    for cfg in configs:
        label = f"{cfg['placement']}-r{cfg['readers']}-{cfg['wait']}-rate{cfg['rate']}"
        by_config[label] = aggregate(cfg, [r for r in results if r['config'] == label])
    payload = {
        'date': '2026-11-16',
        'scope': 'cross-NUMA performance: same-node controls and writer/reader split across NUMA0/NUMA1',
        'environment': environment,
        'configs': by_config,
        'rounds': results,
        'duration_seconds': time.time() - started,
        'limits': [
            'One writer and up to 8 readers, payload 256B, inline/classic, 10k/s and saturated 8-reader.',
            'Warmup is 5 s and measured window 30 s per round; finals are post-warmup deltas.',
            'Latency percentiles are per-round histogram quantiles (<=1.6% bucket error) averaged over rounds.',
            'The host is shared; selected cores were idle at start but not reserved.',
            'Only two NUMA nodes exist; no cross-socket variants beyond NUMA0/NUMA1, and no NUMA interleave policy.',
        ],
    }
    with open(args.out / 'summary.json', 'w') as f:
        json.dump(payload, f, indent=2)
        f.write('\n')
    failed = sum(1 for r in results if r['status'] != 'PASS')
    print(json.dumps({'rounds': len(results), 'failed_rounds': failed,
                      'output': str(args.out)}))
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
