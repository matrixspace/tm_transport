#!/usr/bin/env python3
"""Deterministic negative controls for the coordinator's final-drain oracle.

These fixture processes never use TM Kit or shared memory: they emit an invalid
record only after QUIT, where a coordinator that merely checks earlier callbacks
would incorrectly report success.
"""
import json
from pathlib import Path
import tempfile
from run_tests import Suite, Worker


def verify():
    cases = [('', False), ('DATA aa', True), ('BAD_HB', True),
             ('ERROR late failure', True), ('REGISTER', True)]
    results = []
    with tempfile.TemporaryDirectory(prefix='shmhb-oracle-') as temporary:
        root = Path(temporary)
        for index, (tail, should_reject) in enumerate(cases):
            output = root / str(index)
            output.mkdir()
            binary = output / 'fake-worker'
            binary.write_text('#!/usr/bin/env python3\n'
                              'print("READY", flush=True)\n'
                              'input()\n'
                              f'print({tail!r}, flush=True)\n'
                              'print("DONE", flush=True)\n')
            binary.chmod(0o700)
            suite = Suite(str(binary), output, 'inline')
            rejected = False
            try:
                worker = Worker(suite, 'client', '-', '-')
                try:
                    assert worker.close() == 0
                except AssertionError:
                    rejected = True
                assert rejected == should_reject, (tail, rejected, should_reject)
                results.append({'tail': tail, 'expected_rejection': should_reject, 'status': 'PASS'})
            finally:
                suite.cleanup()
    print(json.dumps({'driver_oracle': results}))


if __name__ == '__main__':
    verify()
