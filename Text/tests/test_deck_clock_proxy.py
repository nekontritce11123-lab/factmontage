# SPDX-License-Identifier: MIT
"""Native forwarding tests for the opt-in Deck clock probe (not a render engine)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#include <stdint.h>
#include <stdlib.h>
typedef struct { unsigned w,h; double value; } Instance;
int f0r_init(void){return 1;}
void f0r_deinit(void){}
void f0r_get_plugin_info(void *p){*(int*)p=123;}
void f0r_get_param_info(void *p,int n){*(int*)p=200+n;}
void *f0r_construct(unsigned w,unsigned h){Instance *p=calloc(1,sizeof(*p));p->w=w;p->h=h;return p;}
void f0r_destruct(void *p){free(p);}
void f0r_set_param_value(void *p,void *v,int n){((Instance*)p)->value=*(double*)v+n;}
void f0r_get_param_value(void *p,void *v,int n){*(double*)v=((Instance*)p)->value+n;}
void f0r_update(void *p,double t,const uint32_t *in,uint32_t *out){
 Instance *s=p;for(unsigned k=0;k<s->w*s->h;++k)out[k]=(in?in[k]:0)+(uint32_t)(t*1000)+s->w+s->h+(uint32_t)(s->value*100);}
'''
WORKER = r'''
import ctypes as C,os,sys
proxy=C.CDLL(sys.argv[1]); proxy.f0r_init.restype=C.c_int
if sys.argv[2]=='preinit':
 value=C.c_int(-1)
 proxy.f0r_get_plugin_info(C.byref(value));assert value.value==123,value.value
 proxy.f0r_get_param_info(C.byref(value),2);assert value.value==202,value.value
ok=proxy.f0r_init()
if sys.argv[2]=='reject':
 assert ok==0,ok
 sys.exit(0)
assert ok==1,ok
real=C.CDLL(os.environ['SUNIMO_STAGE0_REAL_LIBRARY']);assert real.f0r_init()==1
for lib in [proxy,real]:
 for name,args,result in [('f0r_construct',[C.c_uint,C.c_uint],C.c_void_p),('f0r_destruct',[C.c_void_p],None),('f0r_set_param_value',[C.c_void_p,C.c_void_p,C.c_int],None),('f0r_get_param_value',[C.c_void_p,C.c_void_p,C.c_int],None),('f0r_update',[C.c_void_p,C.c_double,C.c_void_p,C.c_void_p],None),('f0r_get_plugin_info',[C.c_void_p],None),('f0r_get_param_info',[C.c_void_p,C.c_int],None)]:
  f=getattr(lib,name);f.argtypes=args;f.restype=result
 info=C.c_int();lib.f0r_get_plugin_info(C.byref(info));assert info.value==123
 lib.f0r_get_param_info(C.byref(info),2);assert info.value==202
p=proxy.f0r_construct(2,2);r=real.f0r_construct(2,2)
try:
 for lib,inst in [(proxy,p),(real,r)]:
  v=C.c_double(.25);lib.f0r_set_param_value(inst,C.byref(v),1)
  lib.f0r_get_param_value(inst,C.byref(v),2);assert v.value==3.25
 for t in [0.0,0.04,2.125,3.0,6.0,3.0]:
  a=(C.c_uint32*4)();b=(C.c_uint32*4)();source=(C.c_uint32*4)(3,4,5,6)
  proxy.f0r_update(p,t,source,a);real.f0r_update(r,t,source,b)
  assert list(a)==list(b),(t,list(a),list(b))
finally:
 proxy.f0r_destruct(p);real.f0r_destruct(r);proxy.f0r_deinit();real.f0r_deinit()
'''

class ClockProxyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.proxy = self.dir/'proxy.so'
        self.real = self.dir/'real.so'

    def build(self):
        source = ROOT/'tools/deck_clock_proxy.c'
        self.assertTrue(source.is_file(), 'Clock forwarding probe is not implemented')
        result = subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror','-o',str(self.proxy),str(source),'-ldl'],capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stderr)

    def run_worker(self, mode, library=None):
        env = os.environ.copy()
        env.pop('SUNIMO_STAGE0_REAL_LIBRARY',None)
        if library is not None: env['SUNIMO_STAGE0_REAL_LIBRARY']=str(library)
        return subprocess.run([sys.executable,'-c',WORKER,str(self.proxy),mode],env=env,capture_output=True,text=True,timeout=10)

    def test_preserves_parameters_pixels_and_exact_incoming_time(self):
        self.build()
        fixture = self.dir/'fixture.c';fixture.write_text(FIXTURE)
        subprocess.run(['cc','-shared','-fPIC','-o',str(self.real),str(fixture)],check=True,capture_output=True,timeout=20)
        result = self.run_worker('forward',self.real)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        lines=[s for s in result.stderr.splitlines() if s.startswith('SUNIMO_STAGE0_CLOCK ')]
        self.assertEqual(len(lines),6,result.stderr)
        times=[float(dict(v.split('=',1) for v in s.split()[1:])['time']) for s in lines]
        self.assertEqual(times,[0.0,0.04,2.125,3.0,6.0,3.0])
        self.assertTrue(all('width=2 height=2' in s for s in lines))

    def test_metadata_before_init_matches_mlt_discovery(self):
        self.build()
        fixture=self.dir/'fixture.c';fixture.write_text(FIXTURE)
        subprocess.run(['cc','-shared','-fPIC','-o',str(self.real),str(fixture)],check=True,capture_output=True,timeout=20)
        result=self.run_worker('preinit',self.real)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_requires_explicit_library(self):
        self.build();result=self.run_worker('reject')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('SUNIMO_STAGE0_ERROR',result.stderr)

    def test_missing_library_fails_closed(self):
        self.build();result=self.run_worker('reject',self.dir/'absent.so')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('SUNIMO_STAGE0_ERROR',result.stderr)

    def test_refuses_its_own_library(self):
        self.build();result=self.run_worker('reject',self.proxy)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('SUNIMO_STAGE0_ERROR',result.stderr)

    def test_missing_required_symbols_fails_closed(self):
        self.build();source=self.dir/'incomplete.c';source.write_text('int f0r_init(void){return 1;}')
        subprocess.run(['cc','-shared','-fPIC','-o',str(self.real),str(source)],check=True,capture_output=True,timeout=20)
        result=self.run_worker('reject',self.real)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('SUNIMO_STAGE0_ERROR',result.stderr)

if __name__=='__main__':unittest.main(verbosity=2)
