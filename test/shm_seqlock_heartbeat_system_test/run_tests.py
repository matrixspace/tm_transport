#!/usr/bin/env python3
"""Real heartbeat, discovery and TTL tests. Control uses pipes, data uses shm.

Each case runs in fresh service/client processes, bounded by phase deadlines.
Only explicitly registered children and shared-memory filenames are cleaned.
"""
import argparse
import collections
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import threading
import time
import uuid


class PhaseTimeout(TimeoutError):
    pass


def hx(data):
    return data.hex() or '-'


class Worker:
    def __init__(self, suite, role, hb, data, timer_ms=0, expect_ready=True, risk=False):
        self.suite = suite
        self.risk = risk
        self.expected_error = False
        self.records = []
        self.pending = collections.deque()
        self.lines = queue.Queue()
        self.log = (suite.output / f'{len(suite.children):03d}-{role}.log').open('w')
        self.process = subprocess.Popen([suite.binary, role, hb, data, str(timer_ms), 'first-unlocked' if risk and role == 'service' else '-'], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        suite.children.append(self)
        def read():
            for line in self.process.stdout:
                self.log.write(line)
                self.log.flush()
                self.records.append(line.rstrip('\n'))
                self.lines.put(line.rstrip('\n'))
            self.lines.put(None)
        self.thread = threading.Thread(target=read, daemon=True)
        self.thread.start()
        if expect_ready:
            self.expect('READY')

    def send(self, command):
        assert self.process.poll() is None, f'worker exited {self.process.returncode}'
        self.process.stdin.write(command + '\n')
        self.process.stdin.flush()

    def expect(self, prefix, timeout=5):
        deadline = time.monotonic() + timeout
        while True:
            for line in list(self.pending):
                if line == prefix or line.startswith(prefix + ' '):
                    self.pending.remove(line)
                    if prefix == 'ERROR':
                        self.expected_error = True
                    return line
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise PhaseTimeout(f'timeout waiting {prefix}; pending={list(self.pending)[-10:]}')
            try:
                line = self.lines.get(timeout=remaining)
            except queue.Empty:
                raise PhaseTimeout(f'timeout waiting {prefix}')
            assert line is not None, f'worker exited waiting {prefix}: {self.process.poll()}'
            assert self.risk or prefix == 'ERROR' or not line.startswith(('ERROR ', 'BAD_')), line
            self.pending.append(line)

    def quiet(self, prefix, seconds=0.15):
        # Negative observations have an explicit time window, not a readiness sleep.
        deadline = time.monotonic() + seconds
        while True:
            assert not any(x == prefix or x.startswith(prefix + ' ') for x in self.pending), f'unexpected {prefix}'
            try:
                line = self.lines.get(timeout=max(0.001, deadline-time.monotonic()))
            except queue.Empty:
                return
            assert line is not None, 'unexpected exit'
            assert self.risk or not line.startswith(('ERROR ', 'BAD_')), line
            self.pending.append(line)
            if time.monotonic() >= deadline:
                assert not any(x == prefix or x.startswith(prefix + ' ') for x in self.pending), f'unexpected {prefix}'
                return

    def close(self, kill=False):
        if self.process.poll() is None:
            if kill:
                self.process.kill()
            else:
                try:
                    self.send('QUIT')
                    self.process.wait(timeout=5)
                except (AssertionError, BrokenPipeError, subprocess.TimeoutExpired):
                    self.process.kill()
        self.process.wait(timeout=5)
        self.thread.join(timeout=2)
        self.log.close()
        self.process.stdin.close()
        self.process.stdout.close()
        # Last-iteration failures and duplicates may arrive after the final
        # expect(). Always drain after child exit, when no producer remains.
        while True:
            try:
                line = self.lines.get_nowait()
            except queue.Empty:
                break
            if line is not None:
                self.pending.append(line)
        if not kill and not self.risk:
            assert not any(x.startswith('BAD_') for x in self.records), 'invalid tail record'
            assert self.expected_error or not any(x.startswith('ERROR ') for x in self.records), 'worker error after final assertion'
            assert not any(x.startswith(('DATA ', 'ALERT ', 'CUSTOM ', 'HB ', 'REGISTER', 'DEREGISTER')) for x in self.pending), 'unchecked or duplicated tail delivery'
        return self.process.returncode


class Suite:
    def __init__(self, binary, output, layout):
        self.binary, self.output, self.layout = binary, output, layout
        self.children = []
        self.segments = []
        self.prefix = 'shmhb_' + uuid.uuid4().hex[:16]

    def channel(self, name, capacity=4096, count=64):
        segment = self.prefix + '_' + name
        self.segments.append(segment)
        return f'shm_seqlock://::::{segment}[slotSize={capacity},slotCount={count},layout={self.layout},writerLock=true,wait=futex,spinCount=1,mlock=false]'

    def cleanup(self):
        for child in self.children:
            child.close(kill=True)
        # No live publisher exists when lock files are removed.
        for name in self.segments:
            for suffix in ('', '.lock'):
                try:
                    Path('/dev/shm', name+suffix).unlink()
                except FileNotFoundError:
                    pass

    def pair(self, shared=False, capacity=4096, timer_ms=0, risk=False):
        hb = self.channel('hb', capacity)
        data = hb if shared else self.channel('data')
        service = Worker(self, 'service', hb, data, timer_ms, risk=risk)
        client = Worker(self, 'client', hb, data, risk=risk)
        # Prove the heartbeat reader and discovery path actually deliver data.
        # Repeated startup probes do not enter formal sequence accounting.
        deadline = time.monotonic()+5
        service.send('HB')
        service.expect('SENT_HB')
        self.check_hb(client.expect('HB'), service)
        client.expect('REGISTER')
        marker = b'readiness-probe'
        while True:
            service.send('DATA '+hx(marker))
            service.expect('SENT_DATA')
            try:
                assert client.expect('DATA', timeout=min(0.2, max(0.01, deadline-time.monotonic()))) == 'DATA '+hx(marker)
                break
            except PhaseTimeout:
                if time.monotonic() >= deadline:
                    raise
        return service, client, hb, data

    @staticmethod
    def check_hb(line, service, status='Good', channels=1):
        _, identity, pid, name, timestamp, actual_status, count, host = line.split()
        uuid.UUID(identity)
        assert int(pid) == service.process.pid and name == 'system-service', line
        assert abs(int(timestamp)/1000-time.time()) < 5, line
        assert actual_status == status and int(count) == channels, line
        assert bytes.fromhex(host), line
        return identity

    def direct(self):
        s, c, _, _ = self.pair()
        s.send('STATUS Warning'); s.expect('STATUS_SET')
        s.send('HB'); s.expect('SENT_HB')
        self.check_hb(c.expect('HB'), s, 'Warning')
        for command, prefix in [('ALERT', 'ALERT'), ('CUSTOM', 'CUSTOM')]:
            payload = b'hello\x00\xff-heartbeat'
            s.send(command+' '+hx(payload)); s.expect('SENT_'+command)
            fields = c.expect(prefix).split()
            assert fields == [prefix, 'system-service', str(s.process.pid), hx(payload)], fields
        # Repeated heartbeats must not install duplicate subscriptions.
        for _ in range(10):
            s.send('HB'); s.expect('SENT_HB'); self.check_hb(c.expect('HB'), s, 'Warning')
        for k in range(20):
            payload = bytes(range(256))+f'/{k}'.encode()
            s.send('DATA '+hx(payload)); s.expect('SENT_DATA')
            assert c.expect('DATA') == 'DATA '+hx(payload)
        c.quiet('DATA')
        assert s.close() == 0
        assert c.close() == 0

    def ttl(self):
        s, c, _, _ = self.pair()
        # Actual converter records receiver wall-clock acceptance time. These
        # checks deliberately use real elapsed time, rather than heartbeat time.
        c.send('CHECK'); c.expect('CHECKED'); c.quiet('DEREGISTER', 0.1)
        time.sleep(1.1)
        c.send('CHECK'); c.expect('DEREGISTER'); c.expect('CHECKED')
        c.send('CHECK'); c.expect('CHECKED'); c.quiet('DEREGISTER', 0.1)
        s.send('HB'); s.expect('SENT_HB'); self.check_hb(c.expect('HB'), s); c.expect('REGISTER')
        assert s.close() == c.close() == 0

    def shared(self):
        s, c, _, _ = self.pair(shared=True)
        # Explicit sends check topic/type isolation. True concurrent calls are
        # exercised in timer() by the independent ClockComponent thread.
        for k in range(100):
            payload = f'shared/{k}'.encode()+bytes(range(128))
            s.send('DATA '+hx(payload)); s.expect('SENT_DATA')
            s.send('HB'); s.expect('SENT_HB')
            assert c.expect('DATA') == 'DATA '+hx(payload)
            self.check_hb(c.expect('HB'), s)
        assert s.close() == c.close() == 0

    def timer(self, shared=True):
        s, c, _, _ = self.pair(shared=shared, timer_ms=1600)
        # Prove the real graph timer is active before overlapping data traffic.
        self.check_hb(c.expect('HB', timeout=2), s)
        # Timer invokes publishHeartbeat from the clock thread concurrently with
        # explicit data writes. Independent per-topic validation catches mixing.
        for k in range(400):
            payload = f'timer/{k}'.encode()+bytes(range(256))
            s.send('DATA '+hx(payload)); s.expect('SENT_DATA')
            assert c.expect('DATA') == 'DATA '+hx(payload)
            time.sleep(0.002)
        timed = c.expect('HB', timeout=2)
        self.check_hb(timed, s)
        time.sleep(1.6)
        # Drain already delivered timer heartbeats, then prove the short
        # furthestPoint stops further publication while process remains alive.
        while True:
            try:
                self.check_hb(c.expect('HB', timeout=0.01), s)
            except PhaseTimeout:
                break
        c.quiet('HB', 0.35)
        c.send('CHECK'); c.expect('DEREGISTER'); c.expect('CHECKED')
        # Expiring the graph timer does not disable the real component. Explicit
        # heartbeat publication restores discovery and the real converter.
        s.send('HB'); s.expect('SENT_HB')
        self.check_hb(c.expect('HB'), s); c.expect('REGISTER')
        c.quiet('HB', 0.25)
        payload = b'after-furthest-point'
        s.send('DATA '+hx(payload)); s.expect('SENT_DATA')
        assert c.expect('DATA') == 'DATA '+hx(payload)
        assert s.close() == c.close() == 0

    def timer_separate(self):
        self.timer(shared=False)

    def slow_reader(self):
        s, c, _, _ = self.pair()
        c.send('BLOCK'); c.expect('BLOCK_ARMED')
        s.send('HB'); s.expect('SENT_HB'); c.expect('BLOCKED_HB')
        # The callback is blocked before the converter accepts this heartbeat.
        # Main-thread CHECK uses the previous acceptance and must expire it.
        time.sleep(1.15)
        c.send('CHECK'); c.expect('DEREGISTER'); c.expect('CHECKED')
        c.send('RELEASE'); c.expect('RELEASED')
        delayed = c.expect('HB')
        self.check_hb(delayed, s); c.expect('REGISTER')
        age = time.time()-int(delayed.split()[4])/1000
        assert age > 1.0, f'fixture did not actually delay beyond TTL: {age}'
        # The actual converter trusts acceptance time, not the old sent time.
        c.send('CHECK'); c.expect('CHECKED'); c.quiet('DEREGISTER', 0.2)
        s.send('HB'); s.expect('SENT_HB'); self.check_hb(c.expect('HB'), s)
        c.quiet('REGISTER', 0.1)
        payload = b'slow-reader-recovered'
        s.send('DATA '+hx(payload)); s.expect('SENT_DATA')
        assert c.expect('DATA') == 'DATA '+hx(payload)
        assert s.close() == c.close() == 0
        return {'delayed_heartbeat_age_seconds': age}

    def risk_first_unlocked(self):
        s, c, _, _ = self.pair(shared=True, risk=True)
        assert s.expect('FIRST_WRITER_LOCK') == 'FIRST_WRITER_LOCK false'
        assert s.expect('LATE_WRITER_LOCK') == 'LATE_WRITER_LOCK true'
        s.send('RISK_BURST 100000')
        observation = []
        try:
            s.expect('RISK_SENT', timeout=10)
        except (AssertionError, PhaseTimeout) as e:
            observation.append(str(e))
        # Explicit observation window drains asynchronous transport output.
        time.sleep(0.3)
        exits = [s.close(), c.close()]
        bad, seen, last = 0, set(), -1
        evidence = []
        delivered = 0
        for record in c.records:
            if record.startswith('BAD_'):
                bad += 1
                if len(evidence) < 10:
                    evidence.append(record)
            if not record.startswith('DATA '):
                continue
            payload = bytes.fromhex(record.split()[1])
            if payload == b'readiness-probe':
                continue
            delivered += 1
            try:
                head, number, tail = payload.split(b'/', 2)
                k = int(number)
                assert head == b'risk' and 0 <= k < 100000
                assert tail == bytes((k*13+b*7)&255 for b in range(512))
                assert k not in seen and k > last
                seen.add(k); last=k
            except (ValueError, AssertionError):
                bad += 1
                if len(evidence) < 10:
                    evidence.append(record[:300])
        observed = bool(bad or any(x != 0 for x in exits))
        return {'status': 'RISK_OBSERVED' if observed else ('RISK_INCONCLUSIVE' if observation or not delivered else 'RISK_NOT_OBSERVED'),
                'category': 'unsupported-concurrent-first-unlocked',
                'invalid_or_reordered': bad, 'delivered': delivered,
                'exit_codes': exits, 'observation_notes': observation, 'first_anomalies': evidence,
                'interpretation': 'No safety claim; later writerLock cannot retrofit the cached sender.'}

    def statistics(self, worker):
        worker.send('STATS')
        worker.expect('STATS_END')
        result = {}
        for line in list(worker.pending):
            if line.startswith('PUB '):
                worker.pending.remove(line)
                fields = line.split()
                result[fields[1]] = [int(x) for x in fields[2:]]
        return result

    def address(self):
        s, c, _, _ = self.pair()
        new_channel = self.channel('data-new')
        s.send('CHANNEL '+new_channel); s.expect('CHANNEL_ADDED')
        s.send('HB'); s.expect('SENT_HB'); self.check_hb(c.expect('HB'), s, channels=2)
        deadline = time.monotonic()+5
        while True:
            s.send('DATA '+hx(b'new-address')); s.expect('SENT_DATA')
            try:
                assert c.expect('DATA', 0.2) == 'DATA '+hx(b'new-address')
                break
            except PhaseTimeout:
                if time.monotonic() >= deadline:
                    raise
        # Broadcast discovery is append-only, so old locator remains subscribed.
        s.send('OLD_DATA '+hx(b'old-still-subscribed')); s.expect('SENT_OLD_DATA')
        assert c.expect('DATA') == 'DATA '+hx(b'old-still-subscribed')
        assert s.close() == c.close() == 0

    def lock(self):
        s, c, hb, data = self.pair()
        s.process.send_signal(signal.SIGSTOP)
        try:
            other = Worker(self, 'service', hb, data, expect_ready=False)
            assert 'another live publisher holds' in other.expect('ERROR')
            assert other.close() == 1
        finally:
            s.process.send_signal(signal.SIGCONT)
        s.send('HB'); s.expect('SENT_HB'); self.check_hb(c.expect('HB'), s)
        # Independent services have independent writer ownership and discovery.
        other_hb, other_data = self.channel('other-hb'), self.channel('other-data')
        other = Worker(self, 'service', other_hb, other_data)
        listener = Worker(self, 'client', other_hb, other_data)
        other.send('HB'); other.expect('SENT_HB'); self.check_hb(listener.expect('HB'), other)
        listener.expect('REGISTER')
        assert other.close() == listener.close() == s.close() == c.close() == 0

    def growth(self):
        s, c, _, _ = self.pair(capacity=1024)
        # Discard the initial hook's size; subsequent sizes correspond exactly to
        # the explicit heartbeat command below. Hook does not alter payload.
        s.expect('WIRE_SIZE')
        low, high = 0, 1024
        exact = None
        while low <= high:
            length = (low+high)//2
            s.send('GROW '+str(length)); s.expect('GROWN')
            s.send('HB'); s.expect('SENT_HB')
            size = int(s.expect('WIRE_SIZE').split()[1])
            if size <= 1024:
                self.check_hb(c.expect('HB'), s)
            if size == 1024:
                exact = length
                break
            if size < 1024:
                low = length+1
            else:
                high = length-1
        assert exact is not None, 'no exact-fill heartbeat found'
        before=self.statistics(s)
        s.send('GROW '+str(exact+1)); s.expect('GROWN')
        s.send('HB'); s.expect('SENT_HB')
        assert int(s.expect('WIRE_SIZE').split()[1]) == 1025
        after=self.statistics(s)
        hb_name = self.prefix+'_hb'
        assert after[hb_name][1] == before[hb_name][1]+1, (before, after)
        assert after[hb_name][0] == before[hb_name][0], (before, after)
        c.quiet('HB')
        s.send('GROW 0'); s.expect('GROWN')
        s.send('HB'); s.expect('SENT_HB'); s.expect('WIRE_SIZE')
        self.check_hb(c.expect('HB'), s)
        assert s.close() == c.close() == 0

    def restart(self):
        s, c, hb, data = self.pair()
        s.send('HB'); s.expect('SENT_HB'); old_id=self.check_hb(c.expect('HB'), s)
        s.process.kill(); assert s.close() == -signal.SIGKILL
        time.sleep(1.1)
        c.send('CHECK'); c.expect('DEREGISTER'); c.expect('CHECKED')
        replacement = Worker(self, 'service', hb, data)
        replacement.send('HB'); replacement.expect('SENT_HB')
        new_id=self.check_hb(c.expect('HB'), replacement)
        assert old_id != new_id
        c.expect('REGISTER')
        replacement.send('DATA '+hx(b'restarted')); replacement.expect('SENT_DATA')
        assert c.expect('DATA') == 'DATA '+hx(b'restarted')
        # New client on a dead writer's residual latest heartbeat must stay quiet.
        assert replacement.close() == c.close() == 0
        late=Worker(self, 'client', hb, data)
        # READY alone precedes mapping. With every previous reader joined and
        # destroyed, this real futex registration proves late's reader mapped
        # the residual segment and selected its join index before quiet().
        segment=hb.split('::::',1)[1].split('[',1)[0]
        deadline=time.monotonic()+5
        while True:
            header=Path('/dev/shm', segment).read_bytes()[:516]
            # Source contract: ShmSeqlockHeader.waiters starts at 4*128.
            if len(header) >= 516 and int.from_bytes(header[512:516], sys.byteorder) > 0:
                break
            if time.monotonic() >= deadline:
                raise PhaseTimeout('late reader did not map/register futex')
            time.sleep(0.005)
        late.quiet('HB', 0.3)
        assert late.close() == 0


CASES = ['direct', 'ttl', 'shared', 'timer', 'growth', 'restart', 'address', 'lock', 'timer_separate', 'slow_reader', 'risk_first_unlocked']

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary')
    p.add_argument('--output', type=Path, default=Path('/tmp/shm-heartbeat-system'))
    p.add_argument('--case', choices=CASES, action='append')
    p.add_argument('--layout', choices=['inline', 'split', 'inline-premark'], action='append')
    args=p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output/'invocation.json').write_text(json.dumps(vars(args),default=str,indent=2)+'\n')
    results=[]
    for layout in args.layout or ['inline', 'split', 'inline-premark']:
        for case in args.case or CASES:
            output=args.output / f'{layout}-{case}'
            output.mkdir(parents=True, exist_ok=True)
            suite=Suite(str(Path(args.binary).resolve()), output, layout)
            result={'case': case, 'layout': layout, 'status': 'PASS'}
            start=time.monotonic()
            timed_out=[]
            def watchdog():
                timed_out.append(True)
                snapshots=[]
                for child in suite.children:
                    if child.process.poll() is None:
                        proc=Path('/proc')/str(child.process.pid)
                        snapshot={'pid':child.process.pid}
                        for name in ['status', 'wchan', 'stack']:
                            try:
                                snapshot[name]=(proc/name).read_text()
                            except OSError as e:
                                snapshot[name]=str(e)
                        snapshots.append(snapshot)
                        child.process.kill()
                (output/'watchdog.json').write_text(json.dumps(snapshots,indent=2)+'\n')
            timer=threading.Timer(30, watchdog)
            timer.start()
            try:
                details = getattr(suite, case)()
                if details:
                    result.update(details)
            except Exception as e:
                result.update(status='FAIL', error=str(e))
            finally:
                timer.cancel()
                timer.join()
                suite.cleanup()
            if timed_out:
                result.update(status='FAIL', error='30 second case watchdog expired')
            result['seconds']=time.monotonic()-start
            result['children']=[{'pid': c.process.pid, 'exit': c.process.returncode} for c in suite.children]
            print(json.dumps(result), flush=True)
            results.append(result)
    (args.output/'results.json').write_text(json.dumps({'results':results}, indent=2)+'\n')
    return int(any(r['status']=='FAIL' for r in results))

if __name__ == '__main__':
    raise SystemExit(main())
