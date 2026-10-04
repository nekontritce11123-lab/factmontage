#!/usr/bin/env python3
"""Real binary tests. No GUI/Flatpak claims are made by these checks."""
import ctypes as C,json,time,unittest,xml.etree.ElementTree as E
from pathlib import Path
import numpy as np
from host import EnginePlugin as Plugin, ROOT, ENGINE_SCHEMA as SCHEMA, ENGINE_BYKEY as BYKEY, Color

def image(w=256,h=144):
    a=np.zeros((h,w,4),np.uint8);a[:]=[224,65,42,255]
    a[::17,:,0:3]=[250,235,197];a[:,::19,0:3]=[250,235,197]
    return a
class RendererTests(unittest.TestCase):
    def new(self,w=256,h=144,**params):
        p=Plugin(w,h);self.addCleanup(p.close);p.set(**params);return p
    def clean(self,**kw):
        v=dict(ENTRANCE=0,DELAY=0,SWING=0,BASE_YAW=0,BASE_PITCH=0,SHADOW=0,DEPTH=0,RADIUS=0,BORDER=0,PLACEMENT=0,SIZE=80)
        v.update(kw);return self.new(**v)
    def test_01_abi_rejects_bad_dimensions(self):
        p=self.new();self.assertFalse(p.lib.f0r_construct(0,100));self.assertFalse(p.lib.f0r_construct(999999,2))
    def test_02_defaults_match_schema(self):
        p=self.new()
        for s in SCHEMA:
            if s['kind']=='color':continue
            out=C.c_double();p.lib.card3d_test_get(p.instance,C.byref(out),s['index'])
            self.assertAlmostEqual(out.value,s['normalized'],12)
    def test_03_nan_and_bounds(self):
        p=self.new();old=C.c_double();idx=BYKEY['SWING']['index'];p.lib.card3d_test_get(p.instance,C.byref(old),idx)
        p.lib.card3d_test_set(p.instance,C.byref(C.c_double(float('nan'))),idx)
        now=C.c_double();p.lib.card3d_test_get(p.instance,C.byref(now),idx);self.assertEqual(old.value,now.value)
        p.lib.card3d_test_set(p.instance,C.byref(C.c_double(9)),idx)
        p.lib.card3d_test_get(p.instance,C.byref(now),idx);self.assertEqual(now.value,1)
    def test_04_empty_start_includes_shadow_and_depth(self):
        p=self.new(SHADOW=80,DEPTH=2);self.assertFalse(p.render(image(),0).any())
    def test_05_delay_is_exact_and_seekable(self):
        p=self.new(DELAY=2);src=image();p.render(src,7)
        for t in [-1,0,1.99,2]:self.assertFalse(p.render(src,t).any())
        self.assertGreater(p.render(src,2.8)[:,:,3].sum(),0)
    def test_06_all_entrance_corner_combinations(self):
        src=image()
        for k in range(1,5):
            for d in range(4):
                p=self.new(ENTRANCE=k,CORNER=d,SWING=0,DURATION=.8)
                self.assertFalse(p.render(src,0).any())
                self.assertGreater(p.render(src,.7)[:,:,3].sum(),0)
                self.assertTrue(np.array_equal(p.render(src,1),p.render(src,8)))
    def test_07_stationary_after_entry(self):
        p=self.new(SWING=0,EDGE=1);src=image()
        self.assertTrue(np.array_equal(p.render(src,1.2),p.render(src,95.3)))
    def test_08_repeated_time_independent_of_history(self):
        p=self.new();src=image();a=p.render(src,3.7)
        for t in [9,.2,15,0]:p.render(src,t)
        self.assertTrue(np.array_equal(a,p.render(src,3.7)))
    def test_09_fresh_and_cached_match(self):
        src=image();p=self.new();p.render(src,3);a=p.render(src,.3)
        q=self.new();self.assertTrue(np.array_equal(a,q.render(src,.3)))
    def test_10_input_not_modified(self):
        src=image();copy=src.copy();self.new().render(src,2);self.assertTrue(np.array_equal(src,copy))
    def test_11_alias_input_output(self):
        src=image();p=self.new();a=p.render(src,2);b=p.render(src.copy(),2,True);self.assertTrue(np.array_equal(a,b))
    def test_12_transparent_source_no_frame(self):
        p=self.clean(AUTOCROP=1);self.assertFalse(p.render(np.zeros_like(image()),2).any())
    def test_13_hidden_rgb_does_not_leak(self):
        p=self.clean(AUTOCROP=0,PLATE=0);src=image();src[:,:,3]=0
        self.assertFalse(p.render(src,1).any())
    def test_14_rounded_corners_reduce_silhouette(self):
        src=image();p=self.clean();a=p.render(src,1)[:,:,3].sum();p.set(RADIUS=20);b=p.render(src,1)[:,:,3].sum();self.assertLess(b,a*.99)
    def test_15_paper_changes_edge_and_is_stable(self):
        src=image();p=self.clean(BORDER=4);a=p.render(src,1);p.set(EDGE=1,TEAR=3);b=p.render(src,1)
        self.assertFalse(np.array_equal(a,b));self.assertTrue(np.array_equal(b,p.render(src,42)))
    def test_16_shadow_expands_silhouette(self):
        p=self.clean();src=image();a=p.render(src,1);p.set(SHADOW=65)
        # A shadow setting changes safe layout, so compare absolute low-alpha counts.
        b=p.render(src,1);self.assertGreater(np.sum((b[:,:,3]>0)&(b[:,:,3]<150)),np.sum((a[:,:,3]>0)&(a[:,:,3]<150))+10)
    def test_17_frame_color_changes_only_material(self):
        p=self.clean(BORDER=8);src=image();a=p.render(src,1);p.set(FRAME_COLOR='#00ff00');b=p.render(src,1)
        self.assertGreater(np.sum(b[:,:,1].astype(int)>b[:,:,0].astype(int)+20),np.sum(a[:,:,1].astype(int)>a[:,:,0].astype(int)+20))
    def test_18_auto_alpha_crop_portrait(self):
        src=np.zeros_like(image());src[12:-12,104:152]=[200,70,40,255]
        p=self.clean(AUTOCROP=1);o=p.render(src,1);yy,xx=np.where(o[:,:,3]>240)
        self.assertLess(xx.max()-xx.min(),(yy.max()-yy.min())*.5)
    def test_19_changed_source_cache_invalidated(self):
        p=self.new();src=image();p.render(src,2);src2=src.copy();src2[:,:,0:3]=[35,83,171]
        a=p.render(src2,2);q=self.new();self.assertTrue(np.array_equal(a,q.render(src2,2)))
    def test_20_source_crop_change_history(self):
        p=self.new();src=image();p.render(src,2);src2=np.zeros_like(src);src2[10:135,90:170]=[35,83,171,255]
        a=p.render(src2,2);q=self.new();self.assertTrue(np.array_equal(a,q.render(src2,2)))
    def test_21_style_change_history(self):
        p=self.new();src=image();p.render(src,2);args=dict(BORDER=5,RADIUS=12,FRAME_COLOR='#abcdef',PLATE=20,TEAR=1.7,EDGE=1)
        a=p.set(**args).render(src,2);b=self.new(**args).render(src,2);self.assertTrue(np.array_equal(a,b))
    def test_22_portrait_square_odd_sizes(self):
        for w,h in [(144,256),(181,181),(321,181),(1,1),(17,29)]:
            p=self.new(w,h);o=p.render(image(w,h),2);self.assertEqual(o.shape,(h,w,4))
    def test_23_no_rgb_outside_alpha(self):
        p=self.new(EDGE=1);o=p.render(image(),2);self.assertFalse(o[o[:,:,3]==0,:3].any())
    def test_24_geometry_changes_dont_rebuild_wrong_image(self):
        p=self.new();src=image();p.render(src,2);args=dict(SWING=22,BASE_PITCH=-9,BASE_YAW=23,PLACEMENT=6,SIZE=90)
        a=p.set(**args).render(src,5);b=self.new(**args).render(src,5);self.assertTrue(np.array_equal(a,b))
    def test_26_numeric_stress(self):
        rng=np.random.default_rng(17);src=image(96,64);p=self.new(96,64)
        for n in range(80):
            for key in ['ENTRANCE','CORNER','RADIUS','BORDER','EDGE','TEAR','DEPTH','SWING','ORGANIC','BASE_YAW','BASE_PITCH','PLACEMENT','SIZE','VARIANT']:
                s=BYKEY[key];p.set(**{key:rng.uniform(s['lo'],s['hi'])})
            a=p.render(src,float(rng.uniform(-2,25)));self.assertEqual(a.shape,(64,96,4))
    def test_27_proxy_relative_shape(self):
        result=[]
        for w,h in [(320,180),(640,360)]:
            p=self.new(w,h,ENTRANCE=0,SWING=0,BASE_YAW=0,BASE_PITCH=0,SHADOW=0,DEPTH=0,BORDER=0,RADIUS=5)
            a=p.render(image(w,h),2);result.append(np.sum(a[:,:,3]>128)/(w*h))
        self.assertLess(abs(result[0]-result[1]),.015)
    def test_28_sides_can_be_disabled(self):
        p=self.clean(DEPTH=0,BASE_YAW=25);src=image();a=p.render(src,2);p.set(DEPTH=2);b=p.render(src,2);self.assertFalse(np.array_equal(a,b))

if __name__=='__main__':
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(RendererTests)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    (ROOT/'docs/engine_results.json').write_text(json.dumps(dict(tests=result.testsRun,failures=len(result.failures),errors=len(result.errors),success=result.wasSuccessful()),indent=2)+'\n', encoding='utf-8')
    raise SystemExit(not result.wasSuccessful())
