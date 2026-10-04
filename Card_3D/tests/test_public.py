"""Release ABI and UI-to-renderer regression tests, separate from private engine tests."""
import ctypes as C
import importlib.util
import math
import unittest
import xml.etree.ElementTree as E
import numpy as np
from host import Plugin, ROOT, SCHEMA, BYKEY, Color, library_path, ENGINE_BYKEY
from test_engine import image


class Info(C.Structure):
    _fields_ = [('name', C.c_char_p), ('author', C.c_char_p), ('plugin_type', C.c_int),
                ('color_model', C.c_int), ('frei0r_version', C.c_int), ('major', C.c_int),
                ('minor', C.c_int), ('num_params', C.c_int), ('explanation', C.c_char_p)]


class ParamInfo(C.Structure):
    _fields_ = [('name', C.c_char_p), ('type', C.c_int), ('explanation', C.c_char_p)]


class PublicTests(unittest.TestCase):
    def new(self, debug=False, **kw):
        p = Plugin(256, 144, path=library_path(True) if debug else None)
        self.addCleanup(p.close)
        return p.set(**kw)

    def raw(self, p, key):
        value = C.c_double()
        p.lib.f0r_get_param_value(p.instance, C.byref(value), BYKEY[key]['index'])
        return value.value

    def internal(self, p, key):
        spec = ENGINE_BYKEY[key]
        value = Color() if spec['kind'] == 'color' else C.c_double()
        p.lib.card3d_test_get(p.instance, C.byref(value), spec['index'])
        return tuple(round(x*255) for x in (value.r, value.g, value.b)) if spec['kind'] == 'color' else spec['lo'] + value.value*(spec['hi']-spec['lo'])

    def test_release_metadata_and_exports(self):
        p = self.new(); info = Info()
        p.lib.f0r_get_plugin_info.argtypes = [C.POINTER(Info)]
        p.lib.f0r_get_plugin_info(C.byref(info))
        self.assertEqual((info.name, info.num_params, info.color_model), (b'Card 3D', 18, 1))
        p.lib.f0r_get_param_info.argtypes = [C.POINTER(ParamInfo), C.c_int]
        for n, name in enumerate(('STYLE', 'ENTRANCE', 'CORNER', 'PLACEMENT', 'SIZE', 'DURATION', 'MOTION', 'SOURCE', 'ROUNDING', 'BACKGROUND', 'SHADOW', 'POSITION_MODE', 'X', 'Y', 'EXIT_ENABLED', 'EXIT_ANIMATION', 'EXIT_DURATION', 'EXIT_AT')):
            item = ParamInfo(); p.lib.f0r_get_param_info(C.byref(item), n)
            self.assertEqual((item.name.decode(), item.type), (name, 1))
        self.assertFalse(hasattr(p.lib, 'card3d_test_set'))

    def test_defaults_are_independently_expected(self):
        p = self.new()
        for key, expected in dict(STYLE=0, ENTRANCE=.25, CORNER=1/3, PLACEMENT=1/6,
                                  SIZE=.68, DURATION=.34, MOTION=.5, SOURCE=0, ROUNDING=.2,
                                  BACKGROUND=0, SHADOW=.35, POSITION_MODE=0, X=0, Y=.5,
                                  EXIT_ENABLED=1, EXIT_ANIMATION=0, EXIT_DURATION=.34, EXIT_AT=0).items():
            self.assertAlmostEqual(self.raw(p, key), expected, 12)

    def test_xml_and_generator_agree(self):
        xml = E.parse(ROOT/'kdenlive/card3d.xml').getroot()
        self.assertEqual(xml.tag, 'effect'); self.assertEqual(xml.get('tag'), 'frei0r.card3d')
        params = xml.findall('parameter'); self.assertEqual(len(params), 18)
        self.assertEqual({int(p.get('name')) for p in params}, set(range(18)))
        self.assertTrue(all(p.get('type') in ('constant', 'list', 'bool', 'fixed') for p in params))
        self.assertTrue(all('offset' not in p.attrib for p in params))
        spec = importlib.util.spec_from_file_location('ui', ROOT/'scripts/generate_ui.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        for path, text in module.outputs().items(): self.assertEqual(path.read_text(encoding='utf-8'), text)

    def test_numeric_roundtrip_ui_host_renderer(self):
        p = self.new(debug=True)
        params = E.parse(ROOT/'kdenlive/card3d.xml').getroot().findall('parameter')
        for key, cases in {'SIZE': [15, 68, 100], 'DURATION': [.25, .85, 2.5],
                           'ROUNDING': [0, .8, 4, 5, 10, 20], 'SHADOW': [0, 35, 100],
                           'X': [0, 50, 100], 'Y': [0, 50, 100]}.items():
            node = params[BYKEY[key]['index']]; factor = float(node.get('factor'))
            for requested in cases:
                wire = C.c_double(requested/factor)
                p.lib.f0r_set_param_value(p.instance, C.byref(wire), BYKEY[key]['index'])
                self.assertAlmostEqual(self.raw(p, key)*factor, requested, 10)
                self.assertAlmostEqual(self.internal(p, 'RADIUS' if key == 'ROUNDING' else key), requested, 10)

    def test_bounds_nonfinite_and_menu_snap(self):
        p = self.new()
        for key in BYKEY:
            before = self.raw(p, key)
            for value in (math.nan, math.inf, -math.inf):
                p.lib.f0r_set_param_value(p.instance, C.byref(C.c_double(value)), BYKEY[key]['index'])
                self.assertEqual(self.raw(p, key), before)
        p.set(SIZE=-100, DURATION=-1); self.assertEqual(self.raw(p, 'SIZE'), .15)
        self.assertEqual(self.raw(p, 'DURATION'), .1)
        p.set(SIZE=200, DURATION=20, STYLE=1.8)
        self.assertEqual(self.raw(p, 'SIZE'), 1); self.assertEqual(self.raw(p, 'DURATION'), 1)
        self.assertEqual(self.raw(p, 'STYLE'), 1)

    def test_presets_exact_and_reset(self):
        p = self.new(debug=True)
        expected = [dict(EDGE=0,RADIUS=4,BORDER=.5,TEAR=0,DEPTH=.7,FRAME_COLOR=(182,195,209)),
                    dict(EDGE=1,RADIUS=4,BORDER=4,TEAR=1.5,DEPTH=.12,FRAME_COLOR=(242,236,219)),
                    dict(EDGE=0,RADIUS=4,BORDER=3,TEAR=0,DEPTH=.2,FRAME_COLOR=(255,255,255))]
        for style in (0,1,2,1,0,2):
            p.set(STYLE=style)
            for key, val in (expected[style] | dict(SHADOW=35,SOFTNESS=1.6,DISTANCE=1)).items():
                actual = self.internal(p,key)
                if isinstance(val,tuple): self.assertEqual(actual,val)
                else: self.assertAlmostEqual(actual,val,10)
        for mode, values in enumerate(((0,8,0,0,0),(5,8,50,-6,3),(10,6,75,-6,3))):
            p.set(MOTION=mode)
            for key,val in zip(('SWING','PERIOD','ORGANIC','BASE_YAW','BASE_PITCH'),values):
                self.assertAlmostEqual(self.internal(p,key),val,10)

    def test_every_preset_switch_matches_new_instance(self):
        p = self.new(); src = image()
        for style in (2,1,0):
            for mode in (2,0,1):
                p.set(STYLE=style,MOTION=mode); p.render(src,1)
                q = self.new(STYLE=style,MOTION=mode)
                self.assertTrue(np.array_equal(p.render(src,3.7),q.render(src,3.7)))

    def test_rounding_survives_style_switches(self):
        p = self.new(debug=True, ROUNDING=10)
        for style in (0, 1, 2, 1, 0):
            p.set(STYLE=style)
            self.assertAlmostEqual(self.internal(p, 'RADIUS'), 10, 10)

    def test_background_and_shadow_survive_style_switches(self):
        p = self.new(debug=True, BACKGROUND=1, SHADOW=72)
        for style in (0, 1, 2, 1, 0):
            p.set(STYLE=style)
            self.assertAlmostEqual(self.internal(p, 'PLATE'), 0, 10)
            self.assertAlmostEqual(self.internal(p, 'SHADOW'), 72, 10)

    def test_transparent_background_and_shadow_intensity(self):
        src = np.zeros_like(image())
        src[24:120, 72:184] = [190, 70, 90, 255]
        src[52:92, 108:148] = [0, 0, 0, 0]
        p = self.new(ENTRANCE=0, MOTION=0, STYLE=2, ROUNDING=0, BACKGROUND=0, SHADOW=0)
        filled = p.render(src, 1)
        p.set(BACKGROUND=1)
        outline = p.render(src, 1)
        self.assertGreater(filled[:, :, 3].sum(), outline[:, :, 3].sum() * 1.08)
        self.assertGreater(np.count_nonzero(outline[:, :, 3]), 0)
        shadow_pixels = []
        for shadow in (0, 35, 100):
            p.set(SHADOW=shadow)
            shadow_pixels.append(np.count_nonzero(p.render(src, 1)[:, :, 3]))
        self.assertLess(shadow_pixels[0], shadow_pixels[1])
        self.assertLess(shadow_pixels[1], shadow_pixels[2])

    def test_entrances_and_static_mode(self):
        for style in range(3):
            p = self.new(STYLE=style,MOTION=0,DURATION=.85)
            for entrance in range(1,5):
                for corner in range(4):
                    p.set(ENTRANCE=entrance,CORNER=corner)
                    self.assertFalse(p.render(image(),0).any())
                    self.assertGreater(p.render(image(),.85)[:,:,3].sum(),0)
                    self.assertTrue(np.array_equal(p.render(image(),.85),p.render(image(),100)))
            p.set(ENTRANCE=0)
            self.assertGreater(p.render(image(),0)[:,:,3].sum(),0)

    def test_exit_reverses_selected_entrance(self):
        src = image()
        p = self.new(ENTRANCE=2, MOTION=0, EXIT_ENABLED=1, EXIT_ANIMATION=0, EXIT_DURATION=1, EXIT_AT=3)
        shown = p.render(src, 1.5)
        leaving = p.render(src, 2.5)
        gone = p.render(src, 3)
        self.assertFalse(np.array_equal(shown, leaving))
        self.assertGreater(leaving[:, :, 3].sum(), 0)
        self.assertFalse(gone.any())

    def test_seek_and_serialized_settings(self):
        p = self.new(STYLE=1,MOTION=2,SIZE=70,PLACEMENT=4,SOURCE=1)
        src=image(); expected=p.render(src,3.7)
        saved={key:self.raw(p,key) for key in BYKEY}
        for t in (15,0,.4,9): p.render(src,t)
        self.assertTrue(np.array_equal(expected,p.render(src,3.7)))
        q=self.new()
        for key,value in saved.items():q.lib.f0r_set_param_value(q.instance,C.byref(C.c_double(value)),BYKEY[key]['index'])
        self.assertTrue(np.array_equal(expected,q.render(src,3.7)))

    def test_dynamic_source_and_crop_mode(self):
        src=np.zeros_like(image()); src[12:-12,104:152]=[200,70,40,255]
        p=self.new(ENTRANCE=0,MOTION=0,SOURCE=0)
        a=p.render(src,1); yy,xx=np.where(a[:,:,3]>240)
        self.assertLess(xx.max()-xx.min(),(yy.max()-yy.min())*.6)
        p.set(SOURCE=1); a=p.render(src,1)
        src2=src.copy(); src2[20:50,20:80]=[0,255,0,255]
        b=p.render(src2,1)
        self.assertFalse(np.array_equal(a,b))
        self.assertTrue(np.array_equal(a[:,:,3],b[:,:,3]))
        self.assertTrue(np.array_equal(b,self.new(ENTRANCE=0,MOTION=0,SOURCE=1).render(src2,1)))

    def test_video_entrance_ready_and_manual_positions(self):
        first = image()
        second = np.roll(first, 31, axis=1)
        p = self.new(debug=True, ENTRANCE=1, DURATION=.85, MOTION=0, SOURCE=1, PLACEMENT=1)
        self.assertFalse(p.render(first, 0).any())
        self.assertGreater(p.render(second, .4)[:, :, 3].sum(), 0)
        left = p.render(first, 1)
        p.set(PLACEMENT=2)
        right = p.render(second, 1)
        left_x = np.where(left[:, :, 3] > 0)[1].mean()
        right_x = np.where(right[:, :, 3] > 0)[1].mean()
        self.assertGreater(right_x - left_x, 30)
        p.set(POSITION_MODE=1, X=25, Y=75)
        self.assertAlmostEqual(self.internal(p, 'PLACEMENT'), 7, 10)
        self.assertAlmostEqual(self.internal(p, 'X'), 25, 10)
        self.assertAlmostEqual(self.internal(p, 'Y'), 75, 10)

    def test_color_storage_guards(self):
        self.assertEqual(C.sizeof(Color),12)
        class Guard(C.Structure):
            _fields_=[('before',C.c_uint32),('value',Color),('after',C.c_uint32)]
        p=self.new(debug=True); guard=Guard(0x12345678,Color(),0x87654321)
        p.lib.card3d_test_get(p.instance,C.byref(guard,Guard.value.offset),ENGINE_BYKEY['FRAME_COLOR']['index'])
        self.assertEqual((guard.before,guard.after),(0x12345678,0x87654321))
        self.assertAlmostEqual(guard.value.r,182/255,6)


if __name__ == '__main__': unittest.main(verbosity=2)
