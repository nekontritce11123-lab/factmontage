# SPDX-License-Identifier: MIT
"""Run: python3 -m unittest discover -s tests -v (no third-party packages)."""
from __future__ import annotations
import concurrent.futures, ctypes as C, hashlib, json, math, os, random, struct, sys, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools.native import Native
from tools.scene import metadata, with_config, mlt_document, safe_name, atomic_write

class EngineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source=(ROOT/'examples/demo.stxt').read_bytes()
        cls.catalog=json.loads((ROOT/'web/catalog.json').read_text())
    def scene(self,**kw):
        changes={0:3,1:1,2:1,6:2,7:-1,10:0,11:0,14:0,15:0,16:0,17:0,22:0,21:1}
        changes.update({int(k):v for k,v in kw.items()})
        return with_config(self.source,changes)
    def test_catalog_and_all_100_recipes(self):
        self.assertEqual([p['id'] for p in self.catalog],list(range(1,101)))
        self.assertEqual(len({p['kernel'] for p in self.catalog}),48)
        fingerprints={}
        with Native() as n:
            for p in self.catalog:
                n.load(self.scene(**{'6':p['id'],'8':p['group'],'9':p['order']}))
                h=hashlib.sha256()
                for t in (.08,.19,.36,.62,.91):h.update(n.render(t,256,144))
                fingerprints.setdefault(h.hexdigest(),[]).append(p['id'])
                self.assertTrue(any(n.render(1.3,256,144)[3::4]),p['id'])
                self.assertFalse(any(n.render(0,256,144)),p['id'])
                self.assertFalse(any(n.render(3,256,144)),p['id'])
        duplicates=[ids for ids in fingerprints.values() if len(ids)>1]
        self.assertEqual(duplicates,[],f'Identical rendered recipes: {duplicates}')
    def test_reverse_matches_entrance_for_all_recipes(self):
        with Native() as n:
            for p in self.catalog:
                n.load(self.scene(**{'6':p['id'],'8':p['group'],'9':p['order']}))
                for t in (.125,.25,.625):
                    self.assertEqual(n.render(t,192,112),n.render(3-t,192,112),(p['id'],t))
    def test_none_and_no_entrance_reverse(self):
        with Native() as n:
            for out in (0,-1):
                n.load(self.scene(**{'6':0,'7':out}))
                a=n.render(0,320,180)
                self.assertTrue(any(a))
                self.assertEqual(a,n.render(1.5,320,180))
                self.assertEqual(a,n.render(2.999,320,180))
    def test_seek_and_independent_instances(self):
        data=self.scene(**{'6':47,'9':4,'10':7,'11':.6,'14':35,'15':.4,'22':.5})
        with Native() as a,Native() as b:
            a.load(data);b.load(data)
            times=[0,.14,2.5,.6,1.2,.14,2.9]
            expected={t:a.render(t,256,144) for t in times}
            for t in reversed(times):self.assertEqual(expected[t],b.render(t,256,144))
    def test_parallel_instances(self):
        data=self.scene(**{'6':47,'10':7,'11':.5})
        def worker(_):
            with Native() as n:n.load(data);return hashlib.sha256(n.render(.47,320,180)).hexdigest()
        with concurrent.futures.ThreadPoolExecutor(4) as pool:self.assertEqual(len(set(pool.map(worker,range(8)))),1)
    def test_exit_selection_and_combination(self):
        with Native() as n:
            n.load(self.scene(**{'6':2,'7':31}));a=n.render(2.65,320,180)
            n.load(self.scene(**{'6':2,'7':2}));b=n.render(2.65,320,180)
            self.assertNotEqual(a,b)
            n.load(self.scene(**{'6':2,'14':35,'15':.8}));mixed=n.render(.35,320,180)
            n.load(self.scene(**{'6':2}));plain=n.render(.35,320,180)
            self.assertNotEqual(mixed,plain)
    def test_alpha_composition_and_transparent_frame(self):
        with Native() as n:
            n.load(self.scene(**{'6':0,'7':0,'21':.6}));rgba=n.render(1,320,180)
            bg=bytes((21,77,130,129))*(320*180);composite=n.render(1,320,180,bg)
            self.assertEqual(composite[:4],bg[:4]);self.assertEqual(rgba[:4],bytes(4))
            alpha=rgba[3::4];self.assertGreater(max(alpha),30);self.assertLessEqual(max(alpha),154)
            # Blend expected straight alpha with the source over equation.
            for px in range(0,320*180,7):
                k=4*px;a=rgba[k+3]/255;ia=129/255;oa=a+ia*(1-a)
                self.assertLessEqual(abs(composite[k+3]-round(oa*255)),1)
                if a>.05:
                    for c in range(3):
                        expected=(rgba[k+c]*a+bg[k+c]*ia*(1-a))/oa
                        self.assertLessEqual(abs(composite[k+c]-round(expected)),2)
    def test_all_life_modes_are_time_varying(self):
        with Native() as n:
            for mode in range(1,9):
                n.load(self.scene(**{'6':0,'7':0,'10':mode,'11':1}))
                self.assertNotEqual(n.render(.7,320,180),n.render(1.7,320,180),mode)
    def test_motion_blur_quality(self):
        with Native() as n:
            n.load(self.scene(**{'22':1,'13':0}));draft=n.render(.21,384,216)
            n.load(self.scene(**{'22':0,'13':1}));self.assertEqual(draft,n.render(.21,384,216))
            n.load(self.scene(**{'22':1,'13':2}));self.assertNotEqual(draft,n.render(.21,384,216))
    def test_binary_validation_preserves_last_good_scene(self):
        variants=[b'',self.source[:20],self.source[:-1],self.source+b'extra',b'BADMAGIC'+self.source[8:]]
        d=bytearray(self.source);struct.pack_into('<f',d,32,float('nan'));variants.append(d)
        for value in (-2,101,1e9):variants.append(with_config(self.source,{6:value}))
        d=bytearray(self.source);struct.pack_into('<I',d,16,513);variants.append(d)
        with Native() as n:
            n.load(self.source);baseline=n.render(1.5,64,64)
            for v in variants:
                with self.assertRaises(ValueError):n.load(bytes(v))
                self.assertEqual(baseline,n.render(1.5,64,64))
            with self.assertRaises(RuntimeError):n.render(float('nan'),64,64)
    def test_no_scene_is_passthrough(self):
        with Native() as n:
            bg=bytes((32,111,244,118))*64*64
            self.assertEqual(n.render(0,64,64,bg),bg)
            self.assertFalse(any(n.render(0,64,64)))
    def test_dimensions_and_frame_boundaries(self):
        with Native() as n:
            n.load(self.scene())
            for w,h in ((64,64),(320,180),(180,320),(768,432)):
                self.assertEqual(len(n.render(.4,w,h)),w*h*4)
                self.assertFalse(any(n.render(-.01,w,h)))
                self.assertFalse(any(n.render(3,w,h)))
            with self.assertRaises(ValueError):n.render(0,0,3)
    def test_short_scene_and_disabled_entrance_duration(self):
        with Native() as n:
            n.load(self.scene(**{'0':.4,'1':4,'2':3}));self.assertTrue(any(n.render(.21,256,144)))
            n.load(self.scene(**{'6':0,'7':2,'1':30,'2':1}));a=n.render(2.5,256,144)
            n.load(self.scene(**{'6':0,'7':2,'1':0,'2':1}));self.assertEqual(a,n.render(2.5,256,144))
    def test_frei0r_abi(self):
        with Native() as n,tempfile.TemporaryDirectory() as tmp:
            class Info(C.Structure):_fields_=[('name',C.c_char_p),('author',C.c_char_p),('type',C.c_int),('color',C.c_int),('api',C.c_int),('major',C.c_int),('minor',C.c_int),('params',C.c_int),('explanation',C.c_char_p)]
            lib=n.lib;lib.f0r_get_plugin_info.argtypes=[C.POINTER(Info)]
            info=Info();lib.f0r_get_plugin_info(C.byref(info));self.assertEqual((info.type,info.color,info.api,info.params),(0,1,1,3))
            lib.f0r_construct.argtypes=[C.c_uint,C.c_uint];lib.f0r_construct.restype=C.c_void_p
            lib.f0r_set_param_value.argtypes=[C.c_void_p,C.c_void_p,C.c_int]
            lib.f0r_get_param_value.argtypes=[C.c_void_p,C.c_void_p,C.c_int]
            lib.f0r_update.argtypes=[C.c_void_p,C.c_double,C.c_void_p,C.c_void_p]
            lib.f0r_destruct.argtypes=[C.c_void_p]
            ptr=lib.f0r_construct(320,180);self.assertTrue(ptr)
            try:
                path=Path(tmp)/'Текст.stxt';path.write_bytes(self.scene());cp=C.c_char_p(os.fsencode(path))
                lib.f0r_set_param_value(ptr,C.byref(cp),0);back=C.c_char_p()
                lib.f0r_get_param_value(ptr,C.byref(back),0);self.assertEqual(back.value,cp.value)
                n.load(path.read_bytes());out=C.create_string_buffer(320*180*4)
                lib.f0r_update(ptr,.42,None,out);self.assertEqual(out.raw,n.render(.42,320,180))
                value=C.c_double(4/120);lib.f0r_set_param_value(ptr,C.byref(value),1)
                lib.f0r_update(ptr,3.5,None,out);self.assertTrue(any(out.raw))
            finally:lib.f0r_destruct(ptr)
    def test_document_helpers(self):
        import xml.etree.ElementTree as ET
        m,c,w,h=metadata(self.source)
        root=ET.fromstring(mlt_document(Path('/tmp/Текст & hello.stxt'),c,w,h))
        self.assertEqual(root.find('.//filter/property[@name="0"]').text,'/tmp/Текст & hello.stxt')
        self.assertEqual(root.find('.//filter/property[@name="threads"]').text,'1')
        self.assertNotIn('/',safe_name('../../foo'))
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'asset.stxt';atomic_write(path,b'a');atomic_write(path,b'b');self.assertEqual(path.read_bytes(),b'b')

if __name__=='__main__':unittest.main(verbosity=2)
