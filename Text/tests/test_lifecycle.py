# SPDX-License-Identifier: MIT
"""v0.2 regressions: hold signatures, v1 loading, v2 flags, no accumulated drift."""
from __future__ import annotations
import hashlib,json,struct,sys,unittest,subprocess,shutil,tempfile,os
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from tools.native import Native
from tools.scene import with_config,metadata

class LifecycleTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.source=(ROOT/'examples/life_v02.stxt').read_bytes()
  cls.catalog=json.loads((ROOT/'web/catalog.json').read_text())
 def scene(self,changes=None):
  c={0:12,1:1.2,2:1,6:0,7:0,10:9,11:1,12:1,14:0,15:0,16:0,17:0,18:0,22:0};c.update(changes or {})
  return with_config(self.source,c)
 def test_all_100_hold_profiles_render_and_differ(self):
  signatures={}
  with Native() as n:
   for p in self.catalog:
    n.load(self.scene({18:p['id'],8:p['group'],9:p['order']}))
    h=hashlib.sha256();frames=set()
    for t in (.31,.77,1.21,1.63,2.17,2.61,3.19,4.07,5.11,6.23,7.31):
     frame=n.render(t,256,144);h.update(frame);frames.add(hashlib.sha256(frame).digest())
     self.assertTrue(any(frame[3::4]),p['id'])
    self.assertGreater(len(frames),1,p['id']);signatures.setdefault(h.hexdigest(),[]).append(p['id'])
  self.assertEqual([ids for ids in signatures.values() if len(ids)>1],[])
 def test_life_none_is_exactly_static(self):
  with Native() as n:
   for mode,amount in ((0,1),(9,0)):
    n.load(self.scene({10:mode,11:amount,18:47}));frame=n.render(.01,320,180)
    for t in (.7,3.2,10.9):self.assertEqual(frame,n.render(t,320,180))
 def test_auto_follows_entrance_not_exit(self):
  with Native() as n:
   n.load(self.scene({6:47,7:2,18:0}));a=n.render(3.5,320,180)
   n.load(self.scene({6:47,7:70,18:0}));self.assertEqual(a,n.render(3.5,320,180))
   n.load(self.scene({6:21,7:70,18:0}));self.assertNotEqual(a,n.render(3.5,320,180))
 def test_standalone_life_works_without_transitions(self):
  with Native() as n:
   n.load(self.scene({6:0,7:0,18:47}));a=n.render(0,320,180)
   self.assertTrue(any(a));self.assertNotEqual(a,n.render(1,320,180))
 def test_hold_mixing_and_amount(self):
  with Native() as n:
   n.load(self.scene({6:2,18:0}));a=n.render(3,320,180)
   n.load(self.scene({6:2,18:0,14:31,15:.8}));self.assertNotEqual(a,n.render(3,320,180))
   n.load(self.scene({6:2,18:0,11:.3}));self.assertNotEqual(a,n.render(3,320,180))
 def test_digital_profiles_are_sparse_and_deterministic(self):
  with Native() as n:
   for id in (63,64,69,70):
    n.load(self.scene({18:id}));ref={t:n.render(t,192,108) for t in (.17,.4,1.61,3.4,5.1)}
    for t in reversed(list(ref)):self.assertEqual(ref[t],n.render(t,192,108))
 def test_format_v2_and_legacy(self):
  self.assertEqual(struct.unpack_from('<I',self.source,8)[0],2)
  metadata(self.source)
  with Native() as n:
   n.load((ROOT/'examples/demo.stxt').read_bytes());self.assertTrue(any(n.render(1.3,96,64)))
   data=bytearray(self.scene());struct.pack_into('<I',data,8,1)
   with self.assertRaises(ValueError):n.load(bytes(data))
   for changes in ({18:101},{10:10},{18:-1}):
    with self.assertRaises(ValueError):n.load(self.scene(changes))
 def test_v3_layout_keeps_v1_v2_preset_ids(self):
  data=bytearray(self.scene({6:73,18:44}));struct.pack_into('<I',data,8,3)
  self.assertEqual(metadata(data)[0]['text'],metadata(self.source)[0]['text'])
  with Native() as n:
   n.load(bytes(data));self.assertTrue(any(n.render(2,192,108)))
 def test_analytic_grouping_and_continuity(self):
  built=os.environ.get('SUNIMO_TEXT_INVARIANTS')
  if built:
   p=subprocess.run([built,str(ROOT/'examples/life_v02.stxt')],capture_output=True,text=True,timeout=30)
   self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   return
  compiler=shutil.which('c++')
  if not compiler:self.skipTest('C++ compiler not installed (optional invariant harness)')
  with tempfile.TemporaryDirectory() as tmp:
   binary=Path(tmp)/'invariants'
   subprocess.run([compiler,'-std=c++17','-O2','-I',str(ROOT/'src'),str(ROOT/'tests/motion_invariants.cpp'),'-o',str(binary)],check=True,capture_output=True)
   p=subprocess.run([str(binary),str(ROOT/'examples/life_v02.stxt')],capture_output=True,text=True)
   self.assertEqual(p.returncode,0,p.stdout+p.stderr)

if __name__=='__main__':unittest.main(verbosity=2)
