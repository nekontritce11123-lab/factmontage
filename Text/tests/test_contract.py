"""Source/install contract. Does not claim Kdenlive GUI acceptance."""
from pathlib import Path
import importlib.util
import os
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.native import Native

class TextContract(unittest.TestCase):
    def test_descriptor_installed_and_neutral_offset(self):
        cmake = (ROOT/'CMakeLists.txt').read_text(encoding='utf-8')
        self.assertIn('kdenlive/sunimo_text_studio.xml DESTINATION share/kdenlive/effects', cmake)
        effect = ET.parse(ROOT/'kdenlive/sunimo_text_studio.xml').getroot()
        self.assertEqual(effect.get('tag'), 'frei0r.sunimo_text_studio')
        offset = effect.find("parameter[@name='2']")
        self.assertEqual(offset.get('type'), 'fixed')
        self.assertEqual(float(offset.get('default')), .5)
        self.assertNotIn('offset', offset.attrib)
        self.assertEqual((float(offset.get('default'))-.5)*120, 0)

    def test_explicit_missing_binary_never_falls_back_to_bin(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(RuntimeError, 'старый бинарник'):
                Native(Path(tmp)/'missing.so')

    def test_importing_generator_never_writes_files(self):
        paths = [ROOT/'web/catalog.json', ROOT/'src/recipes.inc', ROOT/'src/life_recipes.inc']
        before = [(p.read_bytes(), p.stat().st_mtime_ns) for p in paths]
        spec = importlib.util.spec_from_file_location('text_catalog_import_check', ROOT/'tools/make_catalog.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertEqual(before, [(p.read_bytes(), p.stat().st_mtime_ns) for p in paths])

if __name__ == '__main__':
    unittest.main()
