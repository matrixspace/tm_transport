#!/usr/bin/env python3
"""Multi-lane long-running shm_seqlock stability campaign.

Each lane owns one segment, one self-driving writer process and several reader
processes. Writers and readers can be killed/restarted independently; readers
verify generation/sequence/checksum and report gaps, duplicates, ordering and
corruption. The runner samples resources and writes progress/summary JSON.
"""
import argparse
import hashlib
import json
import os
import random
import signal
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

STOP = threading.Event()


def props_string(values):
    defaults = {
        'mlock': 'false', 'layout': 'inline', 'slotSize': '8192', 'slotCount': '1024',
        'wait': 'spin', 'read': 'classic', 'spinCount': '1', 'pauseAfter': '1', 'join': 'latest',
    }
    defaults.update({k: str(v) for k, v in values.items()})
    return ','.join(f'{k}={v}' for k, v in defaults.items())


def lane_configs():
    return [
        dict(name='fixed-low-2r', rate=100, payload='fixed:64', readers=2, slow=[0, 0],
             props=dict(layout='inline', slotSize=512, slotCount=4096, wait='futex'),
             expect_fast_no_loss=True),
        dict(name='fixed-high-2r', rate=100000, payload='fixed:64', readers=2, slow=[0, 0],
             props=dict(layout='inline', slotSize=512, slotCount=4096, wait='spin'),
             expect_fast_no_loss=True),
        dict(name='var-high-8r', rate=20000, payload='random:64:4096', readers=8, slow=[0] * 8,
             props=dict(layout='split', slotSize=8192, slotCount=1024, wait='spin', stride128='true'),
             expect_fast_no_loss=True),
        dict(name='ultra-var-max', rate=0, payload='random:64:4096', readers=8, slow=[0] * 8,
             props=dict(layout='split', slotSize=8192, slotCount=1024, wait='spin', stride128='true'),
             expect_fast_no_loss=False),
        dict(name='fixed-4k-premark-4r', rate=10000, payload='fixed:4096', readers=4, slow=[0] * 4,
             props=dict(layout='inline-premark', slotSize=8192, slotCount=512, wait='spin', read='spec'),
             expect_fast_no_loss=True),
        dict(name='restarts', rate=2000, payload='random:128:1024', readers=4, slow=[0, 0, 2000, 0],
             props=dict(layout='inline', slotSize=2048, slotCount=2048, wait='futex'),
             restart=dict(writer=(120, 300), reader=(60, 240), reader_count=(1, 1)),
             expect_fast_no_loss=False),
        dict(name='slow-vs-fast', rate=2000, payload='fixed:256', readers=3, slow=[0, 0, 5000],
             props=dict(layout='inline', slotSize=1024, slotCount=4096, wait='spin'),
             expect_fast_no_loss=True),
        dict(name='tiny-ring', rate=1000, payload='fixed:16', readers=3, slow=[0, 0, 2000],
             props=dict(layout='split', slotSize=128, slotCount=2, wait='spin'),
             expect_fast_no_loss=False),
        dict(name='large-slot', rate=100, payload='fixed:1048576', readers=2, slow=[0, 0],
             props=dict(layout='inline', slotSize=2097152, slotCount=64, wait='futex'),
             expect_fast_no_loss=True),
        dict(name='mlock-writerlock', rate=1000, payload='fixed:256', readers=2, slow=[0, 0],
             props=dict(layout='inline', slotSize=512, slotCount=512, wait='futex',
                        mlock='true', writerLock='true'),
             expect_fast_no_loss=True),
        # Cross-NUMA lanes. Physical cores: NUMA0 0/2/.../26, NUMA1 1/3/.../27
        # (siblings are +28). The four lanes use disjoint cores so concurrent
        # lanes do not share a spinning reader core: same lane occupies
        # NUMA1 13/15/17/19/21, cross-w0 lane NUMA0 12 + NUMA1 23/25/27/3,
        # cross-w1 lane NUMA1 5 + NUMA0 14, var lane NUMA0 16 + NUMA1 1/7/9/11.
        dict(name='numa-same-w1-r1-4r', rate=20000, payload='fixed:256', readers=4, slow=[0] * 4,
             props=dict(layout='inline', slotSize=512, slotCount=4096, wait='spin'),
             affinity=dict(writer='13', readers=['15', '17', '19', '21']),
             expect_fast_no_loss=True),
        dict(name='numa-cross-w0-r1-4r', rate=20000, payload='fixed:256', readers=4, slow=[0] * 4,
             props=dict(layout='inline', slotSize=512, slotCount=4096, wait='spin'),
             affinity=dict(writer='12', readers=['23', '25', '27', '3']),
             expect_fast_no_loss=True),
        dict(name='numa-cross-w1-r0-1r', rate=10000, payload='fixed:256', readers=1, slow=[0],
             props=dict(layout='inline', slotSize=512, slotCount=4096, wait='futex'),
             affinity=dict(writer='5', readers=['14']),
             expect_fast_no_loss=True),
        dict(name='numa-cross-w0-r1-var4r', rate=20000, payload='random:64:4096', readers=4, slow=[0] * 4,
             props=dict(layout='split', slotSize=8192, slotCount=1024, wait='spin', stride128='true'),
             affinity=dict(writer='16', readers=['1', '7', '9', '11']),
             expect_fast_no_loss=True),
    ]


def validate_affinity_disjoint(configs):
    # Concurrent lanes must not share a pinned CPU: two spinning reader threads
    # on one core inflate latency by orders of magnitude and confound the
    # same-node versus cross-node comparison.
    used = {}
    for cfg in configs:
        affinity = cfg.get('affinity')
        if not affinity:
            continue
        placements = []
        writer = affinity.get('writer')
        if writer:
            placements.append(('writer', writer))
        readers = affinity.get('readers')
        if isinstance(readers, (list, tuple)):
            placements.extend((f'reader-{i}', cpus) for i, cpus in enumerate(readers))
        elif readers:
            placements.append(('readers', readers))
        for role, cpus in placements:
            for cpu in str(cpus).split(','):
                if cpu in used:
                    raise SystemExit(
                        f"affinity overlap on cpu {cpu}: {used[cpu]} and {cfg['name']} {role}")
                used[cpu] = f"{cfg['name']} {role}"


def validate_lane(cfg):
    extra = cfg['props']
    slot_size = int(extra.get('slotSize', 8192))
    slot_count = int(extra.get('slotCount', 1024))
    if slot_count < 2 or slot_count > (1 << 24) or (slot_count & (slot_count - 1)):
        raise SystemExit(f"{cfg['name']}: slotCount must be a power of two between 2 and 2^24")
    if cfg['payload'].startswith('fixed:'):
        maximum = int(cfg['payload'].split(':')[1])
    else:
        maximum = int(cfg['payload'].split(':')[2])
    if 1 + 40 + maximum > slot_size:
        raise SystemExit(f"{cfg['name']}: slotSize {slot_size} cannot hold topic+header+{maximum}")
    affinity = cfg.get('affinity')
    if affinity:
        for key in ('writer', 'readers'):
            values = affinity.get(key)
            if values is None:
                continue
            for value in (values if isinstance(values, (list, tuple)) else [values]):
                if not all(part.isdigit() for part in str(value).split(',')):
                    raise SystemExit(f"{cfg['name']}: invalid affinity cpu list {value!r}")


def parse_stat(line):
    parts = line.split()
    if len(parts) < 3 or parts[0] != 'STAT':
        return None, None
    fields = {}
    for item in parts[2:]:
        if '=' not in item:
            continue
        key, value = item.split('=', 1)
        try:
            fields[key] = int(value)
        except ValueError:
            fields[key] = value
    return parts[1], fields


class Worker:
    def __init__(self, lane, kind, index, slow_us, binary, out_dir):
        self.lane = lane
        self.kind = kind
        self.index = index
        self.slow_us = slow_us
        self.binary = binary
        self.instance = uuid.uuid4().hex[:8]
        self.log_path = out_dir / f'{kind}-{index}-{self.instance}.stdout'
        self.err_path = out_dir / f'{kind}-{index}-{self.instance}.stderr'
        self.log = None
        self.err = None
        self.proc = None
        self.thread = None
        self.ready = threading.Event()
        self.stats = {}
        self.final = None
        self.error = None
        self.stats_lock = threading.Lock()
        self.started_at = time.monotonic()
        self.resource_max = {}
        self.killed_by_runner = False

    def command(self, seed):
        base = [str(self.binary), self.kind, self.lane.segment, self.lane.props,
                str(self.lane.cfg['rate']), self.lane.cfg['payload'], str(seed), str(self.slow_us)]
        cpus = self.lane.affinity_for(self.kind, self.index)
        return (['taskset', '-c', cpus] + base) if cpus else base

    def start(self, seed):
        self.log = open(self.log_path, 'w', buffering=1)
        self.err = open(self.err_path, 'w')
        self.proc = subprocess.Popen(self.command(seed), stdout=subprocess.PIPE, stderr=self.err,
                                     stdin=subprocess.DEVNULL, text=True, bufsize=1,
                                     start_new_session=True)
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self):
        for line in self.proc.stdout:
            line = line.rstrip('\n')
            self.log.write(f'{time.monotonic():.9f} {line}\n')
            if line.startswith('READY'):
                self.ready.set()
            elif line.startswith('STAT '):
                kind, fields = parse_stat(line)
                if fields is not None:
                    with self.stats_lock:
                        self.stats = dict(fields)
                        if fields.get('final'):
                            self.final = dict(fields)
            elif line.startswith('ERROR'):
                self.error = line
                self.lane.violation(f'{self.kind}-{self.index} {self.instance}: {line}')
        self.log.write(f'{time.monotonic():.9f} EOF\n')
        self.log.flush()

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def wait_ready(self, timeout):
        return self.ready.wait(timeout)

    def stop(self, timeout=5.0):
        if self.alive():
            try:
                self.proc.terminate()
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.kill()
        if self.thread:
            self.thread.join(timeout=2)
        self._close()
        return self.proc.returncode if self.proc else None

    def kill(self):
        if self.alive():
            self.killed_by_runner = True
            try:
                self.proc.kill()
            except ProcessLookupError:
                pass
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        if self.thread:
            self.thread.join(timeout=2)
        self._close()

    def _close(self):
        for stream in (self.log, self.err):
            if stream and not stream.closed:
                stream.close()
        if self.proc and self.proc.stdout and not self.proc.stdout.closed:
            self.proc.stdout.close()

    def snapshot(self):
        with self.stats_lock:
            return dict(self.stats)

    def resource_sample(self):
        if not self.alive():
            return
        pid = self.proc.pid
        try:
            status = Path(f'/proc/{pid}/status').read_text()
            rss = threads = 0
            for line in status.splitlines():
                if line.startswith('VmRSS:'):
                    rss = int(line.split()[1])
                elif line.startswith('Threads:'):
                    threads = int(line.split()[1])
            maps = sum(1 for _ in open(f'/proc/{pid}/maps'))
            fds = len(list(Path(f'/proc/{pid}/fd').iterdir()))
            pss = 0
            try:
                for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines():
                    if line.startswith('Pss:'):
                        pss = int(line.split()[1])
            except OSError:
                pass
            values = {'rss_kib': rss, 'pss_kib': pss, 'threads': threads, 'maps': maps, 'fds': fds}
            for key, value in values.items():
                self.resource_max[key] = max(self.resource_max.get(key, 0), value)
        except OSError:
            pass


class Lane:
    def __init__(self, cfg, binary, out_root, run_id, seed, restart_scale):
        self.cfg = cfg
        self.segment = f'shmstab_{run_id}_{cfg["name"]}'
        self.props = props_string(cfg['props'])
        self.binary = binary
        self.out_dir = out_root / cfg['name']
        self.out_dir.mkdir(parents=True)
        self.seed = seed
        self.restart_scale = restart_scale
        self.writer = None
        self.writer_starts = 0
        self.readers = []
        self.instances = []
        self.restarts = {'writer': 0, 'reader': 0}
        self.violations = []
        self._violation_seen = set()
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.started_at = time.monotonic()

    def affinity_for(self, kind, index):
        # Optional per-lane placement: writer gets one CPU list, readers get a
        # per-index list (one CPU each) or one shared list. None means inherit.
        affinity = self.cfg.get('affinity')
        if not affinity:
            return None
        if kind == 'writer':
            return str(affinity.get('writer', '') or '')
        readers = affinity.get('readers', '')
        if isinstance(readers, (list, tuple)):
            return str(readers[index % len(readers)])
        return str(readers)

    def violation(self, message, key=None):
        with self.lock:
            key = key or message
            if key in self._violation_seen:
                return
            self._violation_seen.add(key)
            self.violations.append(message)

    def _new_worker(self, kind, index, slow_us):
        worker = Worker(self, kind, index, slow_us, self.binary, self.out_dir)
        self.instances.append(worker)
        return worker

    def _start_writer(self):
        self.writer_starts += 1
        for attempt in range(40):
            writer = self._new_worker('writer', 0, 0)
            writer.start(self.seed + 1000003 * self.writer_starts + attempt)
            if writer.wait_ready(10):
                self.writer = writer
                return True
            writer.kill()
            if writer.error and 'another live publisher' in writer.error:
                time.sleep(0.2)
                continue
            self.violation(f'writer failed to start: {writer.error}')
            return False
        self.violation('writer could not acquire the segment lock')
        return False

    def _start_reader(self, index):
        slow_us = self.cfg['slow'][index]
        reader = self._new_worker('reader', index, slow_us)
        reader.start(self.seed + 1000 + index)
        if not reader.wait_ready(30):
            # A reader started while the writer is down blocks in
            # addSubscriptionClient until the next writer appears. That is
            # expected in the restart lane, so it is a note, not a violation.
            print(f'{self.cfg["name"]}: reader-{index} not ready yet', flush=True)
        self.readers[index] = reader

    def start(self):
        if not self._start_writer():
            return False
        self.readers = [None] * self.cfg['readers']
        for index in range(self.cfg['readers']):
            self._start_reader(index)
        if self.cfg.get('restart'):
            threading.Thread(target=self._restart_loop, daemon=True).start()
        return True

    def _restart_loop(self):
        rng = random.Random(self.seed ^ hash(self.cfg['name']))
        restart = self.cfg['restart']
        next_writer = time.monotonic() + rng.uniform(*restart['writer']) * self.restart_scale
        next_reader = time.monotonic() + rng.uniform(*restart['reader']) * self.restart_scale
        while not self.stop_event.wait(1.0):
            now = time.monotonic()
            if now >= next_writer:
                self.restart_writer()
                next_writer = time.monotonic() + rng.uniform(*restart['writer']) * self.restart_scale
            if now >= next_reader:
                count = rng.randint(*restart.get('reader_count', (1, 1)))
                for index in rng.sample(range(len(self.readers)), count):
                    self.restart_reader(index)
                next_reader = time.monotonic() + rng.uniform(*restart['reader']) * self.restart_scale

    def restart_writer(self):
        old = self.writer
        if old:
            old.kill()
        self.restarts['writer'] += 1
        if not self._start_writer():
            self.violation('writer restart failed')

    def restart_reader(self, index):
        old = self.readers[index]
        if old:
            old.kill()
        self.restarts['reader'] += 1
        self._start_reader(index)

    def sample_resources(self):
        workers = [self.writer] + self.readers if self.writer else list(self.readers)
        for worker in workers:
            if worker:
                worker.resource_sample()

    def check(self):
        workers = [self.writer] + list(self.readers)
        for worker in workers:
            if not worker:
                continue
            stats = worker.snapshot()
            if not stats:
                continue
            for field in ('corrupt', 'bad', 'dup', 'ooo'):
                if stats.get(field, 0):
                    self.violation(f'{worker.kind}-{worker.index} {field}={stats[field]}',
                                   key=(worker.instance, field))
            if worker.kind == 'writer' and stats.get('rejected', 0):
                self.violation(f'writer rejected={stats["rejected"]}', key=(worker.instance, 'rejected'))
            if worker.kind == 'reader' and self.cfg.get('expect_fast_no_loss') and not self.cfg.get('restart'):
                if worker.slow_us == 0 and stats.get('lost', 0):
                    self.violation(f'reader-{worker.index} lost={stats["lost"]} in a no-loss lane',
                                   key=(worker.instance, 'lost'))

    def summary(self):
        def worker_summary(worker):
            if not worker:
                return None
            return {
                'instance': worker.instance, 'kind': worker.kind, 'index': worker.index,
                'slow_us': worker.slow_us, 'alive': worker.alive(),
                'started_at_elapsed': worker.started_at - self.started_at,
                'stats': worker.final or worker.snapshot(),
                'resource_max': worker.resource_max,
                'error': worker.error,
            }
        writers = [worker for worker in self.instances if worker.kind == 'writer']
        return {
            'name': self.cfg['name'], 'segment': self.segment, 'props': self.props,
            'rate': self.cfg['rate'], 'payload': self.cfg['payload'],
            'readers': self.cfg['readers'], 'slow': self.cfg['slow'],
            'restart': self.cfg.get('restart'), 'restarts': self.restarts,
            'violations': list(self.violations), 'passed': not self.violations,
            'writer': worker_summary(writers[-1]) if writers else None,
            'writer_instances': [worker_summary(w) for w in writers],
            'reader_instances': [worker_summary(w) for w in self.instances if w.kind == 'reader'],
            'elapsed_seconds': time.monotonic() - self.started_at,
        }

    def stop(self):
        self.stop_event.set()
        for worker in [self.writer] + list(self.readers):
            if worker:
                worker.stop(timeout=5)


def cleanup_segments(lanes):
    for lane in lanes:
        for suffix in ('', '.lock'):
            try:
                Path('/dev/shm', lane.segment + suffix).unlink()
            except FileNotFoundError:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--hours', type=float, default=24.0)
    parser.add_argument('--minutes', type=float, default=0.0, help='smoke duration; overrides --hours')
    parser.add_argument('--lanes', default='all')
    parser.add_argument('--seed', type=int, default=20261113)
    parser.add_argument('--sample-seconds', type=float, default=60.0)
    parser.add_argument('--restart-scale', type=float, default=1.0,
                        help='multiply restart intervals (0.1 for smoke runs)')
    args = parser.parse_args()
    if args.out.exists():
        parser.error('output directory must not already exist')
    args.out.mkdir(parents=True)
    if not args.binary.is_file():
        parser.error('binary does not exist')
    configs = lane_configs()
    if args.lanes != 'all':
        wanted = set(args.lanes.split(','))
        configs = [c for c in configs if c['name'] in wanted]
        missing = wanted - {c['name'] for c in configs}
        if missing:
            parser.error(f'unknown lanes: {sorted(missing)}')
    for cfg in configs:
        validate_lane(cfg)
    validate_affinity_disjoint(configs)
    run_id = f'{int(time.time())}_{uuid.uuid4().hex[:8]}'
    lanes = [Lane(cfg, args.binary.resolve(), args.out, run_id, args.seed, args.restart_scale)
             for cfg in configs]
    duration = (args.minutes * 60) if args.minutes else (args.hours * 3600)

    def stop_handler(signum, frame):
        STOP.set()
    signal.signal(signal.SIGINT, stop_handler)
    signal.signal(signal.SIGTERM, stop_handler)

    environment = {
        'uname': list(os.uname()), 'binary': str(args.binary.resolve()),
        'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        'command': sys.argv, 'lanes': configs, 'duration_seconds': duration,
        'affinity': sorted(os.sched_getaffinity(0)),
    }
    (args.out / 'environment.json').write_text(json.dumps(environment, indent=2))
    started = time.monotonic()
    for lane in lanes:
        if not lane.start():
            STOP.set()
            break
    last_sample = started
    try:
        while not STOP.wait(5.0):
            now = time.monotonic()
            for lane in lanes:
                lane.check()
            if now - last_sample >= args.sample_seconds:
                last_sample = now
                for lane in lanes:
                    lane.sample_resources()
                progress = {
                    'elapsed_seconds': now - started,
                    'lanes': {lane.cfg['name']: {
                        'restarts': lane.restarts, 'violations': lane.violations,
                        'writer': lane.writer.snapshot() if lane.writer else None,
                        'readers': [w.snapshot() for w in lane.readers if w],
                    } for lane in lanes},
                }
                (args.out / 'progress.json').write_text(json.dumps(progress, indent=2))
                print(json.dumps({k: v['violations'] for k, v in progress['lanes'].items()}), flush=True)
            if now - started >= duration:
                break
    finally:
        for lane in lanes:
            lane.stop()
        cleanup_segments(lanes)
        summaries = [lane.summary() for lane in lanes]
        summary = {
            'started_unix': time.time() - (time.monotonic() - started),
            'duration_seconds': time.monotonic() - started,
            'environment': environment, 'lanes': summaries,
            'passed': all(lane['passed'] for lane in summaries),
        }
        (args.out / 'summary.json').write_text(json.dumps(summary, indent=2))
        print(json.dumps({'passed': summary['passed'],
                          'lanes': {lane['name']: lane['passed'] for lane in summaries}}), flush=True)
    return 0 if all(lane['passed'] for lane in summaries) else 1


if __name__ == '__main__':
    raise SystemExit(main())
