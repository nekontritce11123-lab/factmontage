#!/usr/bin/env python3
"""Single source of truth for the offline subtitle contract and presets."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SCHEMA = {
    'schema': 1,
    'version': '0.6.0',
    'module': 'StudioSubtitles',
    'output': 'burn-in',
    'asr': {
        'engine': 'whisper.cpp',
        'language': 'ru',
        'sample_rate': 16000,
        'channels': 1,
        'word_timestamps': True,
        'network': False,
        'cache_key': ['source_sha256', 'zone', 'model_sha256', 'contract_version'],
    },
    'layout': {
        'max_lines': 1,
        'safe_margin_percent': 8,
        'min_duration_ms': 650,
        'max_duration_ms': 3200,
        'max_reading_cps': 24,
        'wrap': 'estimated-glyph-width',
        'allow_mid_word_break': False,
    },
    'parameters': [
        {'id': 'max_width', 'label': 'Длина строки', 'kind': 'number', 'min': 45, 'max': 85, 'default': 60, 'unit': '%'},
        {'id': 'position', 'label': 'Положение', 'kind': 'choice', 'min': 0, 'max': 2, 'default': 0, 'options': ['снизу', 'по центру', 'сверху']},
        {'id': 'font', 'label': 'Шрифт', 'kind': 'font', 'default': 'DejaVu Sans'},
        {'id': 'font_size', 'label': 'Размер шрифта', 'kind': 'number', 'min': 48, 'max': 72, 'default': 64, 'unit': 'px'},
        {'id': 'bold', 'label': 'Жирный', 'kind': 'choice', 'min': 0, 'max': 1, 'default': 1, 'options': ['нет', 'да']},
        {'id': 'text_color', 'label': 'Цвет текста', 'kind': 'color', 'default': '#FFFFFFFF'},
        {'id': 'outline', 'label': 'Обводка', 'kind': 'number', 'min': 0, 'max': 20, 'default': 3, 'unit': 'px'},
        {'id': 'outline_color', 'label': 'Цвет обводки', 'kind': 'color', 'default': '#FF000000'},
        {'id': 'shadow', 'label': 'Тень', 'kind': 'number', 'min': 0, 'max': 20, 'default': 0, 'unit': 'px'},
        {'id': 'background', 'label': 'Плашка', 'kind': 'choice', 'min': 0, 'max': 2, 'default': 1, 'options': ['нет', 'сплошная', 'полупрозрачная']},
        {'id': 'background_color', 'label': 'Цвет плашки', 'kind': 'color', 'default': '#AA000000'},
        {'id': 'safe_margin', 'label': 'Безопасное поле', 'kind': 'number', 'min': 0, 'max': 25, 'default': 8, 'unit': '%'},
        {'id': 'animation', 'label': 'Анимация', 'kind': 'choice', 'min': 0, 'max': 8, 'default': 0,
         'options': ['без анимации', 'подсветка слов', 'по буквам', 'появление', 'плавно', 'подъём', 'сбоку', 'появление словами', 'мягкий акцент']},
        {'id': 'min_duration', 'label': 'Минимум фразы', 'kind': 'number', 'min': 300, 'max': 1200, 'default': 650, 'unit': 'мс'},
        {'id': 'max_duration', 'label': 'Максимум фразы', 'kind': 'number', 'min': 1500, 'max': 5000, 'default': 3200, 'unit': 'мс'},
        {'id': 'max_reading_cps', 'label': 'Скорость чтения', 'kind': 'number', 'min': 12, 'max': 35, 'default': 24, 'unit': 'зн/с'},
    ],
    'presets': [
        {'id': 'classic', 'label': 'Классика', 'description': 'Белый текст на тёмной плашке снизу.',
         'values': {'max_width': 60, 'position': 0, 'font': 'DejaVu Sans', 'font_size': 64, 'bold': 1,
                    'text_color': '#FFFFFFFF', 'outline': 3, 'outline_color': '#FF000000', 'shadow': 0,
                    'background': 1, 'background_color': '#AA000000', 'safe_margin': 8, 'animation': 0,
                    'min_duration': 650, 'max_duration': 3200, 'max_reading_cps': 24}},
        {'id': 'clean', 'label': 'Обводка без плашки', 'description': 'Текст с обводкой без фона.',
         'values': {'max_width': 70, 'position': 0, 'font': 'DejaVu Sans', 'font_size': 64, 'bold': 1,
                    'text_color': '#FFFFFFFF', 'outline': 5, 'outline_color': '#FF000000', 'shadow': 2,
                    'background': 0, 'background_color': '#AA000000', 'safe_margin': 8, 'animation': 0,
                    'min_duration': 650, 'max_duration': 3200, 'max_reading_cps': 24}},
        {'id': 'fade', 'label': 'Плавное появление', 'description': 'Фраза плавно проявляется.',
         'values': {'max_width': 60, 'position': 0, 'font': 'DejaVu Sans', 'font_size': 64, 'bold': 1,
                    'text_color': '#FFFFFFFF', 'outline': 3, 'outline_color': '#FF000000', 'shadow': 0,
                    'background': 1, 'background_color': '#AA000000', 'safe_margin': 8, 'animation': 4,
                    'min_duration': 650, 'max_duration': 3200, 'max_reading_cps': 24}},
    ],
}

_base_style = SCHEMA['presets'][0]['values']
for _id, _label, _description, _animation, _overrides in (
    ('rise', 'Мягкий подъём', 'Фраза плавно поднимается на место.', 5, {}),
    ('slide', 'Боковое скольжение', 'Фраза входит сбоку.', 6, {'background': 0}),
    ('reveal_words', 'Появление словами', 'Слова появляются по меткам речи.', 7, {'background': 0}),
    ('letters', 'Печать буквами', 'Буквы появляются последовательно.', 2, {'background': 0}),
    ('accent', 'Мягкий акцент', 'Фраза мягко увеличивается в начале.', 8, {}),
):
    SCHEMA['presets'].append({
        'id': _id, 'label': _label, 'description': _description,
        'values': {**_base_style, 'animation': _animation, **_overrides},
    })


def generated():
    header = [
        '// Generated by scripts/generate_ui.py. Do not edit.',
        '#pragma once',
        'namespace studio_subtitles::contract {',
        f'inline constexpr int schema = {SCHEMA["schema"]};',
        f'inline constexpr const char *version = "{SCHEMA["version"]}";',
        'inline constexpr const char *output = "burn-in";',
        'inline constexpr const char *language = "ru";',
        'inline constexpr int sample_rate = 16000;',
        'inline constexpr int channels = 1;',
        'inline constexpr bool word_timestamps = true;',
        'inline constexpr bool network = false;',
        'inline constexpr int max_lines = 1;',
        'inline constexpr int safe_margin_percent = 8;',
        'inline constexpr int min_duration_ms = 650;',
        'inline constexpr int max_duration_ms = 3200;',
        'inline constexpr int max_reading_cps = 24;',
        f'inline constexpr int parameter_count = {len(SCHEMA["parameters"])};',
        f'inline constexpr int preset_count = {len(SCHEMA["presets"])};',
        '}',
        '',
    ]
    return {
        'parameters.json': json.dumps(SCHEMA, ensure_ascii=False, indent=2) + '\n',
        'subtitle_parameters.hpp': '\n'.join(header),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    target = ROOT / 'generated'
    target.mkdir(parents=True, exist_ok=True)
    for name, content in generated().items():
        path = target / name
        if args.check:
            if not path.exists() or path.read_text(encoding='utf-8') != content:
                raise SystemExit(f'Outdated: {path}')
        else:
            path.write_text(content, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
