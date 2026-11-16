#!/usr/bin/env python3
"""External resource monitor for a running shm_seqlock stability campaign.

This script never starts, stops or signals the measured processes. It discovers
the workers whose parent is the campaign runner, samples /proc, and writes:

  resources.jsonl          one compact record per worker per sample
  progress_snapshots.jsonl one record whenever the runner's progress.json changes
  resource_summary.json    per-PID and per-lane aggregates

Each record carries the experiment elapsed time (derived from the runner's
progress.json) so the resource data can be joined with the runner's summary and
per-instance logs. Shared-memory traffic is mmap traffic, so /proc/pid/io mostly
shows control-plane/log I/O; page faults and CPU are the useful signals there.
"""
import argparse
import json
import os
import signal
import sys
import time
from collections import defaultdict
from pathlib import Path

STOP = False


def stop_handler(signum, frame):
    global STOP
    STOP = True


def read_json(path):
    for _ in range(20):
        try:
            with open(path) as f:
                return json.load(f)
        except (FileNotFoundError, json.JSONDecodeError):
            time.sleep(0.2)
    return None


def read_cmdline(pid):
    try:
        raw = Path(f'/proc/{pid}/cmdline').read_bytes()
    except OSError:
        return []
    return [part.decode('utf-8', 'replace') for part in raw.split(b'\0') if part]


def read_stat(pid):
    try:
        text = Path(f'/proc/{pid}/stat').read_text()
    except OSError:
        return None
    # comm may contain spaces; everything after the last ')' is fixed fields.
    tail = text[text.rfind(')') + 2:].split()
    try:
        return {
            'state': tail[0],
            'minflt': int(tail[7]),
            'majflt': int(tail[9]),
            'utime': int(tail[11]),
            'stime': int(tail[12]),
            'num_threads': int(tail[17]),
            'starttime': int(tail[19]),
        }
    except (IndexError, ValueError):
        return None


def read_status(pid):
    values = {}
    try:
        text = Path(f'/proc/{pid}/status').read_text()
    except OSError:
        return values
    keys = {
        'VmPeak': 'vm_peak_kib', 'VmSize': 'vm_size_kib', 'VmLck': 'vm_lck_kib',
        'VmPin': 'vm_pin_kib', 'VmHWM': 'vm_hwm_kib', 'VmRSS': 'rss_kib',
        'RssAnon': 'rss_anon_kib', 'RssFile': 'rss_file_kib', 'RssShmem': 'rss_shmem_kib',
        'VmSwap': 'swap_kib', 'Threads': 'threads',
        'voluntary_ctxt_switches': 'vctx', 'nonvoluntary_ctxt_switches': 'nctx',
    }
    for line in text.splitlines():
        key, _, rest = line.partition(':')
        if key in keys:
            try:
                values[keys[key]] = int(rest.split()[0])
            except (IndexError, ValueError):
                pass
    return values


def read_io(pid):
    values = {}
    try:
        text = Path(f'/proc/{pid}/io').read_text()
    except OSError:
        return values
    keys = {'rchar': 'rchar', 'wchar': 'wchar', 'syscr': 'syscr', 'syscw': 'syscw',
            'read_bytes': 'read_bytes', 'write_bytes': 'write_bytes'}
    for line in text.splitlines():
        key, _, rest = line.partition(':')
        if key in keys:
            try:
                values[keys[key]] = int(rest.strip())
            except ValueError:
                pass
    return values


def read_smaps_rollup(pid):
    values = {}
    try:
        text = Path(f'/proc/{pid}/smaps_rollup').read_text()
    except OSError:
        return values
    keys = {'Pss': 'pss_kib', 'Pss_Anon': 'pss_anon_kib', 'Pss_File': 'pss_file_kib',
            'Pss_Shmem': 'pss_shmem_kib', 'Shared_Clean': 'shared_clean_kib',
            'Shared_Dirty': 'shared_dirty_kib', 'Private_Clean': 'private_clean_kib',
            'Private_Dirty': 'private_dirty_kib', 'Referenced': 'referenced_kib',
            'Anonymous': 'anonymous_kib', 'Swap': 'smaps_swap_kib'}
    for line in text.splitlines():
        key, _, rest = line.partition(':')
        if key in keys:
            try:
                values[keys[key]] = int(rest.split()[0])
            except (IndexError, ValueError):
                pass
    return values


def read_maps(pid):
    count = 0
    total = 0
    try:
        with open(f'/proc/{pid}/maps') as f:
            for line in f:
                count += 1
                parts = line.split()
                if len(parts) >= 2:
                    try:
                        lo, hi = parts[0].split('-')
                        total += int(hi, 16) - int(lo, 16)
                    except ValueError:
                        pass
    except OSError:
        return {}, 0
    return {'maps': count, 'maps_bytes': total}, count


def read_fds(pid):
    counts = defaultdict(int)
    try:
        entries = list(Path(f'/proc/{pid}/fd').iterdir())
    except OSError:
        return {}
    for entry in entries:
        try:
            target = os.readlink(entry)
        except OSError:
            continue
        if target.startswith('socket:'):
            counts['fds_socket'] += 1
        elif target.startswith('pipe:'):
            counts['fds_pipe'] += 1
        elif target.startswith('anon_inode:'):
            counts['fds_anon_inode'] += 1
        elif target.startswith('/dev/shm/'):
            counts['fds_dev_shm'] += 1
        else:
            counts['fds_file'] += 1
    counts['fds'] = len(entries)
    return dict(counts)


def boot_time():
    try:
        for line in Path('/proc/stat').read_text().splitlines():
            if line.startswith('btime '):
                return int(line.split()[1])
    except OSError:
        pass
    return int(time.time())


def discover_runner(out_dir, explicit_pid):
    if explicit_pid:
        if Path(f'/proc/{explicit_pid}/cmdline').exists():
            return explicit_pid
        return None
    marker = f'--out={out_dir}'.encode()
    needle = b'run_stability.py'
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            raw = (entry / 'cmdline').read_bytes()
        except OSError:
            continue
        if needle in raw and marker in raw and b'python' in raw.split(b'\0')[0]:
            return int(entry.name)
    return None


def env_seed_and_lanes(out_dir):
    environment = read_json(Path(out_dir) / 'environment.json') or {}
    command = environment.get('command', [])
    seed = 20261113
    for i, value in enumerate(command):
        if value == '--seed' and i + 1 < len(command):
            seed = int(command[i + 1])
        elif value.startswith('--seed='):
            seed = int(value.split('=', 1)[1])
    lanes = [lane['name'] for lane in environment.get('lanes', [])]
    return seed, lanes, environment


def identify(pid, runner_pid, lanes, base_seed, hz):
    cmdline = read_cmdline(pid)
    if len(cmdline) < 8 or os.path.basename(cmdline[0]) != 'shm_seqlock_stability_worker':
        return None
    stat = read_stat(pid)
    if not stat or stat['state'] == 'Z':
        return None
    # Only workers started by this campaign runner (restarts keep the same PPID).
    status = read_status(pid)
    ppid = None
    try:
        text = Path(f'/proc/{pid}/status').read_text()
        for line in text.splitlines():
            if line.startswith('PPid:'):
                ppid = int(line.split()[1])
                break
    except OSError:
        pass
    if ppid != runner_pid:
        return None
    segment = cmdline[2]
    lane = 'unknown'
    for name in lanes:
        if segment.endswith('_' + name):
            lane = name
            break
    role = cmdline[1]
    rate = cmdline[4]
    payload = cmdline[5]
    try:
        seed = int(cmdline[6])
        slow_us = int(cmdline[7])
    except (ValueError, IndexError):
        seed, slow_us = 0, 0
    reader_index = None
    if role == 'reader':
        candidate = seed - base_seed - 1000
        if 0 <= candidate < 64:
            reader_index = candidate
    return {
        'lane': lane, 'role': role, 'reader_index': reader_index,
        'seed': seed, 'slow_us': slow_us, 'rate': rate, 'payload': payload,
        'segment': segment, 'ppid': ppid,
    }


def sample(pid):
    stat = read_stat(pid)
    if not stat:
        return None
    values = {'cpu_jiffies': stat['utime'] + stat['stime'],
              'minflt': stat['minflt'], 'majflt': stat['majflt'],
              'threads_stat': stat['num_threads'], 'starttime': stat['starttime']}
    values.update(read_status(pid))
    values.update(read_io(pid))
    values.update(read_smaps_rollup(pid))
    maps, _ = read_maps(pid)
    values.update(maps)
    values.update(read_fds(pid))
    return values


class Monitor:
    def __init__(self, out_dir, monitor_dir, interval, base_seed, lanes, hz):
        self.out_dir = out_dir
        self.monitor_dir = monitor_dir
        self.interval = interval
        self.base_seed = base_seed
        self.lanes = lanes
        self.hz = hz
        self.runner_pid = None
        self.runner_start_wall = None
        self.prev = {}
        self.records = []
        self.by_pid = {}
        self.by_group = defaultdict(lambda: {'pids': set(), 'samples': 0})
        self.last_progress_elapsed = None

    def refresh_runner(self):
        if self.runner_pid and Path(f'/proc/{self.runner_pid}').exists():
            return True
        self.runner_pid = discover_runner(self.out_dir, None)
        if not self.runner_pid:
            return False
        progress = read_json(self.out_dir / 'progress.json')
        if progress:
            self.runner_start_wall = time.time() - progress['elapsed_seconds']
        return True

    def experiment_elapsed(self):
        if self.runner_start_wall is None:
            progress = read_json(self.out_dir / 'progress.json')
            if not progress:
                return None
            self.runner_start_wall = time.time() - progress['elapsed_seconds']
        return time.time() - self.runner_start_wall

    def scan(self):
        if not self.refresh_runner():
            return []
        workers = []
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit():
                continue
            pid = int(entry.name)
            info = identify(pid, self.runner_pid, self.lanes, self.base_seed, self.hz)
            if info:
                workers.append((pid, info))
        return workers

    def sample_once(self):
        exp = self.experiment_elapsed()
        if exp is None:
            return
        now = time.time()
        mono = time.monotonic()
        for pid, info in self.scan():
            values = sample(pid)
            if not values:
                continue
            previous = self.prev.get(pid)
            cpu_percent = None
            if previous:
                dt = mono - previous[0]
                dj = values['cpu_jiffies'] - previous[1]
                if dt > 0:
                    cpu_percent = (dj / self.hz) / dt * 100.0
            self.prev[pid] = (mono, values['cpu_jiffies'])
            record = {
                't': now, 'mono': mono, 'exp': exp,
                'lane': info['lane'], 'role': info['role'], 'reader_index': info['reader_index'],
                'pid': pid, 'seed': info['seed'], 'slow_us': info['slow_us'],
                'rate': info['rate'], 'payload': info['payload'],
                'cpu_percent': None if cpu_percent is None else round(cpu_percent, 3),
            }
            record.update(values)
            self.records.append(record)
            key = (info['lane'], info['role'], info['reader_index'])
            group = self.by_group[key]
            group['pids'].add(pid)
            group['samples'] += 1
            entry = self.by_pid.get(pid)
            if entry is None:
                entry = {'first': record, 'last': record, 'samples': 0, 'agg': {}}
                self.by_pid[pid] = entry
            entry['last'] = record
            entry['samples'] += 1
            for key, value in record.items():
                if isinstance(value, (int, float)) and key not in ('t', 'mono', 'exp', 'pid', 'seed', 'slow_us'):
                    aggregate = entry['agg'].setdefault(
                        key, {'min': value, 'max': value, 'sum': 0.0, 'count': 0})
                    aggregate['min'] = min(aggregate['min'], value)
                    aggregate['max'] = max(aggregate['max'], value)
                    aggregate['sum'] += value
                    aggregate['count'] += 1
        progress = read_json(self.out_dir / 'progress.json')
        if progress and progress.get('elapsed_seconds') != self.last_progress_elapsed:
            self.last_progress_elapsed = progress.get('elapsed_seconds')
            snapshot = {'t': now, 'exp': exp, 'progress_elapsed': progress.get('elapsed_seconds')}
            snapshot['lanes'] = {
                name: {
                    'restarts': value.get('restarts'),
                    'violations': value.get('violations'),
                    'writer': value.get('writer'),
                    'readers': value.get('readers'),
                }
                for name, value in progress.get('lanes', {}).items()
            }
            with open(self.monitor_dir / 'progress_snapshots.jsonl', 'a') as f:
                f.write(json.dumps(snapshot, separators=(',', ':')) + '\n')

    def write_records(self):
        if not self.records:
            return
        with open(self.monitor_dir / 'resources.jsonl', 'a') as f:
            for record in self.records:
                f.write(json.dumps(record, separators=(',', ':')) + '\n')
        self.records.clear()

    @staticmethod
    def combine_agg(entries):
        combined = {}
        for entry in entries:
            for key, value in entry['agg'].items():
                target = combined.setdefault(
                    key, {'min': value['min'], 'max': value['max'], 'sum': 0.0, 'count': 0})
                target['min'] = min(target['min'], value['min'])
                target['max'] = max(target['max'], value['max'])
                target['sum'] += value['sum']
                target['count'] += value['count']
        return {
            key: {'min': value['min'], 'max': value['max'],
                  'avg': value['sum'] / value['count'], 'count': value['count']}
            for key, value in combined.items()
        }

    def summary(self):
        groups = defaultdict(list)
        for pid, entry in self.by_pid.items():
            first = entry['first']
            groups[(first['lane'], first['role'], first['reader_index'])].append(entry)
        lanes = defaultdict(lambda: defaultdict(list))
        for key, entries in groups.items():
            for entry in entries:
                lanes[key[0]][key[1]].append(entry)
        out = {
            'runner_pid': self.runner_pid,
            'runner_start_wall': self.runner_start_wall,
            'generated_wall': time.time(),
            'lanes': {}, 'groups': {}, 'pid_count': len(self.by_pid),
        }
        for key, entries in groups.items():
            out['groups']['|'.join('' if x is None else str(x) for x in key)] = {
                'instances': len(entries),
                'samples': sum(entry['samples'] for entry in entries),
                'first_exp': min(entry['first']['exp'] for entry in entries),
                'last_exp': max(entry['last']['exp'] for entry in entries),
                'pids': sorted(entry['first']['pid'] for entry in entries),
                'aggregate': self.combine_agg(entries),
                'per_instance': [
                    {'pid': entry['first']['pid'], 'samples': entry['samples'],
                     'first_exp': entry['first']['exp'], 'last_exp': entry['last']['exp'],
                     'aggregate': {
                         k: {'min': v['min'], 'max': v['max'], 'avg': v['sum'] / v['count'], 'count': v['count']}
                         for k, v in entry['agg'].items()
                     }}
                    for entry in entries
                ],
            }
        for lane, roles in lanes.items():
            for role, entries in roles.items():
                out['lanes'].setdefault(lane, {})[role] = {
                    'instances': len(entries),
                    'samples': sum(entry['samples'] for entry in entries),
                    'aggregate': self.combine_agg(entries),
                }
        with open(self.monitor_dir / 'resource_summary.json', 'w') as f:
            json.dump(out, f, indent=2)
            f.write('\n')

    def run(self, hours):
        started = time.time()
        deadline = started + hours * 3600 if hours > 0 else None
        last_summary = 0.0
        self.last_progress_elapsed = None
        while not STOP:
            try:
                self.sample_once()
                self.write_records()
                now = time.time()
                if now - last_summary >= 600:
                    self.summary()
                    last_summary = now
                if deadline and now >= deadline:
                    break
                if not Path(f'/proc/{self.runner_pid}').exists() if self.runner_pid else False:
                    # Give a final sample, then stop.
                    time.sleep(self.interval)
                    self.sample_once()
                    self.write_records()
                    break
            except Exception as exc:  # keep monitoring across transient /proc races
                print(f'monitor sample error: {exc}', file=sys.stderr, flush=True)
            sleep_until = time.monotonic() + self.interval
            while not STOP and time.monotonic() < sleep_until:
                time.sleep(min(0.5, max(0.0, sleep_until - time.monotonic())))
        self.write_records()
        self.summary()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True, help="stability run output directory")
    parser.add_argument('--monitor-out', type=Path, required=True)
    parser.add_argument('--runner-pid', type=int, default=0)
    parser.add_argument('--interval', type=float, default=30.0)
    parser.add_argument('--hours', type=float, default=72.0)
    args = parser.parse_args()
    if args.monitor_out.exists():
        parser.error('monitor output directory must not already exist')
    args.monitor_out.mkdir(parents=True)
    base_seed, lanes, environment = env_seed_and_lanes(args.out)
    if not lanes:
        parser.error(f'cannot read lanes from {args.out}/environment.json')
    hz = os.sysconf('SC_CLK_TCK')
    monitor = Monitor(args.out, args.monitor_out, args.interval, base_seed, lanes, hz)
    monitor.runner_pid = args.runner_pid or discover_runner(args.out, None)
    progress = read_json(args.out / 'progress.json')
    if progress:
        monitor.runner_start_wall = time.time() - progress['elapsed_seconds']
    with open(args.monitor_out / 'environment.json', 'w') as f:
        json.dump({'out': str(args.out), 'runner_pid': monitor.runner_pid,
                   'base_seed': base_seed, 'lanes': lanes, 'interval': args.interval,
                   'hours': args.hours, 'command': sys.argv,
                   'stability_environment': environment}, f, indent=2)
        f.write('\n')
    signal.signal(signal.SIGINT, stop_handler)
    signal.signal(signal.SIGTERM, stop_handler)
    monitor.run(args.hours)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
