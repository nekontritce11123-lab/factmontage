# SPDX-License-Identifier: MIT
"""ctypes bridge shared by preview, CLI, benchmark and integration tests."""
from __future__ import annotations
import ctypes as C
import os
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
class Native:
    def __init__(self, library: str | Path | None=None):
        explicit = library or os.environ.get('SUNIMO_TEXT_LIBRARY')
        path = Path(explicit).expanduser().resolve() if explicit else ROOT/'bin/sunimo_text_studio.so'
        if not explicit and not path.is_file():
            path=ROOT/'build/sunimo_text_studio.so'
        if not path.is_file():
            raise RuntimeError(f'Не найден выбранный движок: {path}. Выполните ./dev build text; старый бинарник автоматически не подставляется.')
        self.lib=C.CDLL(str(path))
        self.lib.smt_create.argtypes=[]; self.lib.smt_create.restype=C.c_void_p
        self.lib.smt_destroy.argtypes=[C.c_void_p]; self.lib.smt_destroy.restype=None
        self.lib.smt_load.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t]; self.lib.smt_load.restype=C.c_int
        self.lib.smt_render.argtypes=[C.c_void_p,C.c_double,C.c_uint,C.c_uint,C.c_void_p,C.c_void_p]; self.lib.smt_render.restype=C.c_int
        self.lib.smt_last_error.argtypes=[C.c_void_p]; self.lib.smt_last_error.restype=C.c_char_p
        self.handle=self.lib.smt_create()
        if not self.handle: raise RuntimeError('Не удалось создать экземпляр движка.')
        self._buffer=None; self._size=0
    def load(self, data: bytes)->None:
        if not self.lib.smt_load(self.handle,data,len(data)): raise ValueError(self.error)
    @property
    def error(self)->str:
        return self.lib.smt_last_error(self.handle).decode('utf-8','replace')
    def render(self,t:float,w:int,h:int,background:bytes|None=None)->bytes:
        if not 1<=w<=7680 or not 1<=h<=7680 or w*h>33554432: raise ValueError('Недопустимый размер кадра')
        size=w*h*4
        if background is not None and len(background)!=size: raise ValueError('Неверный размер входного RGBA')
        if self._size!=size:self._buffer=C.create_string_buffer(size);self._size=size
        if not self.lib.smt_render(self.handle,t,w,h,background,self._buffer): raise RuntimeError(self.error)
        return self._buffer.raw
    def close(self):
        if self.handle:self.lib.smt_destroy(self.handle);self.handle=None
    def __enter__(self):return self
    def __exit__(self,*_):self.close()
