"""The native panel must expose the renderer contract without parallel defaults."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1]/'scripts/generate_ui.py'
spec = importlib.util.spec_from_file_location('generator', path)
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class StudioSchemaTests(unittest.TestCase):
    def test_each_control_is_visible_once(self):
        schema = generator.studio_schema()
        self.assertEqual(schema['version'], 2)
        keys = [key for group in schema['groups'] for key in group['keys']]
        public = [p for p in generator.P if not p.get('internal')]
        self.assertCountEqual(keys, [p['key'] for p in public])
        self.assertEqual(len(keys), len(set(keys)))
        for control, parameter in zip(schema['controls'], public):
            for key, value in parameter.items():
                if key not in ('labels', 'descriptions'):
                    self.assertEqual(control[key], value)
            if control['kind'] == 'list':
                self.assertEqual(len(control['options']), len(parameter['labels']))
                self.assertEqual([option['value'] for option in control['options']], list(range(len(parameter['labels']))))
            if 'positions' in control:
                self.assertEqual(len(control['positions']), len(control['options']))
                self.assertEqual(len({tuple(p) for p in control['positions']}), len(control['options']))

    def test_generated_resources_are_current(self):
        for path, data in generator.outputs().items():
            self.assertEqual(path.read_text(encoding='utf-8'), data, str(path))


if __name__ == '__main__':
    unittest.main(verbosity=2)
