#!/usr/bin/env python3
"""Reader leave/join and publisher restart test for multitransport_lifecycle_test.

usage: run_tests.py <multitransport_lifecycle_test>

For each transport:
  phase 1: reader a joins, a publisher starts, then reader a leaves
  phase 2: reader b joins, a restarted publisher sends to it
Reader b must write its result file, exit 0 and report bad=0.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

READY_TIMEOUT_S = 5.0
SUB_TIMEOUT_S = 60.0
#messages each publisher sends; reader b must receive all of them
MESSAGES = 1024


def parse_result(line):
    """parse "received=N bad=N gaps=N ooo=N" into a dict of ints"""
    stats = {}
    for tok in line.split():
        k, sep, v = tok.partition("=")
        if sep and v.isdigit():
            stats[k] = int(v)
    return stats


class Runner:
    def __init__(self, binary, root):
        self.binary = binary
        self.root = root
        self.children = []

    def spawn(self, args, log):
        with open(log, "wb") as f:
            p = subprocess.Popen([self.binary] + args, stdout=f, stderr=subprocess.STDOUT)
        self.children.append(p)
        return p

    def kill_all(self):
        for p in self.children:
            if p.poll() is None:
                p.kill()
                p.wait()

    @staticmethod
    def wait_ready(path):
        deadline = time.monotonic() + READY_TIMEOUT_S
        while time.monotonic() < deadline:
            if os.path.exists(path + ".ready"):
                return True
            time.sleep(0.05)
        return False

    @staticmethod
    def wait_exit(p, timeout):
        try:
            return p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            return None

    def run_one(self, name, channel):
        a = os.path.join(self.root, name + "_a")
        b = os.path.join(self.root, name + "_b")
        err = None

        #phase 1: reader a joins, then leaves while the publisher is still running
        ap = self.spawn(["sub", channel, a, "2048", "0"], a + ".log")
        if not self.wait_ready(a):
            err = "reader a not ready"
        pp = self.spawn(["pub", channel, a, str(MESSAGES)], os.path.join(self.root, name + "_p.log"))
        self.wait_exit(pp, SUB_TIMEOUT_S)
        ap.kill()
        ap.wait()

        #phase 2: reader b joins, a restarted publisher sends to it
        bp = self.spawn(["sub", channel, b, str(MESSAGES), "0"], b + ".log")
        if err is None and not self.wait_ready(b):
            err = "reader b not ready"
        pp2 = self.spawn(["pub", channel, b, str(MESSAGES)], os.path.join(self.root, name + "_p2.log"))
        self.wait_exit(pp2, SUB_TIMEOUT_S)
        brc = self.wait_exit(bp, SUB_TIMEOUT_S)

        result = ""
        if os.path.exists(b):
            with open(b) as f:
                result = f.read().strip()
        if err is None:
            if brc is None:
                err = "reader b timed out"
            elif not os.path.exists(b):
                err = "reader b wrote no result (exit %d)" % brc
            else:
                stats = parse_result(result)
                if brc != 0 or stats.get("bad") != 0 or stats.get("ooo") != 0:
                    err = "reader b exit %d: %s" % (brc, result)
                elif stats.get("received") != MESSAGES or stats.get("gaps") != 0:
                    err = "reader b expected received=%d gaps=0: %s" % (MESSAGES, result)

        if err is not None:
            print("FAIL %s: %s" % (name, err), flush=True)
            return False
        print("PASS %s reader leave/join and publisher restart: %s" % (name, result), flush=True)
        return True


def main():
    if len(sys.argv) < 2:
        print("usage: run_tests.py <multitransport_lifecycle_test>", file=sys.stderr)
        return 2
    binary = os.path.abspath(sys.argv[1])
    pid = os.getpid()
    zmq_port = int(os.environ.get("TM_MT_ZMQ_PORT", "52000")) + pid % 1000
    cases = [
        ("seqlock", "shm_seqlock://::::mt-life-%d_s[slotSize=8192,slotCount=4096]" % pid),
        ("cursor", "shm_cursor://::::mt-life-%d_c[slotSize=8192,slotCount=4096]" % pid),
        ("zeromq", "zeromq://127.0.0.1:%d" % zmq_port),
    ]
    root = tempfile.mkdtemp(prefix="tm-mt-life-%d-" % pid)
    runner = Runner(binary, root)
    ok = True
    try:
        for name, channel in cases:
            ok = runner.run_one(name, channel) and ok
    finally:
        runner.kill_all()
        shutil.rmtree(root, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
