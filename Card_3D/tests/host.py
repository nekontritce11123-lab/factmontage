"""Test/demo host. Release tests use only the published controls."""
from pathlib import Path
from functools import lru_cache
import atexit
import ctypes as C
import json
import os
import sys
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = json.loads((ROOT/'docs/parameters.json').read_text(encoding='utf-8'))
BYKEY = {p['key']: p for p in SCHEMA}
ENGINE_SCHEMA = json.loads((ROOT/'docs/engine_parameters.json').read_text(encoding='utf-8'))
ENGINE_BYKEY = {p['key']: p for p in ENGINE_SCHEMA}

# Independently checked against the system frei0r.h by tests/abi_host.cpp.
class Color(C.Structure):
    _fields_ = [('r', C.c_float), ('g', C.c_float), ('b', C.c_float)]


def library_path(internal=False):
    env = 'CARD3D_TEST_LIBRARY' if internal else 'CARD3D_LIBRARY'
    if os.environ.get(env): return Path(os.environ[env]).resolve()
    if sys.platform == 'win32':
        return ROOT/'build/native'/('card3d_test.dll' if internal else 'card3d.dll')
    return ROOT/('build/linux/card3d_test.so' if internal else 'bin/linux-x86_64/card3d.so')


@lru_cache(maxsize=None)
def load_library(path):
    lib = C.CDLL(str(path))
    lib.f0r_init.restype = C.c_int
    if not lib.f0r_init(): raise RuntimeError('f0r_init failed')
    lib.f0r_deinit.restype = None
    atexit.register(lib.f0r_deinit)
    lib.f0r_construct.argtypes = [C.c_uint, C.c_uint]
    lib.f0r_construct.restype = C.c_void_p
    lib.f0r_destruct.argtypes = [C.c_void_p]
    lib.f0r_destruct.restype = None
    for name in ('f0r_set_param_value', 'f0r_get_param_value', 'card3d_test_set', 'card3d_test_get'):
        if hasattr(lib, name):
            fn = getattr(lib, name); fn.argtypes = [C.c_void_p, C.c_void_p, C.c_int]; fn.restype = None
    lib.f0r_update.argtypes = [C.c_void_p, C.c_double, C.c_void_p, C.c_void_p]
    lib.f0r_update.restype = None
    return lib


class Plugin:
    schema = BYKEY
    internal = False

    def __init__(self, w=1280, h=720, path=None):
        self.w, self.h = w, h
        self.lib = load_library(str(path or library_path(self.internal)))
        self.instance = self.lib.f0r_construct(w, h)
        if not self.instance: raise RuntimeError('Filter rejected dimensions/allocation')

    def set(self, **kwargs):
        for key, value in kwargs.items():
            p = self.schema[key]
            if p['kind'] == 'color':
                v = value.lstrip('#'); data = Color(*[int(v[i:i+2], 16)/255 for i in (0, 2, 4)])
            else:
                data = C.c_double((float(value)-p['lo'])/(p['hi']-p['lo']) if self.internal else float(value)/p['hi'])
            fn = self.lib.card3d_test_set if self.internal else self.lib.f0r_set_param_value
            fn(self.instance, C.byref(data), p['index'])
        return self

    def render(self, image, time, inplace=False):
        a = np.ascontiguousarray(image, dtype=np.uint8)
        if a.shape != (self.h, self.w, 4): raise ValueError('Expected an RGBA frame matching the instance')
        out = a if inplace else np.empty_like(a)
        self.lib.f0r_update(self.instance, float(time), a.ctypes.data, out.ctypes.data)
        return out

    def close(self):
        if self.instance: self.lib.f0r_destruct(self.instance); self.instance = None

    def __enter__(self): return self
    def __exit__(self, *args): self.close()


class EnginePlugin(Plugin):
    """Private algorithm regression controls. Never loaded from the release binary."""
    schema = ENGINE_BYKEY
    internal = True

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.lib.card3d_test_reset.argtypes = [C.c_void_p]
        self.lib.card3d_test_reset.restype = None
        self.lib.card3d_test_reset(self.instance)
