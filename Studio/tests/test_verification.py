import importlib.util
import hashlib
import io
import json
import tarfile
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('verification', Path(__file__).resolve().parents[1]/'scripts/verification.py')
verification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verification)


class VerificationTests(unittest.TestCase):
    def test_corresponding_upstream_archive_rejects_a_tampered_copy(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            upstream = root / 'kdenlive.tar.xz'
            upstream.write_bytes(b'pinned upstream')
            lock = json.dumps({'archive_sha256': hashlib.sha256(upstream.read_bytes()).hexdigest()}).encode()
            prepared = root / 'verified-changes.tar.gz'
            with tarfile.open(prepared, 'w:gz') as archive:
                entry = tarfile.TarInfo('Studio/upstream/sources.json')
                entry.size = len(lock)
                archive.addfile(entry, io.BytesIO(lock))
            verification.verify_upstream_archive(prepared, upstream)
            upstream.write_bytes(b'replaced after the build')
            with self.assertRaisesRegex(ValueError, 'Pinned Kdenlive source archive'):
                verification.verify_upstream_archive(prepared, upstream)

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
