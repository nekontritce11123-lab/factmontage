"""Behavioural tests of the offline runner; no SDK, GUI, or external network."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('studio_dev', ROOT/'Studio/scripts/dev.py')
dev = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = dev
spec.loader.exec_module(dev)
host_spec = importlib.util.spec_from_file_location('studio_host_dev', ROOT/'Studio/scripts/host_dev.py')
host_dev = importlib.util.module_from_spec(host_spec)
host_spec.loader.exec_module(host_dev)

class SelectionTests(unittest.TestCase):
    def setUp(self):
        self.modules = dev.load_modules()

    def test_registry_covers_every_module(self):
        found = {p.name for p in ROOT.iterdir() if p.is_dir() and (p/'CMakeLists.txt').exists() and p.name != 'Studio'}
        self.assertEqual(found, {m['path'] for m in self.modules.values()})
        self.assertIn('text', self.modules)

    def test_reverse_dependency_and_exact_module(self):
        self.assertEqual(dev.affected_modules(['Audio/src/audio.cpp'], self.modules), ['audio', 'subtitles'])
        self.assertEqual(dev.affected_modules(['Text/src/engine.hpp'], self.modules), ['text'])
        self.assertEqual(dev.affected_modules(['Text/CMakeLists.txt'], self.modules), ['text'])
        self.assertEqual(dev.change_scopes(['Text/qt/scenecompiler.hpp'], self.modules), (['text'], True, False))
        self.assertEqual(dev.change_scopes(['Text/qt/styles.hpp'], self.modules), (['text'], True, False))
        self.assertEqual(dev.affected_modules(['Studio/overlay/core/resource.cpp'], self.modules), [])
        self.assertEqual(dev.change_scopes(['Studio/overlay/core/resource.cpp'], self.modules)[1:], (True, False))
        self.assertEqual(dev.affected_modules(['Unknown/engine.cpp'], self.modules), list(self.modules))
        self.assertEqual(dev.affected_modules(['docs/STATUS.md', 'README.md', '.beads/config.yaml'], self.modules), [])
        self.assertEqual(dev.change_scopes(['Studio/scripts/prepare.py'], self.modules), ([], False, True))
        self.assertEqual(dev.change_scopes(['Studio/tests/studioregressiontest.cpp'], self.modules), ([], True, False))
        self.assertEqual(dev.change_scopes(['dev'], self.modules), ([], False, True))

    def test_host_only_change_cannot_report_native_pass_as_integration(self):
        reports = []
        with patch.object(dev, 'changed_paths', return_value=['Studio/overlay/studiopanel.cpp']), \
             patch.object(dev, 'write_report', lambda runner, selected, status, command: reports.append(status)):
            self.assertEqual(dev.main(['test', 'changed']), 3)
        self.assertEqual(reports, ['HOST INTEGRATION NOT RUN'])

    def test_tooling_change_runs_preparation_without_native_build(self):
        calls = []
        reports = []
        with patch.object(dev, 'changed_paths', return_value=['Studio/scripts/prepare.py']), \
             patch.object(dev.Runner, 'run', lambda self, command, **kwargs: calls.append([str(part) for part in command])), \
             patch.object(dev, 'write_report', lambda runner, selected, status, command: reports.append((selected, status))):
            self.assertEqual(dev.main(['test', 'changed']), 0)
        self.assertEqual(reports, [([], 'TOOLING CHECKS PASS')])
        self.assertTrue(any('test_preparation.py' in ' '.join(command) for command in calls))

    def test_missing_base_is_error_not_empty_green(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(dev.DevError):
                dev.changed_paths('missing', Path(folder))

    def test_git_selection_includes_staged_unstaged_untracked_and_committed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def git(*args):
                return subprocess.check_output(['git', *args], cwd=root, stderr=subprocess.PIPE).decode().strip()
            git('init', '-q'); git('config', 'user.name', 'test'); git('config', 'user.email', 'test@example.invalid')
            (root/'base').write_text('old'); git('add', '.'); git('commit', '-qm', 'base')
            base = git('rev-parse', 'HEAD')
            (root/'Text').mkdir(); (root/'Text/new.cpp').write_text('committed')
            git('add', '.'); git('commit', '-qm', 'text')
            (root/'base').write_text('unstaged')
            (root/'Audio').mkdir(); (root/'Audio/new.cpp').write_text('staged'); git('add', 'Audio')
            (root/'space name.cpp').write_text('untracked')
            paths = dev.changed_paths(base, root)
            self.assertEqual(paths, ['Audio/new.cpp', 'Text/new.cpp', 'base', 'space name.cpp'])
            self.assertNotIn('Text/new.cpp', dev.changed_paths(None, root))

    def test_test_plan_has_no_network_prepare_or_baseline(self):
        calls = []
        with patch.object(dev.Runner, 'run', lambda self, command, **kwargs: calls.append([str(c) for c in command])):
            dev.main(['test', 'all', '--dry-run'])
        flat = '\n'.join(' '.join(c) for c in calls).replace('\\', '/')
        for forbidden in ('fetch-upstream', 'prepare.py', 'flatpak', 'pip install', 'urlretrieve', 'baseline'):
            self.assertNotIn(forbidden, flat)
        self.assertIn('Text/scripts/generate_ui.py', flat)
        self.assertIn('--no-tests=error', flat)

class RunnerTests(unittest.TestCase):
    def test_build_is_not_reported_as_test_acceptance(self):
        reports = []
        with patch.object(dev, 'build_module', return_value=Path('/unused')) as build, \
             patch.object(dev, 'write_report', lambda runner, selected, status, command: reports.append((status, command))):
            dev.main(['build', 'text'])
        build.assert_called_once()
        self.assertEqual(reports, [('BUILT — TESTS NOT RUN', 'build')])
        with self.assertRaises(dev.DevError):
            dev.main(['build', 'text', '--contracts-only'])

    def test_failure_and_timeout_preserve_logs_and_nonzero_status(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runner = dev.Runner(root, .2)
            with self.assertRaises(dev.DevError):
                runner.run([sys.executable, '-S', '-c', 'print("original failure", flush=True); raise SystemExit(7)'], timeout=5)
            self.assertEqual(runner.results[-1].returncode, 7)
            self.assertIn('original failure', Path(runner.results[-1].log).read_text())
            with self.assertRaises(dev.DevError):
                runner.run([sys.executable, '-c', 'import time; time.sleep(10)'])
            self.assertEqual(runner.results[-1].status, 'TIMEOUT')
            self.assertLess(runner.results[-1].seconds, 4)
            dev.write_report(runner, ['text'], 'FAIL', 'test')
            self.assertEqual(json.loads((root/'report.json').read_text())['status'], 'FAIL')

    def test_missing_executable_does_not_certify(self):
        with tempfile.TemporaryDirectory() as folder:
            runner = dev.Runner(Path(folder), 1)
            with self.assertRaises(dev.DevError):
                runner.run([str(Path(folder)/'no-such-executable')])
            self.assertEqual(runner.results[0].status, 'ERROR')

    def test_dry_run_creates_no_build_or_report(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'untouched'
            runner = dev.Runner(path, 1, True)
            runner.run(['definitely-not-a-command'])
            dev.write_report(runner, ['text'], 'DRY RUN', 'test')
            self.assertFalse(path.exists())

    def test_host_sync_reuses_unchanged_source_and_rejects_external_edit(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            cache, build = root/'cache', root/'build'
            cache.mkdir()
            build.mkdir()
            source = root/'studio.cpp'
            source.write_text('first', encoding='utf-8')
            files = {'src/assets/studio/studio.cpp': source}
            self.assertEqual(host_dev.sync_files(cache, build, files), list(files))
            target = build/'src/assets/studio/studio.cpp'
            timestamp = target.stat().st_mtime_ns
            self.assertEqual(host_dev.sync_files(cache, build, files), [])
            self.assertEqual(target.stat().st_mtime_ns, timestamp)
            source.write_text('second', encoding='utf-8')
            self.assertEqual(host_dev.sync_files(cache, build, files), list(files))
            self.assertEqual(target.read_text(encoding='utf-8'), 'second')
            target.write_text('external edit', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'outside dev loop'):
                host_dev.sync_files(cache, build, files)

    def test_host_binary_is_reused_only_for_the_same_inputs_and_bytes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            binary = root/'build/bin/kdenlive'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b'compiled')
            context = {'work': root, 'build': root/'build', 'app_fingerprint': 'input-a'}
            self.assertFalse(host_dev.app_ready(context))
            (root/'dev-host').mkdir()
            host_dev.record_app(context)
            self.assertTrue(host_dev.app_ready(context))
            binary.write_bytes(b'stale')
            with self.assertRaisesRegex(ValueError, 'outside dev loop'):
                host_dev.app_ready(context)
            binary.write_bytes(b'compiled')
            context['app_fingerprint'] = 'input-b'
            self.assertFalse(host_dev.app_ready(context))
            test = root/'build/bin/studioregressiontest'
            test.write_bytes(b'compiled test')
            context['fingerprint'] = 'test-input'
            host_dev.record_test_binary(context, 'studioregressiontest')
            test.write_bytes(b'stale test')
            with self.assertRaisesRegex(ValueError, 'outside dev loop'):
                host_dev.check_test_binary(context, 'studioregressiontest')

if __name__ == '__main__':
    unittest.main()
