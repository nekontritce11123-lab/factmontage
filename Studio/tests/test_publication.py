"""Checks for the standalone source export used by the public repository."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('studio_public_export', ROOT / 'Studio/scripts/export_public.py')
export = importlib.util.module_from_spec(spec)
spec.loader.exec_module(export)


class PublicSourceTests(unittest.TestCase):
    def test_export_preserves_build_sources_without_private_history(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / 'public'
            files = export.export_sources(target)
            for module in export.ARCHIVES.values():
                self.assertIn(module + '/CMakeLists.txt', files)
                self.assertIn(module + '/LICENSE', files)
            for name in ('LICENSE', 'README.md', 'README_RU.md', 'THIRD_PARTY_NOTICES.md',
                         'Studio/overlay/studiopanel.cpp', 'Studio/scripts/prepare.py',
                         'Studio/patches/whisper-vad-json-timestamps.patch',
                         'Background/scripts/export_humanseg_onnx.py', 'Studio/tests/test_publication.py',
                         'Text/studio.py', 'Text/web/index.html', 'Studio/tests/public_demo.hpp'):
                self.assertIn(name, files)
                self.assertEqual((target / name).read_bytes(), (ROOT / name).read_bytes())
            forbidden = {'.git', '.beads', '.build', 'archive', 'releases'}
            self.assertFalse(any(Path(name).parts[0] in forbidden for name in files))
            self.assertNotIn('docs/TEST_REPORT_RU.md', files)
            self.assertNotIn('docs/STATUS.md', files)
            self.assertFalse(any(name.endswith(('.flatpak', '.dll', '.so', '.exe')) for name in files))

    def test_existing_destination_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder)
            marker = target / 'keep'
            marker.write_bytes(b'untouched')
            with self.assertRaises(ValueError):
                export.export_sources(target)
            self.assertEqual(marker.read_bytes(), b'untouched')
            self.assertEqual(list(target.iterdir()), [marker])

    def test_archive_paths_cannot_escape_the_export(self):
        for name in ('../secret', '/absolute', 'Camera/../../secret', 'C:/secret', 'a\\b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                export.validate_name(name)
        self.assertEqual(export.validate_name('Text/src/plugin.cpp'), 'Text/src/plugin.cpp')


if __name__ == '__main__':
    unittest.main()
