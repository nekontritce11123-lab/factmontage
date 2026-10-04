#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare actual full-resolution RGBA output of independently built plugins."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'Text'))
from tools.native import Native
from tools.scene import with_config


class Filter:
    def __init__(self, path, width, height):
        self.lib = C.CDLL(str(path))
        self.lib.f0r_construct.argtypes = [C.c_uint, C.c_uint]
        self.lib.f0r_construct.restype = C.c_void_p
        self.lib.f0r_set_param_value.argtypes = [C.c_void_p, C.c_void_p, C.c_int]
        self.lib.f0r_update.argtypes = [C.c_void_p, C.c_double, C.c_void_p, C.c_void_p]
        self.lib.f0r_destruct.argtypes = [C.c_void_p]
        self.handle = self.lib.f0r_construct(width, height)
        assert self.handle, path
        self.output = C.create_string_buffer(width * height * 4)

    def set(self, index, value):
        self.lib.f0r_set_param_value(self.handle, C.byref(C.c_double(value)), index)

    def render(self, time, source):
        self.lib.f0r_update(self.handle, time, source, self.output)
        return self.output.raw

    def close(self):
        self.lib.f0r_destruct(self.handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    evidence = {'size': [1920, 1080], 'input': 'periodic_xy_rgba', 'frames': 0, 'binaries': {}}
    rows = [bytes(component for x in range(32) for component in
                  ((x * 7 + y * 3) & 255, (x * 3 + y * 5) & 255, (x * 5 + y * 7) & 255, (x * 11 + y * 13) & 255)) * 60
            for y in range(32)]
    source = b''.join(rows[y % 32] for y in range(1080))
    for module, library in [('card', 'card3d.so'), ('effects', 'studiofx.so')]:
        paths = [root / module / library for root in (args.before, args.after)]
        for path in paths:
            evidence['binaries'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        filters = [Filter(path, 1920, 1080) for path in paths]
        try:
            choices = range(80) if module == 'effects' else range(3)
            for choice in choices:
                for filter in filters:
                    filter.set(0, choice / (127 if module == 'effects' else 2))
                for time in (0, .17, 1.5, 2.95):
                    assert filters[0].render(time, source) == filters[1].render(time, source), (module, choice, time)
                    evidence['frames'] += 1
            # Reuse each instance across disable/re-enable and algorithm changes.
            for filter in filters:
                filter.set(0, 0)
            assert filters[0].render(.4, source) == filters[1].render(.4, source), module
            evidence['frames'] += 1
        finally:
            for filter in filters:
                filter.close()
    paths = [root / 'text/sunimo_text_studio.so' for root in (args.before, args.after)]
    for path in paths:
        evidence['binaries'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    with Native(paths[0]) as before, Native(paths[1]) as after:
        for scene in sorted((ROOT / 'Text/examples').glob('*.stxt')):
            for blur in (0, .5, 0, .5):
                data = with_config(scene.read_bytes(), {13: 2, 22: blur})
                before.load(data); after.load(data)
                for time in (0, .19, .8, 2.9):
                    assert before.render(time, 1920, 1080, source) == after.render(time, 1920, 1080, source), (scene.name, blur, time)
                    evidence['frames'] += 1
    evidence['status'] = 'RGBA IDENTICAL'
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(evidence))


if __name__ == '__main__':
    main()
