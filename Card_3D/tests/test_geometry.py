from pathlib import Path
import ctypes as C,json,subprocess,os
import numpy as np
from host import EnginePlugin as Plugin,ROOT
from test_engine import image
checks={};a=(C.c_double*11)();f=(C.c_double*9)();i=(C.c_double*9)()
with Plugin(256,144) as p:
 p.lib.card3d_debug_anim.argtypes=[C.c_void_p,C.c_double,C.c_void_p]
 p.lib.card3d_debug_matrix.argtypes=[C.c_void_p,C.c_double,C.c_void_p,C.c_void_p]
 p.set(DURATION=1,SWING=25,BASE_YAW=-9,BASE_PITCH=4)
 p.render(image(),2)
 maximum=0
 for style in range(1,5):
  for corner in range(4):
   p.set(ENTRANCE=style,CORNER=corner);p.render(image(),2)
   last=2.
   for t in np.linspace(0,1,101):
    p.lib.card3d_debug_anim(p.instance,float(t),a)
    assert abs(a[8]+9)<1e-10 and abs(a[9]-4)<1e-10 and abs(a[10])<1e-10,'entrance rotated unexpectedly'
    assert 0<=a[2]<=last+1e-12;last=a[2]
    assert .8<a[4]<1.3 and .8<a[5]<1.3
    p.lib.card3d_debug_matrix(p.instance,float(t),f,i)
    m=np.array(f).reshape(3,3);inv=np.array(i).reshape(3,3)
    err=np.max(np.abs(m@inv-np.eye(3)));maximum=max(maximum,float(err));assert err<1e-7
 checks['entrance_states_checked']=4*4*101
 checks['no_spin_during_entrance']=True
 checks['monotonic_ease_out']=True
 checks['matrix_inverse_max_error']=maximum
 p.set(ENTRANCE=2);p.render(image(),2)
 for t in np.linspace(1,400,5000):
  p.lib.card3d_debug_anim(p.instance,float(t),a)
  assert abs(a[8]+9)<=25.000001
  assert abs(a[9]-4)<=25*.32*.65+1e-6
  assert abs(a[10])<=25*.07*.65+1e-6
 checks['bounded_organic_states_checked']=5000
print(json.dumps(checks,indent=2))
if os.environ.get('CARD3D_GEOMETRY_REPORT'):
 Path(os.environ['CARD3D_GEOMETRY_REPORT']).write_text(json.dumps(checks,indent=2)+'\n', encoding='utf-8')
