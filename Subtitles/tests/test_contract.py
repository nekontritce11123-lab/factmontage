#!/usr/bin/env python3
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    schema = json.loads((ROOT / 'generated/parameters.json').read_text(encoding='utf-8'))
    assert schema['schema'] == 1
    assert schema['output'] == 'burn-in'
    assert schema['asr']['engine'] == 'whisper.cpp'
    assert schema['asr']['language'] == 'ru'
    assert schema['asr']['network'] is False
    assert schema['asr']['word_timestamps'] is True
    assert schema['layout']['wrap'] == 'estimated-glyph-width'
    assert schema['layout']['max_duration_ms'] == 3200
    assert schema['layout']['allow_mid_word_break'] is False
    assert schema['layout']['max_lines'] == 1
    assert len(schema['parameters']) == 16
    assert len(schema['presets']) == 8
    assert [preset['id'] for preset in schema['presets']] == [
        'classic', 'clean', 'fade', 'rise', 'slide', 'reveal_words', 'letters', 'accent'
    ]
    assert len(next(item for item in schema['parameters'] if item['id'] == 'animation')['options']) == 9
    assert schema['presets'][0]['values']['background'] == 1
    parameter_ids = [item['id'] for item in schema['parameters']]
    assert len(parameter_ids) == len(set(parameter_ids))
    preset_ids = [item['id'] for item in schema['presets']]
    assert len(preset_ids) == len(set(preset_ids))
    for preset in schema['presets']:
        assert set(preset['values']) == set(parameter_ids)
    print('Studio subtitles contract: PASS')


if __name__ == '__main__':
    main()
