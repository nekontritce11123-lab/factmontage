import os
from pathlib import Path
import shutil
import subprocess
import importlib.util
import tarfile
import tempfile
import unittest


@unittest.skipIf(os.name == 'nt', 'Shell guard is exercised by the Linux source checks')
class PackageTests(unittest.TestCase):
    def test_installed_check_passes_a_valid_shell_command(self):
        script = (Path(__file__).resolve().parents[1]/'scripts/check-installed.sh').read_text(encoding='utf-8')
        start = script.index('flatpak run --user --env=QT_QPA_PLATFORM=offscreen')
        end = script.index('\nflatpak install --user --noninteractive --reinstall "$previous"', start)
        captured = subprocess.run(
            ['bash', '-c', 'flatpak() { while [ "$1" != -ec ]; do shift; done; printf "%s" "$2"; }; app=test; ' + script[start:end]],
            capture_output=True, text=True, check=True, timeout=10)
        checked = subprocess.run(['sh', '-n'], input=captured.stdout,
                                 capture_output=True, text=True, timeout=10)
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_text_source_archive_configures_with_tests_enabled(self):
        script = Path(__file__).resolve().parents[1]/'scripts/prepare.py'
        spec = importlib.util.spec_from_file_location('studio_prepare', script)
        prepare = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(prepare)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            archive = root/'studio-text-source.tar.gz'
            prepare.source_archive(archive, prepare.text_source_files())
            source = root/'source'
            source.mkdir()
            with tarfile.open(archive, 'r:gz') as content:
                content.extractall(source, filter='data')
            self.assertTrue((source/'kdenlive/sunimo_text_studio.xml').is_file())
            self.assertTrue((source/'qt/scenecompiler.hpp').is_file())
            subprocess.run(['cmake', '-S', str(source), '-B', str(root/'build'),
                            '-DBUILD_TESTING=ON', '-DSTXT_BUILD_QT_COMPILER=OFF'],
                           check=True, capture_output=True, text=True)

    def test_existing_candidate_and_invalid_version_fail_before_build_access(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            scripts = root/'Studio/scripts'
            scripts.mkdir(parents=True)
            script = scripts/'package.sh'
            shutil.copyfile(Path(__file__).resolve().parents[1]/'scripts/package.sh', script)
            candidate = root/'dist/1.0-rc9'
            candidate.mkdir(parents=True)
            marker = candidate/'keep'
            marker.write_bytes(b'immutable')
            env = dict(os.environ, STUDIO_BUILD_ROOT=str(root/'missing-build'), STUDIO_DIST_DIR=str(root/'dist'))
            for version, message in [('1.0-rc9', 'Candidate already exists'), ('../escape', 'Invalid release version')]:
                result = subprocess.run(['bash', str(script), version], env=env, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertEqual(marker.read_bytes(), b'immutable')
            self.assertEqual(list(candidate.iterdir()), [marker])


if __name__ == '__main__':
    unittest.main()
