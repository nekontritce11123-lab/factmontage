import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('verification', Path(__file__).resolve().parents[1]/'scripts/verification.py')
verification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verification)


class VerificationTests(unittest.TestCase):
    def test_changed_source_or_binary_invalidates_evidence(self):
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            for name in ('studio-input/source', 'studio/files/binary', 'test-results/log'):
                path = work/name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'original')
            (work/'build-started.json').write_text(json.dumps(verification.inventory(work/'studio-input')), encoding='utf-8')
            (work/'studio/metadata').write_bytes(b'permissions')
            verification.record(work, 'build')
            verification.record(work, 'tests')
            for name in ('studio-input/source', 'studio/files/binary', 'studio/metadata'):
                path = work/name
                original = path.read_bytes()
                path.write_bytes(b'changed')
                with self.assertRaises(ValueError):
                    verification.verify(work, 'tests')
                with self.assertRaises(ValueError):
                    verification.record(work, 'tests')
                path.write_bytes(original)
            verification.verify(work, 'tests')
            (work/'test-results/log').write_bytes(b'changed report')
            with self.assertRaises(ValueError):
                verification.verify(work, 'tests')
            (work/'test-results/log').write_bytes(b'original')
            (work/'studio-input/new-file').write_bytes(b'new')
            with self.assertRaises(ValueError):
                verification.verify(work, 'tests')


if __name__ == '__main__':
    unittest.main()
