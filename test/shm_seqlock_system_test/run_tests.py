#!/usr/bin/env python3
"""Deterministic Linux system tests. No data-plane ACK is used by performance runs.
Every child, shm object and lock is owned and registered by one case context.
"""
import argparse
import collections
import hashlib
import itertools
import json
import os
from pathlib import Path
import queue
import random
import signal
import subprocess
import threading
import time
import traceback
import uuid


def wire(data):
    return data.hex() if data else '-'


def message(seq, size=256, topic_size=7, generation=1):
    # Full deterministic bytes; small and empty messages do not need a header.
    key = f'{generation}:{seq}:'.encode()
    def fill(tag, n):
        return (hashlib.sha256(key + tag).digest() * ((n + 31) // 32))[:n]
    return fill(b'topic', topic_size), fill(b'payload', size)


class Oracle:
    """Compare every byte, including binary topic and small payloads."""
    def __init__(self, expected):
        self.expected = list(expected)
        self.index = 0

    def observe(self, pair):
        if self.index >= len(self.expected) or pair != self.expected[self.index]:
            raise AssertionError(f'payload/topic/order mismatch at {self.index}: {pair!r}')
        self.index += 1

    def finish(self):
        assert self.index == len(self.expected), (self.index, len(self.expected))


class Child:
    def __init__(self, case, role, props='', probe='', occurrence=1,
                 filter_='all', hook='none', segment=None, launcher=None):
        self.case = case
        self.lines = queue.Queue()
        self.pending = collections.deque()
        self.history = []
        self.data_consumed = 0
        self.expected_failure = False
        self.killed_intentionally = False
        child_control, self.control = os.pipe()
        self.name = f'{len(case.children):02d}-{role}'
        self.log = open(case.path / (self.name + '.stdout'), 'w', buffering=1)
        self.err = open(case.path / (self.name + '.stderr'), 'w')
        binary = case.args.blackbox_binary if case.name.startswith('R-blackbox-') else case.args.binary
        self.args = list(launcher or []) + [binary, role, case.segment if segment is None else segment,
                                            props, probe, str(child_control), str(occurrence), filter_, hook]
        try:
            self.proc = subprocess.Popen(self.args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                         stderr=self.err, text=True, bufsize=1,
                                         pass_fds=(child_control,), start_new_session=True)
        finally:
            os.close(child_control)
        case.children.append(self)
        def pump():
            for line in self.proc.stdout:
                line = line.rstrip('\n')
                self.log.write(f'{time.monotonic():.9f} {line}\n')
                self.history.append(line)
                self.lines.put(line)
            self.lines.put(None)
        self.thread = threading.Thread(target=pump, daemon=True)
        self.thread.start()

    def send(self, cmd):
        self.log.write(f'{time.monotonic():.9f} > {cmd}\n')
        self.proc.stdin.write(cmd + '\n')
        self.proc.stdin.flush()

    def wait(self, prefix, timeout=5):
        until = time.monotonic() + timeout
        # Preserve unmatched events (READY and JOINED can arrive in either order).
        for i, line in enumerate(self.pending):
            if line.startswith(prefix):
                del self.pending[i]
                if prefix == 'ERROR': self.expected_failure = True
                return line
        while True:
            try:
                line = self.lines.get(timeout=max(0, until - time.monotonic()))
            except queue.Empty:
                self.snapshot('timeout')
                raise TimeoutError(f'{self.name} waiting for {prefix}; last={self.history[-6:]}')
            if line is None:
                raise AssertionError(f'{self.name} exited {self.proc.poll()} waiting {prefix}: {self.history[-6:]}')
            if line.startswith('ERROR') and not prefix.startswith('ERROR'):
                raise AssertionError(line)
            if line.startswith(prefix):
                if prefix == 'ERROR': self.expected_failure = True
                return line
            self.pending.append(line)

    def snapshot(self, reason):
        root = Path('/proc') / str(self.proc.pid)
        with open(self.case.path / (self.name + '.' + reason), 'w') as out:
            for name in ('status', 'stat', 'wchan', 'maps'):
                try:
                    out.write(f'\n{name}\n' + (root / name).read_text())
                except OSError as exc:
                    out.write(str(exc))
            try:
                for task in (root / 'task').iterdir():
                    for name in ('wchan', 'stack'):
                        try:
                            out.write(f'\n{task.name}/{name}\n' + (task / name).read_text())
                        except OSError as exc:
                            out.write(str(exc))
            except OSError:
                pass

    def data(self, timeout=5):
        _, topic, payload = self.wait('DATA ', timeout).split()
        self.data_consumed += 1
        return tuple(bytes.fromhex(x) if x != '-' else b'' for x in (topic, payload))

    def stats(self):
        self.send('STATS')
        line = self.wait('STATS ')
        self.wait('STATS_END')
        return [int(v) for v in line.split()[1:]]

    def publish(self, pair):
        self.send('SEND ' + ' '.join(wire(x) for x in pair))
        self.wait('SENT')

    def release(self):
        os.write(self.control, b'c')

    def kill(self):
        if self.proc.poll() is None:
            self.killed_intentionally = True
            os.killpg(self.proc.pid, signal.SIGKILL)
        self.proc.wait(timeout=5)
        self.thread.join(timeout=2)

    def stop(self):
        if self.proc.poll() is None:
            self.send('QUIT')
            try:
                rc = self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.snapshot('shutdown-timeout')
                self.kill()
                raise AssertionError('shutdown timeout')
            assert rc == 0, (self.name, rc)
        self.thread.join(timeout=2)
        assert self.proc.returncode == 0 or self.killed_intentionally or self.expected_failure, (self.name, self.proc.returncode)
        if self.proc.returncode == 0:
            actual = sum(line.startswith('DATA ') for line in self.history)
            assert actual == self.data_consumed, ('unexpected trailing delivery', self.name, actual, self.data_consumed)

    def close(self):
        self.kill()
        os.close(self.control)
        self.proc.stdin.close()
        self.proc.stdout.close()
        self.log.close()
        self.err.close()

    def retire(self):
        # Stop a long-lived case's short-lived child and release its fds now, so
        # process-churn cases do not run out of descriptors before cleanup.
        self.stop()
        self.close()
        self.case.children.remove(self)


class Case:
    def __init__(self, args, name):
        self.args = args
        self.name = name
        self.segment = 'shmsys_' + uuid.uuid4().hex
        self.path = Path(args.log_dir) / name
        self.path.mkdir(parents=True, exist_ok=True)
        self.children = []
        self.segments = {self.segment}

    def __enter__(self):
        return self

    def __exit__(self, typ, value, tb):
        # Kill and reap every owned child before unlinking the lock inode.
        exits = []
        for c in self.children:
            if typ:
                c.snapshot('failure')
            c.close()
            exits.append({'argv': c.args, 'pid': c.proc.pid, 'returncode': c.proc.returncode})
        (self.path / 'children.json').write_text(json.dumps(exits, indent=2))
        for seg in self.segments:
            for path in (Path('/dev/shm') / seg, Path('/dev/shm') / (seg + '.lock')):
                path.unlink(missing_ok=True)

    def child(self, role, **kw):
        return Child(self, role, **kw)


def props(args, writer=True, **extra):
    values = dict(mlock='false', layout=args.layout, slotSize=512, slotCount=64,
                  wait=args.writer_wait if writer else args.reader_wait)
    if not writer:
        values.update(read=args.read, spinCount=1, futexTimeoutUs=10000, pauseAfter=1, join='next')
    values.update(extra)
    return ','.join(f'{k}={v}' for k, v in values.items())


def writer(c, **kw):
    w = c.child('writer', props=kw.pop('props', props(c.args)), **kw)
    w.wait('READY')
    return w


def reader(c, **kw):
    r = c.child('reader', props=kw.pop('props', props(c.args, False)), **kw)
    r.wait('READY')
    r.wait('JOINED')
    return r


def receive(r, expected):
    oracle = Oracle(expected)
    for _ in expected:
        oracle.observe(r.data())
    oracle.finish()


def checker(c):
    samples = [message(i, n, t) for i, (n, t) in enumerate(itertools.product((0, 1, 40, 320, 503), (0, 1, 7)))]
    receive_count = 0
    for pair in samples:
        for part in (0, 1):
            if pair[part]:
                bad = list(pair)
                data = bytearray(bad[part]); data[-1] ^= 1; bad[part] = bytes(data)
                try:
                    Oracle([pair]).observe(tuple(bad))
                except AssertionError:
                    receive_count += 1
                else:
                    raise AssertionError('checker missed deterministic corruption')
    assert receive_count > 10
    for expected, actual in ((samples, samples[::-1]), (samples[:1], samples[:1]*2)):
        oracle = Oracle(expected)
        try:
            for pair in actual:
                oracle.observe(pair)
        except AssertionError:
            pass
        else:
            raise AssertionError('checker missed reorder/duplicate')
    c.metrics = {'deterministic_faults_detected': receive_count + 2}


def bytes_case(c):
    # Reader-first proves API-return is distinct from mapping and joining.
    r0 = c.child('reader', props=props(c.args, False))
    r0.wait('READY')
    w = writer(c)
    r0.wait('JOINED')
    r1 = reader(c)
    sizes = (0, 1, 39, 40, 41, 63, 64, 65, 319, 320, 321)
    samples = [message(i, n, t) for i, (n, t) in enumerate(itertools.product(sizes, (0, 1, 7, 65)))]
    samples += [message(100+i, 512-t, t) for i, t in enumerate((0, 1, 7, 65, 512))]
    for pair in samples:
        w.publish(pair)
        receive(r0, [pair]); receive(r1, [pair])
    assert w.stats()[:2] == [len(samples), 0]
    for r in (r0, r1):
        assert r.stats()[:3] == [len(samples), 0, 0]
        r.stop()
    w.stop()


def rejected(c):
    w = writer(c)
    r = reader(c)
    good = message(1, 505)
    for _ in range(10):
        w.publish(message(0, 506))
        w.publish(good)
        receive(r, [good])
    assert w.stats()[:2] == [10, 10]
    assert r.stats()[:3] == [10, 0, 0]
    r.stop(); w.stop()


def filters_hooks(c):
    w = writer(c, hook='wrap')
    all_ = reader(c, hook='unwrap')
    exact = reader(c, filter_='exact:' + wire(b'a.x'), hook='unwrap')
    regex = reader(c, filter_='regex:a\\..*', hook='unwrap')
    for seq, t in enumerate((b'a.x', b'b.x', b'a.y', b'a.x\0', b'a.x')):
        pair = (t, message(seq)[1])
        w.publish(pair); receive(all_, [pair])
        if t == b'a.x': receive(exact, [pair])
        if t.startswith(b'a.'): receive(regex, [pair])
    # hook expansion crosses capacity; accepted value after rejection still works.
    w.publish((b'a.x', b'z'*509))
    pair = (b'a.x', b'')
    w.publish(pair)
    for r in (all_, exact, regex): receive(r, [pair])
    assert w.stats()[:2] == [6, 1]
    for r in (all_, exact, regex):
        assert r.stats()[:2] == [6, 0]  # filtering is after transport accounting
        r.stop()
    w.stop()
    # Incoming nullopt must drop, then resume on a subsequent valid value.
    w = writer(c)
    r = reader(c, hook='unwrap')
    w.publish((b'a', b'bad')); w.publish((b'a', b'!good'))
    receive(r, [(b'a', b'good')])
    assert r.stats()[0] == 2  # reader accounting starts at the newly joined index
    r.stop(); w.stop()


def first_properties(c):
    w = writer(c)
    # Changing sender properties later cannot resize or relayout the existing sender.
    w.send('ALIAS ' + props(c.args, slotSize=1, layout='unknown', writerLock='true'))
    w.wait('ACQUIRED')
    r = reader(c, props=props(c.args, False, layout='invalid', slotCount=3, slotSize=1))
    pair = message(1, 505)
    w.publish(pair); receive(r, [pair])
    assert w.stats()[:2] == [1, 0]
    w.send('COMPETE ' + props(c.args))
    assert 'another live publisher holds' in w.wait('REJECTED')
    r.stop(); w.stop()


def parameters(c):
    invalid = ['slotCount=0', 'slotCount=1', 'slotCount=3', 'slotCount=-1',
               'slotCount=16777217', 'slotSize=0', 'slotSize=-1',
               'slotSize=67108865', 'slotSize=bad', 'layout=unknown']
    for name in ('', 'invalid/name'):
        bad = c.child('writer', props='mlock=false', segment=name)
        assert 'invalid segment name' in bad.wait('ERROR')
        assert bad.proc.wait(timeout=5) == 1
    w = c.child('idle', props='mlock=false')
    w.wait('READY')
    for prop in invalid:
        w.send('TRY mlock=false,' + prop)
        w.wait('REJECTED')
    # Existing queryNumber uses stoull without checking the end position.
    # These are characterization tests of the existing permissive parser behavior.
    w.send('TRY mlock=false,slotCount=64suffix,slotSize=512suffix,unknown=ignored')
    w.wait('ACQUIRED')
    r = reader(c)
    pair = message(1)
    w.publish(pair); receive(r, [pair])
    r.stop(); w.stop()


def joins(c):
    w = writer(c)
    pair = message(0)
    w.publish(pair)
    latest = reader(c, props=props(c.args, False, join='latest'))
    receive(latest, [pair])
    next_ = reader(c)
    fresh = message(1)
    w.publish(fresh)
    receive(latest, [fresh]); receive(next_, [fresh])
    assert latest.stats()[0] == 2 and next_.stats()[0] == 1
    latest.stop(); next_.stop(); w.stop()
    stale = reader(c, props=props(c.args, False, join='latest'))
    # Map the dead segment first; replacement continues indices without replay.
    w = writer(c)
    fresh = message(2, generation=2)
    w.publish(fresh); receive(stale, [fresh])
    assert stale.stats()[0] == 1
    stale.stop(); w.stop()


def lap(c):
    n = 4
    for advance in (n-1, n, n+1, 2*n+1):
        # each interval has its own segment (and accounting origin)
        c.segment += 'x'
        c.segments.add(c.segment)
        w = writer(c, props=props(c.args, slotCount=n))
        r = reader(c, props=props(c.args, False, slotCount=n), probe='callback')
        initial = message(0)
        w.publish(initial); receive(r, [initial]); r.wait('HIT callback')
        batch = [message(i) for i in range(1, advance+1)]
        for pair in batch: w.publish(pair)
        r.release()
        capacity = n if c.args.layout == 'inline' else n-1
        expected = batch if advance <= capacity else batch[-1:]
        receive(r, expected)
        stats = r.stats()
        lost = len(batch)-len(expected)
        assert stats[:3] == [1+len(expected), lost, int(lost > 0)], stats
        r.stop(); w.stop()


def crash(c):
    event = c.args.event
    early = event in ('writer_locked', 'segment_created', 'before_magic')
    recovery = event in ('recovery_scan', 'recovery_epoch', 'superseded')
    if recovery:
        old_layout = 'split' if event == 'superseded' and c.args.layout != 'split' else 'inline'
        initial = writer(c, props=props(c.args, layout=old_layout if event == 'superseded' else c.args.layout))
        initial.publish(message(99, generation=99))
        initial.stop()
    r = c.child('reader', props=props(c.args, False))
    r.wait('READY')
    if recovery: r.wait('JOINED')
    w = c.child('writer', props=props(c.args), probe=event)
    if not early and not recovery:
        w.wait('READY'); r.wait('JOINED')
        w.send('SEND ' + ' '.join(wire(x) for x in message(0, 450, 61)))
    w.wait('HIT ' + event)
    w.kill()
    assert w.proc.returncode == -signal.SIGKILL
    replacement = writer(c)
    if early: r.wait('JOINED')
    if event == 'superseded': r.wait('JOINED')
    # Pre-commit payload must never appear. Post-commit delivery can race kill.
    committed = event in ('slot_even', 'published', 'premarked')
    expected_old = message(0, 450, 61)
    pairs = [message(1, generation=2), message(2, generation=2)]
    replacement.publish(pairs[0])
    first = r.data()
    if first == expected_old:
        assert committed, ('uncommitted data delivered', event)
        first = r.data()
    assert first == pairs[0], ('recovery first', event, first)
    replacement.publish(pairs[1]); receive(r, [pairs[1]])
    assert r.stats()[4] == 0
    r.stop(); replacement.stop()


def transition(c):
    w = writer(c)
    r = reader(c)
    w.publish(message(1)); receive(r, [message(1)])
    w.kill()
    replacement = writer(c, props=props(c.args, layout=c.args.to_layout, slotSize=640, slotCount=4, stride128='true'))
    r.wait('JOINED')
    replacement.publish(message(2, generation=2)); receive(r, [message(2, generation=2)])
    stats = r.stats()
    assert stats[3] >= 1 and stats[5] >= 1, stats
    r.stop(); replacement.stop()


def futex_interleave(c):
    w = writer(c, props=props(c.args, wait='futex'))
    r = reader(c, props=props(c.args, False, wait='futex', spinCount=0), probe=c.args.event)
    r.wait('HIT ' + c.args.event)
    pair = message(1)
    w.publish(pair)
    r.release(); receive(r, [pair])
    assert r.stats()[1] == 0
    r.stop(); w.stop()


def waiter_leak(c):
    w = writer(c, props=props(c.args, wait='futex'))
    r = reader(c, props=props(c.args, False, wait='futex'), probe='before_sleep')
    r.wait('HIT before_sleep'); r.kill()
    for i in range(65): w.publish(message(i))
    assert w.stats()[3] >= 1
    r = reader(c, props=props(c.args, False, wait='futex'))
    pair = message(66)
    w.publish(pair); receive(r, [pair])
    r.stop(); w.stop()


def writer_conflict(c):
    w = writer(c)
    competitors = [c.child('writer', props=props(c.args)) for _ in range(8)]
    for rival in competitors:
        assert 'another live publisher holds' in rival.wait('ERROR')
        assert rival.proc.wait(timeout=5) == 1
    os.kill(w.proc.pid, signal.SIGSTOP)
    rival = c.child('writer', props=props(c.args))
    assert 'another live publisher holds' in rival.wait('ERROR')
    assert rival.proc.wait(timeout=5) == 1
    w.kill()
    takeover = writer(c)
    r = reader(c)
    pair = message(1)
    takeover.publish(pair); receive(r, [pair])
    r.stop(); takeover.stop()


def blackbox(c):
    # No probes in either the writer or reader component. The first delivered
    # marker is the readiness barrier. Random kill delay is fault injection,
    # never a substitute for reader readiness.
    w = writer(c)
    marker = message(0, generation=100)
    w.publish(marker)
    r = c.child('reader', props=props(c.args, False, join='latest'))
    r.wait('READY'); receive(r, [marker])
    rng = random.Random(c.args.seed)
    for gen in range(1, 6):
        stable = [message(i, 450, 61, generation=gen) for i in range(1, 21)]
        attempted = message(21, 450, 61, generation=gen)
        for pair in stable: w.publish(pair)
        w.send('SEND ' + ' '.join(wire(x) for x in attempted))
        time.sleep(rng.random()/10000)
        w.kill()
        w = writer(c)
        marker = message(0, generation=100+gen)
        w.publish(marker)
        # Ring is larger than the interval, so all completed stable writes
        # must arrive. Only the interrupted publication is optional.
        receive(r, stable)
        pair = r.data()
        if pair == attempted: pair = r.data()
        assert pair == marker, ('blackbox recovery', gen)
    r.stop(); w.stop()


def dirty(c):
    for data in (b'', b'\0'*17, b'\0'*640, b'\xff'*640):
        (Path('/dev/shm') / c.segment).write_bytes(data)
        r = c.child('reader', props=props(c.args, False))
        r.wait('READY')
        w = writer(c)
        r.wait('JOINED')
        pair = message(len(data))
        w.publish(pair); receive(r, [pair])
        assert r.stats()[4] == 0
        r.stop(); w.stop()


def parse_fields(line):
    return dict(item.split('=', 1) for item in line.split()[1:])


def query_stress(c):
    # F10: stats and thread-handle queries run while a subscription is added,
    # used and removed on the same component.
    r = c.child('query_stress', props=props(c.args, False, stressThreads=4, stressCycles=100))
    r.wait('READY')
    r.send('RUN')
    fields = parse_fields(r.wait('RESULT', timeout=60))
    assert int(fields['cycles']) == 100, fields
    assert int(fields['queries']) > 0, fields
    assert int(fields['handles']) == 0, fields
    r.wait('RUN_DONE')
    r.stop()


def cpu_provider(c):
    # F11: missing provider, successful bind propagation, duplicate install and
    # a provider that throws; each failure must leave the component usable.
    for mode, expected in (
        ('noprovider', {'cpu_threw': 1, 'provider_message': 1, 'fallback_ok': 1}),
        ('provider', {'provider_calls': 1, 'spec_ok': 1, 'who_has_reader': 1, 'dup_threw': 0}),
        ('dup', {'provider_calls': 1, 'spec_ok': 1, 'who_has_reader': 1, 'dup_threw': 1}),
        ('failure', {'bind_threw': 1, 'fallback_ok': 1}),
    ):
        r = c.child('cpu_provider', props=props(c.args, False), filter_=mode)
        r.wait('READY')
        r.send('RUN')
        fields = parse_fields(r.wait('RESULT', timeout=30))
        for key, value in expected.items():
            assert int(fields[key]) == value, (mode, key, fields)
        r.wait('RUN_DONE')
        r.stop()


def remove_stress(c):
    # R05: all clients remove at once, including duplicate and unknown ids; the
    # last-client stop races with the other removals.
    r = c.child('remove_stress', props=props(c.args, False, stressThreads=4, stressCycles=20))
    r.wait('READY')
    r.send('RUN')
    fields = parse_fields(r.wait('RESULT', timeout=120))
    assert int(fields['cycles']) == 20 and int(fields['threads']) == 4, fields
    assert int(fields['handles']) == 0, fields
    r.wait('RUN_DONE')
    r.stop()


def callback_control(c):
    # R06: remove another client, then remove self and subscribe again.
    w = writer(c)
    r = c.child('callback_control', props=props(c.args, False, join='next'), filter_='remove_other')
    r.wait('READY')
    for i in range(5):
        w.publish(message(i, generation=50 + i))
        r.wait('ADATA')
    r.wait('REMOVED_OTHER')
    r.send('REPORT')
    fields = parse_fields(r.wait('REPORT'))
    assert int(fields['a']) == 5, fields
    assert int(fields['b']) <= 1, fields
    assert int(fields['handles']) == 1, fields
    r.stop(); w.stop()
    w = writer(c)
    r = c.child('callback_control', props=props(c.args, False, join='next'), filter_='resubscribe')
    r.wait('READY')
    w.publish(message(0, generation=60))
    r.wait('RESUBSCRIBED')
    for i in range(3):
        w.publish(message(i + 1, generation=60 + i))
        r.wait('CDATA')
    r.send('REPORT')
    fields = parse_fields(r.wait('REPORT'))
    assert int(fields['a']) == 1, fields
    assert int(fields['c']) == 3, fields
    assert int(fields['handles']) == 1, fields
    r.stop(); w.stop()


def readd(c):
    # R07: last subscriber leaves and is added again with different properties;
    # duplicate and unknown removal stay safe.
    w = writer(c)
    r = c.child('reader', props=props(c.args, False))
    r.wait('READY'); r.wait('JOINED')
    pair = message(0)
    w.publish(pair); receive(r, [pair])
    r.send('REMOVE'); r.wait('REMOVED')
    r.send('REMOVE'); r.wait('REMOVED')
    new_props = props(c.args, False, layout='split', wait='spin', read='spec')
    r.send('ADDP ' + new_props)
    r.wait('ADDED'); r.wait('JOINED')
    pair = message(1)
    w.publish(pair); receive(r, [pair])
    r.send('STATS'); r.wait('STATS '); r.wait('STATS_END')
    r.stop(); w.stop()


def proc_reconnect(c):
    # R08 (scaled): repeated reader process start/receive/quit and periodic
    # writer SIGKILL/takeover. The full 1000-cycle variant is an explicit run.
    w = writer(c)
    cycles = int(os.environ.get('SHM_R08_CYCLES', '200'))
    for i in range(cycles):
        if i and i % 50 == 0:
            w.kill()
            w = writer(c)
        r = c.child('reader', props=props(c.args, False, join='next'))
        r.wait('READY'); r.wait('JOINED')
        pair = message(i, generation=1000 + i)
        w.publish(pair); receive(r, [pair])
        r.retire()
    w.stop()


SHM_SEQLOCK_MAGIC = 0x314B434F4C514553
SEGMENT_HEADER = 640
SEGMENT_SLOT_COUNT = 64
SEGMENT_SLOT_CAPACITY = 512
SEGMENT_SLOT_STRIDE = 576


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def crafted_props():
    # Fixed wire layout matching craft_segment below; join=next so the reader
    # starts at header.published instead of treating the segment as stale.
    return 'mlock=false,layout=inline,slotSize=512,slotCount=64,wait=spin,read=classic,spinCount=1,pauseAfter=1,join=next'


def craft_segment(segment, seq=2, published=0, payload_len=5, topic_len=1, topic_hash=None, data=b'thello'):
    import struct
    buf = bytearray(SEGMENT_HEADER + SEGMENT_SLOT_STRIDE * SEGMENT_SLOT_COUNT)
    struct.pack_into('<QIIIII', buf, 0, SHM_SEQLOCK_MAGIC, 1, SEGMENT_SLOT_CAPACITY,
                     SEGMENT_SLOT_COUNT, SEGMENT_SLOT_STRIDE, 0)
    struct.pack_into('<Q', buf, 128, 1)   # epoch
    struct.pack_into('<Q', buf, 256, published)
    struct.pack_into('<I', buf, 384, 0)   # futexWord
    struct.pack_into('<I', buf, 512, 0)   # waiters
    struct.pack_into('<QIIII', buf, SEGMENT_HEADER, seq, payload_len, topic_len,
                     fnv1a(b't') if topic_hash is None else topic_hash, 0)
    start = SEGMENT_HEADER + 24
    buf[start:start+len(data)] = data
    (Path('/dev/shm') / segment).write_bytes(buf)


def dirty_ext(c):
    # R-dirty-ext: structurally valid headers with one impossible/abnormal field.
    # A reader must not crash or hang; only the valid crafted message may be
    # delivered through the exact-topic filter.
    variants = [
        ('valid', dict(seq=2, published=0, payload_len=5, topic_len=1), True),
        ('odd', dict(seq=1, published=0, payload_len=5, topic_len=1), False),
        ('overlong', dict(seq=2, published=0, payload_len=0xffffffff, topic_len=0xffffffff), False),
        ('badhash', dict(seq=2, published=0, payload_len=5, topic_len=1, topic_hash=0xdeadbeef), False),
        ('published_huge', dict(seq=2, published=1 << 63, payload_len=5, topic_len=1), False),
    ]
    for name, options, expect in variants:
        for path in (Path('/dev/shm') / c.segment, Path('/dev/shm') / (c.segment + '.lock')):
            path.unlink(missing_ok=True)
        craft_segment(c.segment, **options)
        r = c.child('reader', props=crafted_props(), filter_='exact:' + b't'.hex())
        r.wait('READY'); r.wait('JOINED')
        if expect:
            topic, payload = r.data()
            assert (topic, payload) == (b't', b'hello'), (name, topic, payload)
        else:
            r.stats()  # the reader is still responsive
            delivered = [line for line in r.pending if line.startswith('DATA ')]
            if name == 'published_huge':
                # published beyond the real ring wraps 2*next+2 and can select
                # slot 0. Record the observation instead of calling it a pass.
                while delivered:
                    r.data()
                    delivered = [line for line in r.pending if line.startswith('DATA ')]
                if r.data_consumed:
                    print(f'RISK published_huge_delivered={r.data_consumed}', flush=True)
            else:
                assert not delivered, (name, delivered)
        r.stop()
    # After every abnormal image, a fresh segment and writer must work normally.
    for path in (Path('/dev/shm') / c.segment, Path('/dev/shm') / (c.segment + '.lock')):
        path.unlink(missing_ok=True)
    w = writer(c)
    r = c.child('reader', props=props(c.args, False))
    r.wait('READY'); r.wait('JOINED')
    pair = message(0)
    w.publish(pair); receive(r, [pair])
    r.stop(); w.stop()


def wait_exit(child, seconds=2.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if child.proc.poll() is not None:
            return True
        time.sleep(0.01)
    return child.proc.poll() is not None


def risk_cases(c):
    # R-risk: behaviours the component does not promise. Each probe records what
    # happened; an observation is never a safety guarantee.
    w = writer(c)
    r = c.child('throwing_reader', props=props(c.args, False))
    r.expected_failure = True
    r.wait('READY'); r.wait('JOINED')
    w.publish(message(0))
    died = wait_exit(r, 2.0)
    print(f'RISK callback_throw_died={int(died)} exit={r.proc.poll()}', flush=True)
    r.kill(); w.stop()

    d = c.child('dangling_publisher')
    d.expected_failure = True
    d.wait('READY')
    d.send('SEND ' + ' '.join(wire(x) for x in message(0)))
    died = wait_exit(d, 2.0)
    print(f'RISK publisher_outlives_component_died={int(died)} exit={d.proc.poll()}', flush=True)
    d.kill()

    w = writer(c)
    r = c.child('reader', props=props(c.args, False))
    w.expected_failure = True
    r.expected_failure = True
    r.wait('READY'); r.wait('JOINED')
    pair = message(0)
    w.publish(pair); receive(r, [pair])
    os.truncate('/dev/shm/' + c.segment, 0)
    try:
        w.send('SEND ' + ' '.join(wire(x) for x in message(1)))
    except BrokenPipeError:
        pass
    writer_died = wait_exit(w, 2.0)
    reader_died = wait_exit(r, 2.0)
    print(f'RISK truncate_active_mapping_writer_died={int(writer_died)} reader_died={int(reader_died)}', flush=True)
    r.kill(); w.kill()

    w = writer(c)
    lock = Path('/dev/shm') / (c.segment + '.lock')
    lock.unlink(missing_ok=True)
    lock.write_bytes(b'\0' * 128)
    w2 = c.child('writer', props=props(c.args))
    w2.expected_failure = True
    acquired = True
    try:
        w2.wait('READY', timeout=3)
    except (TimeoutError, AssertionError):
        acquired = False
    print(f'RISK lock_file_replaced_second_writer_ready={int(acquired)}', flush=True)
    w2.kill(); w.stop()


def resource_limits(c):
    # R-resource: low RLIMIT_NOFILE must fail cleanly, and mlock failure is
    # documented as best-effort (the component must still work).
    if 'asan' in str(c.args.binary):
        # ASan's runtime needs descriptors of its own before main(), so a
        # five-descriptor limit cannot reach the component's clean error path.
        print('SKIP fd_limit_probe_under_sanitizer', flush=True)
    else:
        r = c.child('writer', props=props(c.args), launcher=['prlimit', '--nofile=5:5', '--'])
        line = r.wait('ERROR', timeout=10)
        assert 'Too many open files' in line, line
        print('RISK fd_limit_writer_clean_error=1', flush=True)
    launcher = ['prlimit', '--memlock=0:0', '--']
    w = c.child('writer', props=props(c.args, mlock='true'), launcher=launcher)
    w.wait('READY')
    r = c.child('reader', props=props(c.args, False, mlock='true'), launcher=launcher)
    r.wait('READY'); r.wait('JOINED')
    pair = message(0)
    w.publish(pair); receive(r, [pair])
    print('RISK mlock_unavailable_best_effort_ok=1', flush=True)
    r.stop(); w.stop()


def slow_shared(c):
    # R03: the two callbacks share the segment's reader thread. The blocked slow
    # callback must also hold back the fast one (no false isolation).
    w = writer(c)
    r = c.child('multi_callback', props=props(c.args, False, join='next'))
    r.wait('READY')
    w.publish(message(0, generation=70))
    r.wait('SLOW_IN')
    for i in range(1, 3):
        w.publish(message(i, generation=70 + i))
    r.send('REPORT')
    before = parse_fields(r.wait('REPORT'))
    assert int(before['fast']) == 0, before
    r.send('RELEASE')
    r.wait('SLOW_OUT')
    for _ in range(3):
        r.wait('FAST')
    r.send('REPORT')
    after = parse_fields(r.wait('REPORT'))
    assert int(after['fast']) == 3, after
    assert int(after['slow']) == 3, after
    r.stop(); w.stop()


NARROW_CASES = {'F10', 'F11', 'R05', 'R06', 'R07', 'R08', 'R-dirty-ext', 'R-risk', 'R03', 'R-resource'}

CASES = {'C00': checker, 'F01-F02': bytes_case, 'F03': rejected, 'F04-F05': filters_hooks,
         'F07': joins, 'F08': first_properties, 'F09': parameters, 'F10': query_stress,
         'F11': cpu_provider, 'R01': lap, 'R04-crash': crash, 'R04-transition': transition,
         'R03': slow_shared, 'R05': remove_stress, 'R06': callback_control, 'R07': readd, 'R08': proc_reconnect,
         'R-futex': futex_interleave, 'R-waiter-leak': waiter_leak,
         'R-writer-conflict': writer_conflict, 'R-blackbox': blackbox, 'R-dirty': dirty,
         'R-dirty-ext': dirty_ext, 'R-risk': risk_cases, 'R-resource': resource_limits}


def execute(args, name, fn):
    start = time.monotonic()
    result = dict(name=name, seed=args.seed, layout=args.layout, read=args.read,
                  writer_wait=args.writer_wait, reader_wait=args.reader_wait, event=args.event)
    try:
        with Case(args, name) as c:
            # Separate watchdog can terminate stuck C++ workers even if main stalls.
            def expired():
                for child in c.children:
                    if child.proc.poll() is None:
                        child.snapshot('watchdog'); os.killpg(child.proc.pid, signal.SIGKILL)
            timer = threading.Timer(args.timeout, expired); timer.daemon = True; timer.start()
            try:
                fn(c)
                # Explicit orderly shutdown, then ownership cleanup in __exit__.
                for child in c.children:
                    child.stop()
                result.update(status='PASS', metrics=getattr(c, 'metrics', {}))
            finally:
                timer.cancel()
    except Exception as exc:
        result.update(status='FAIL', error=str(exc), traceback=traceback.format_exc())
    result['elapsed_seconds'] = time.monotonic()-start
    (Path(args.log_dir) / name / 'result.json').write_text(json.dumps(result, indent=2))
    print(f"{result['status']} {name}: {result.get('error', '')}", flush=True)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', required=True)
    p.add_argument('--blackbox-binary', help='Worker compiled without fault probes; defaults to the sibling binary')
    p.add_argument('--case', choices=['all']+list(CASES), default='all')
    p.add_argument('--layout', choices=['inline', 'split', 'inline-premark'], default='inline')
    p.add_argument('--read', choices=['classic', 'spec'], default='classic')
    p.add_argument('--writer-wait', choices=['spin', 'futex'], default='spin')
    p.add_argument('--reader-wait', choices=['spin', 'futex'], default='spin')
    p.add_argument('--event', default='slot_even')
    p.add_argument('--to-layout', default='split')
    p.add_argument('--matrix', action='store_true', help='24 core configurations and all applicable crash/futex windows')
    p.add_argument('--repeat', type=int, default=1)
    p.add_argument('--seed', type=int, default=20261113)
    p.add_argument('--timeout', type=float, default=30)
    p.add_argument('--log-dir', default=f'/tmp/shm-system-{os.getpid()}')
    args = p.parse_args()
    args.binary = str(Path(args.binary).resolve())
    args.blackbox_binary = str(Path(args.blackbox_binary).resolve()) if args.blackbox_binary else str(Path(args.binary).with_name('shm_seqlock_blackbox_worker'))
    if args.repeat < 1 or args.timeout <= 0: p.error('positive repeat and timeout required')
    Path(args.log_dir).mkdir(parents=True, exist_ok=True)
    (Path(args.log_dir) / 'environment.json').write_text(json.dumps(dict(argv=os.sys.argv, uname=list(os.uname()),
        affinity=sorted(os.sched_getaffinity(0)), cpu=Path('/proc/cpuinfo').read_text(), seed=args.seed), indent=2))
    configs = list(itertools.product(('inline', 'split', 'inline-premark'), ('classic', 'spec'), ('spin', 'futex'), ('spin', 'futex'))) if args.matrix else [(args.layout, args.read, args.writer_wait, args.reader_wait)]
    selected = list(CASES) if args.case == 'all' else [args.case]
    jobs = []
    for config in configs:
        for case in selected:
            # The lifecycle/stress cases are orthogonal to the 24 wire-config
            # combinations; running them once keeps the matrix focused.
            if case in NARROW_CASES and config != ('inline', 'classic', 'spin', 'spin'):
                continue
            events = ['before_sleep' if case == 'R-futex' and args.event == 'slot_even' else args.event]
            if case == 'R04-crash' and args.matrix:
                events = ['writer_locked', 'segment_created', 'before_magic', 'slot_odd', 'metadata_partial', 'topic_partial', 'topic_written', 'payload_partial', 'slot_even', 'published', 'premarked', 'superseded', 'recovery_scan', 'recovery_epoch']
                if config[0] == 'inline-premark':
                    events = [e for e in events if e != 'topic_written'] + ['cold_written']
            if case == 'R-futex' and args.matrix: events = ['before_waiter', 'waiter_registered', 'before_sleep', 'after_sleep']
            # after_sleep needs a first timed-out FUTEX_WAIT, hence is reached naturally.
            for event in events:
                targets = [args.to_layout]
                if case == 'R04-transition' and args.matrix:
                    targets = [layout for layout in ('inline', 'split', 'inline-premark') if layout != config[0]]
                for target in targets:
                    jobs.append((config, case, event, target))
    random.Random(args.seed).shuffle(jobs)
    results = []
    # Append one line per case: rewriting the whole results.json after every
    # case is O(n^2) and makes the advertised --repeat=20 nightly run write
    # tens of gigabytes. The final results.json is still written below.
    results_jsonl = (Path(args.log_dir) / 'results.jsonl').open('w')
    try:
        for repeat in range(args.repeat):
            for config, case, event, target in jobs:
                args.layout, args.read, args.writer_wait, args.reader_wait = config
                args.event = event
                args.to_layout = target
                name = f'{case}-{args.layout}-{args.read}-{args.writer_wait}-{args.reader_wait}-{event}-{target if case == "R04-transition" else "x"}-{repeat}'
                results.append(execute(args, name, CASES[case]))
                results_jsonl.write(json.dumps(results[-1]) + '\n')
                results_jsonl.flush()
    finally:
        results_jsonl.close()
        (Path(args.log_dir) / 'results.json').write_text(json.dumps(results, indent=2))
    counts = collections.Counter(r['status'] for r in results)
    print(json.dumps(dict(counts=counts, log_dir=args.log_dir)), flush=True)
    return int(bool(counts['FAIL']))


if __name__ == '__main__':
    raise SystemExit(main())
