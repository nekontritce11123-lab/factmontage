#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Real executable tests. Python standard library only; no model/network needed."""
import subprocess
import sys
import tempfile
from pathlib import Path

exe = str(Path(sys.argv[1]).resolve())
passed = 0

def run(args, data=b''):
    return subprocess.run([exe, *args], input=data, capture_output=True, timeout=20)

def check(name, condition):
    global passed
    if not condition:
        raise AssertionError(name)
    passed += 1
    print('PASS', name)

base = ['stream', '--width', '1', '--height', '1', '--frames', '1', '--feather', '0']
r = run(base, bytes([0, 255, 0, 255]))
check('green pixel becomes transparent', r.returncode == 0 and r.stdout[3] == 0)
r = run(base, bytes([255, 0, 0, 117]))
check('foreground keeps previous alpha', r.returncode == 0 and r.stdout == bytes([255, 0, 0, 117]))
r = run(base, bytes([0, 255]))
check('truncated stream fails with no completed output', r.returncode == 2 and not r.stdout)
r = run(base + ['--method', '1'], bytes([0, 255, 0, 255]))
check('human mode cannot silently run without mask', r.returncode == 2 and not r.stdout)
r = run(base + ['--reference_w', '4294967296'], bytes([0, 255, 0, 255]))
check('oversized reference dimension does not wrap', r.returncode == 2)
r = run(base + ['--feather', 'nan'], bytes([0, 255, 0, 255]))
check('nonfinite parameters rejected', r.returncode == 2)
r = run(['stream', '--width', '1', '--height', '1', '--frames', '2', '--feather', '0'], bytes([0, 255, 0, 255, 255, 0, 0, 255]))
check('two frames remain ordered', r.returncode == 0 and len(r.stdout) == 8 and r.stdout[3] == 0 and r.stdout[7] == 255)
with tempfile.TemporaryDirectory() as d:
    target = Path(d) / 'исходник с пробелом.png'
    target.write_bytes(b'original file sentinel')
    r = run(['png', '--input', str(target), '--output', str(target)])
    check('PNG command refuses existing destination', r.returncode == 2 and target.read_bytes() == b'original file sentinel')
print(f'{passed} passed, 0 failed')
