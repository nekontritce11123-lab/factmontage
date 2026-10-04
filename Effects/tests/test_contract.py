#!/usr/bin/env python3
import json
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def main():
    catalog = json.loads((ROOT / 'kdenlive/catalog.json').read_text(encoding='utf-8'))
    recipes = catalog['recipes']
    assert len(recipes) == 80
    assert len({item['id'] for item in recipes}) == 80
    assert len({item['category_id'] for item in recipes}) == 10
    assert all(len(item['values']) == 11 for item in recipes)
    assert all(item['values'][0] == item['slot'] / 127 for item in recipes)
    assert catalog['parameters'][10][0] == 'time_offset'
    panel = json.loads((ROOT / 'kdenlive/studiofx.json').read_text(encoding='utf-8'))
    assert panel['version'] == 2 and panel['effect'] == 'studiofx'
    assert len(panel['groups']) == 4 and len(panel['controls']) == 10
    assert len(panel['controls'][0]['options']) == 80
    assert panel['recipes'] == recipes

    xml_files = sorted((ROOT / 'kdenlive').glob('*.xml'))
    assert [path.name for path in xml_files] == ['studio_lens.xml', 'studio_vintage.xml', 'studiofx.xml']
    effect = ET.parse(ROOT / 'kdenlive/studiofx.xml').getroot()
    assert effect.attrib['id'] == 'studiofx'
    assert effect.attrib['tag'] == 'frei0r.studiofx'
    params = {node.attrib['name']: node.attrib for node in effect.findall('{*}parameter')}
    assert set(params) == {str(index) for index in range(11)} | {'threads'}
    assert params['10']['type'] == 'fixed'
    assert params['threads']['default'] == '1'
    assert 'offset' not in params['3']

    specialized = json.loads((ROOT / 'kdenlive/specialized.json').read_text(encoding='utf-8'))
    assert specialized['schema'] == 1
    assert [item['id'] for item in specialized['filters']] == ['studio_lens', 'studio_vintage']
    for item in specialized['filters']:
        parameters = item['parameters']
        assert len(parameters) >= 15
        ids = [parameter['id'] for parameter in parameters]
        assert len(ids) == len(set(ids))
        assert item['tag'] == f'frei0r.{item["id"]}'
        assert item['groups']
        assert item['presets']
        grouped_ids = [control for group in item['groups'] for control in group['controls']]
        assert sorted(grouped_ids) == sorted(ids)
        assert len(grouped_ids) == len(set(grouped_ids))
        for preset in item['presets']:
            assert set(preset['values']).issubset(ids)
    assert len(specialized['filters'][0]['parameters']) == 16
    assert len(specialized['filters'][1]['parameters']) == 17
    for item in specialized['filters']:
        generated = ET.parse(ROOT / f'kdenlive/{item["id"]}.xml').getroot()
        assert generated.attrib['id'] == item['id']
        assert generated.attrib['tag'] == item['tag']
        assert len(generated.findall('{*}parameter')) == len(item['parameters']) + 1
    print('StudioFX and specialized effects contracts: PASS')


if __name__ == '__main__':
    main()
