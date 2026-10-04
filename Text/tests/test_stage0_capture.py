# SPDX-License-Identifier: MIT
"""Tests for the stage-0 evidence collector, not product acceptance."""
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'capture_stage0', Path(__file__).resolve().parents[1] / 'tools' / 'capture_stage0.py')
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


def scene(*, version=2, cfg_count=32, metadata=None):
    metadata = {'text': 'Ёж и е\u0308лка\nВторая строка'} if metadata is None else metadata
    meta = json.dumps(metadata, ensure_ascii=False).encode('utf-8')
    cfg = [0.] * cfg_count
    if cfg_count == 32:
        for index, value in {0:8,1:1,2:1,8:3,11:.35,12:1,18:2,23:60,24:1,28:96}.items():
            cfg[index] = value
    return (struct.pack('<8s6I', b'SUNMTX1\0', version, len(meta), 1, 1920, 1080, cfg_count)
            + struct.pack(f'<{cfg_count}f', *cfg) + meta
            + struct.pack('<6f6I', 0,0,1,1,.5,.5,1,1,0,0,0,4) + bytes([255]*4))


class SceneEvidenceTests(unittest.TestCase):
    def test_reports_binary_config_and_cyrillic_metadata(self):
        result = probe.inspect_scene(scene())
        self.assertIsInstance(result, dict)
        self.assertEqual(result['reference'], [1920,1080])
        self.assertEqual(result['sprite_count'], 1)
        self.assertEqual(result['config']['grouping'], 3)
        self.assertAlmostEqual(result['config']['lifeAmount'], .35)
        self.assertEqual(result['metadata']['text'], 'Ёж и е\u0308лка\nВторая строка')

    def test_rejects_truncated_header(self):
        with self.assertRaisesRegex(ValueError, 'header'):
            probe.inspect_scene(b'SUNMTX1\0')

    def test_rejects_wrong_magic(self):
        with self.assertRaisesRegex(ValueError, 'magic'):
            probe.inspect_scene(b'WRONG!!!' + scene()[8:])

    def test_refuses_to_guess_another_stxt_layout(self):
        with self.assertRaisesRegex(ValueError, 'version'):
            probe.inspect_scene(scene(version=3))

    def test_rejects_wrong_config_length(self):
        with self.assertRaisesRegex(ValueError, 'config'):
            probe.inspect_scene(scene(cfg_count=31))

    def test_rejects_metadata_not_an_object(self):
        with self.assertRaisesRegex(ValueError, 'object'):
            probe.inspect_scene(scene(metadata=[]))

    def test_rejects_truncated_sprite_payload(self):
        with self.assertRaisesRegex(ValueError, 'truncated'):
            probe.inspect_scene(scene()[:-1])

    def test_rejects_nonfinite_config(self):
        data = bytearray(scene())
        struct.pack_into('<f', data, 32+11*4, float('nan'))
        with self.assertRaisesRegex(ValueError, 'finite'):
            probe.inspect_scene(bytes(data))

    def test_binary_metadata_mismatch_is_not_pass(self):
        result = probe.inspect_scene(scene(metadata={'lifeSource':46,'lifeAmount':.35}))
        self.assertIsInstance(result, dict)
        self.assertEqual(probe.setting_mismatches(result, {'lifeSource':46,'lifeAmount':.35}), ['lifeSource'])

    def test_float32_rounding_is_not_a_mismatch(self):
        result = probe.inspect_scene(scene(metadata={'lifeSource':2,'lifeAmount':.35}))
        self.assertIsInstance(result, dict)
        self.assertEqual(probe.setting_mismatches(result, {'lifeSource':2,'lifeAmount':.35}), [])


class CaseTests(unittest.TestCase):
    def catalog(self):
        return [{'id':2,'life':{'name':'Парение после подъёма'}},
                {'id':46,'life':{'name':'Лента на воздухе'}},
                {'id':10,'life':{'name':'Дыхание интервала'}}]

    def test_exact_control_contract_and_stable_recipe_ids(self):
        cases = probe.control_cases(self.catalog())
        self.assertIsInstance(cases, list)
        self.assertEqual([c['settings']['lifeSource'] for c in cases], [0,2,46,10])
        self.assertEqual([c['settings']['lifeAmount'] for c in cases], [0,.35,.35,.35])
        for case in cases:
            self.assertEqual(case['settings']['duration'], 8)
            self.assertEqual(case['settings']['fpsNum'], 60)
            self.assertEqual(case['settings']['size'], 96)
            self.assertEqual(case['settings']['inDuration'], 1)
            self.assertEqual(case['settings']['outDuration'], 1)
            self.assertEqual(case['settings']['text'].count('\n'), 1)

    def test_missing_recipe_is_not_silently_replaced(self):
        with self.assertRaisesRegex(ValueError, 'recipe'):
            probe.control_cases([])

    def test_duplicate_recipe_is_not_silently_chosen(self):
        with self.assertRaisesRegex(ValueError, 'recipe'):
            probe.control_cases(self.catalog() + self.catalog()[:1])


class ProcessEvidenceTests(unittest.TestCase):
    def test_failure_keeps_output_and_exit_code(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d)/'failure.log'
            result = probe.run_logged([sys.executable,'-c','print("failure evidence");raise SystemExit(7)'], log, 3)
            self.assertIsInstance(result, dict)
            self.assertEqual(result['status'], 'FAIL')
            self.assertEqual(result['returncode'], 7)
            self.assertIn('failure evidence', log.read_text())

    def test_timeout_is_not_pass(self):
        with tempfile.TemporaryDirectory() as d:
            result = probe.run_logged([sys.executable,'-c','import time;time.sleep(10)'], Path(d)/'timeout.log', .1)
            self.assertIsInstance(result, dict)
            self.assertEqual(result['status'], 'TIMEOUT')

    def test_missing_executable_is_not_run(self):
        with tempfile.TemporaryDirectory() as d:
            result = probe.run_logged([str(Path(d)/'missing-compiler')], Path(d)/'missing.log', 1)
            self.assertIsInstance(result, dict)
            self.assertEqual(result['status'], 'NOT RUN')

    def test_success_has_actual_command(self):
        with tempfile.TemporaryDirectory() as d:
            command = [sys.executable,'-c','print("ok")']
            result = probe.run_logged(command, Path(d)/'ok.log', 3)
            self.assertIsInstance(result, dict)
            self.assertEqual(result['status'], 'PASS')
            self.assertEqual(result['command'], command)


if __name__ == '__main__':
    unittest.main()
