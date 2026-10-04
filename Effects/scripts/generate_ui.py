#!/usr/bin/env python3
"""Single source of truth for the native ABI, 80 recipes and Kdenlive XML.
Mode slots use an immutable denominator 127, NOT (number of presets - 1).
Generated files are deterministic and must not be edited by hand.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import tempfile
import xml.etree.ElementTree as ET
ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.2.0'
PARAMS = [
 ('recipe', 'Шаблон', 0.0), ('amount', 'Сила', .60), ('size', 'Размер', .50),
 ('speed', 'Скорость', .5), ('mix', 'Смешивание', 1.0),
 ('center_x', 'Центр по горизонтали', .50), ('center_y', 'Центр по вертикали', .50),
 ('softness', 'Мягкость', .65), ('animate', 'Анимация', 1.0),
 ('seed', 'Вариант движения', .137),
 ('time_offset', 'Смещение времени', 0.0),
]
# Each row: Russian title, algorithm, variant, amount, size, softness, animate.
GROUPS = [
 ('vignette', 'Виньетки', [
 ('Мягкая киношная','VIGNETTE',0,.55,.56,.75,0),
 ('Глубокие края','VIGNETTE',0,.88,.39,.65,0),
 ('Круглый акцент','VIGNETTE',1,.70,.47,.55,0),
 ('Вертикальный акцент','VIGNETTE',2,.67,.49,.70,0),
 ('Акцент сбоку','VIGNETTE',0,.65,.52,.70,0),
 ('Белая дымка','VIGNETTE',3,.46,.50,.88,0),
 ('Тёплые края','VIGNETTE',4,.42,.55,.82,0),
 ('Туннель','VIGNETTE',1,.97,.25,.24,0)]),
 ('tv', 'Телевизор и VHS', [
 ('Чистый CRT','TV',0,.42,.38,.65,0),
 ('Тёплый телевизор','TV',1,.54,.42,.65,0),
 ('Домашняя VHS','TV',2,.46,.47,.65,1),
 ('Изношенная кассета','TV',3,.70,.62,.65,1),
 ('Чёрно-белый телевизор','TV',4,.70,.46,.65,1),
 ('Сбитая синхронизация','TV',5,.50,.40,.65,1),
 ('Камера наблюдения','TV',6,.51,.42,.65,1),
 ('Старая веб-камера','TV',7,.59,.58,.65,1)]),
 ('lens','Линзы', [
 ('Мягкий широкий угол','LENS',0,.30,.82,.70,0),
 ('Экшн-рыбий глаз','LENS',0,.75,.91,.70,0),
 ('Пузырь','LENS',1,.68,.46,.72,0),
 ('Дверной глазок','LENS',2,.90,.82,.36,0),
 ('Вогнутая линза','LENS',3,.63,.53,.72,0),
 ('Горизонтальный цилиндр','LENS',4,.54,.59,.72,0),
 ('Вертикальный цилиндр','LENS',5,.54,.59,.72,0),
 ('Дышащая линза','LENS',6,.50,.63,.75,1)]),
 ('fun','Смешные деформации', [
 ('Широкое лицо','DEFORM',0,.56,.48,.78,0),
 ('Узкое лицо','DEFORM',1,.54,.48,.78,0),
 ('Длинное лицо','DEFORM',2,.50,.49,.78,0),
 ('Большой нос','LENS',1,.84,.24,.72,0),
 ('Сплющило','DEFORM',3,.56,.56,.72,0),
 ('Желе','DEFORM',4,.51,.54,.78,1),
 ('Кривое зеркало','DEFORM',5,.53,.58,.78,1),
 ('Воронка','DEFORM',6,.58,.54,.78,0)]),
 ('mirror','Зеркала', [
 ('Зеркало слева','MIRROR',0,1,.5,.65,0),
 ('Зеркало справа','MIRROR',1,1,.5,.65,0),
 ('Зеркало сверху','MIRROR',2,1,.5,.65,0),
 ('Зеркало снизу','MIRROR',3,1,.5,.65,0),
 ('Диагональное зеркало','MIRROR',4,1,.5,.65,0),
 ('Калейдоскоп ×4','KALEIDO',4,1,.60,.65,0),
 ('Калейдоскоп ×8','KALEIDO',8,1,.60,.65,0),
 ('Вращающийся калейдоскоп','KALEIDO',6,1,.60,.65,1)]),
 ('glitch','Цифровые сбои', [
 ('Лёгкая хроматика','RGB',0,.28,.47,.65,0),
 ('RGB-раздвоение','RGB',1,.65,.56,.65,0),
 ('Сдвиг блоков','GLITCH',0,.55,.57,.65,1),
 ('Разрыв строки','GLITCH',1,.59,.50,.65,1),
 ('Цифровые помехи','GLITCH',2,.55,.54,.65,1),
 ('Цветовая волна','RGB',2,.55,.53,.65,1),
 ('Плохой кодек — стилизация','GLITCH',3,.62,.65,.65,1),
 ('Редкий сбой','GLITCH',4,.58,.45,.65,1)]),
 ('film','Плёнка и фактура', [
 ('Мелкое зерно','GRAIN',0,.37,.20,.65,1),
 ('Крупное зерно','GRAIN',0,.58,.72,.65,1),
 ('Домашняя Super 8','GRAIN',1,.55,.43,.65,1),
 ('Пыль','DUST',0,.57,.42,.65,1),
 ('Царапины','DUST',1,.57,.46,.65,1),
 ('Дрожание плёнки','GRAIN',2,.48,.45,.65,1),
 ('Старый проектор','GRAIN',3,.61,.51,.65,1),
 ('Выцветшая эмульсия','GRAIN',4,.55,.30,.65,1)]),
 ('light','Свечение и свет', [
 ('Мягкое свечение','GLOW',0,.46,.42,.65,0),
 ('Сонная диффузия','GLOW',1,.60,.68,.65,0),
 ('Тёплый ореол','GLOW',2,.52,.54,.65,0),
 ('Неоновое свечение','GLOW',3,.61,.45,.65,0),
 ('Горизонтальные лучи','GLOW',4,.52,.62,.65,0),
 ('Вертикальные лучи','GLOW',5,.52,.62,.65,0),
 ('Свет сбоку','LEAK',0,.49,.61,.74,1),
 ('Призма','LEAK',1,.45,.55,.74,1)]),
 ('pixel','Пиксели и печать', [
 ('Крупные пиксели','PIXEL',0,.95,.78,.65,0),
 ('Мелкие пиксели','PIXEL',0,.90,.28,.65,0),
 ('Мозаика','PIXEL',1,.88,.67,.65,0),
 ('Четыре цвета','PALETTE',0,.95,.5,.65,0),
 ('Газетная печать','HALFTONE',0,.92,.45,.65,0),
 ('Ретро-дизеринг','PALETTE',1,.95,.46,.65,0),
 ('Постеризация','PALETTE',2,.92,.49,.65,0),
 ('Цветная печать','HALFTONE',1,.86,.51,.65,0)]),
 ('draw','Рисунок и контуры', [
 ('Карандаш','EDGE',0,.89,.42,.65,0),
 ('Уголь','EDGE',1,.90,.65,.65,0),
 ('Комикс','EDGE',2,.83,.44,.65,0),
 ('Тушь','EDGE',3,.91,.52,.65,0),
 ('Ксерокопия','EDGE',4,.92,.55,.65,0),
 ('Неоновые контуры','EDGE',5,.90,.46,.65,0),
 ('Тиснение','EMBOSS',0,.87,.51,.65,0),
 ('Гравюра','EMBOSS',1,.91,.48,.65,0)]),
]
ALGORITHMS = list(dict.fromkeys(row[1] for _,_,rows in GROUPS for row in rows))

# Contracts for the specialized filters are kept beside the existing StudioFX
# catalog, but are not installed until their native implementations exist.
SPECIALIZED = [
    {
        'id': 'studio_lens',
        'tag': 'frei0r.studio_lens',
        'label': 'Оптический глазок',
        'version': '0.1.0',
        'groups': [
            {'id': 'projection', 'label': 'Проекция', 'controls': ['projection', 'strength']},
            {'id': 'aperture', 'label': 'Апертурное окно', 'controls': ['shape', 'radius', 'aspect', 'center_x', 'center_y']},
            {'id': 'edge', 'label': 'Края и оптика', 'controls': ['edge_darkness', 'edge_softness', 'overscan', 'edge_blur', 'sharpness_falloff']},
            {'id': 'color', 'label': 'Цвет и свечение', 'controls': ['aberration', 'bloom', 'contrast', 'mix']},
        ],
        'parameters': [
            {'id': 'projection', 'label': 'Тип проекции', 'kind': 'choice', 'min': 0, 'max': 2, 'default': 1, 'options': ['широкий угол', 'рыбий глаз', 'глазок']},
            {'id': 'strength', 'label': 'Сила искажения', 'kind': 'number', 'min': 0, 'max': 100, 'default': 78, 'unit': '%'},
            {'id': 'shape', 'label': 'Форма апертуры', 'kind': 'choice', 'min': 0, 'max': 2, 'default': 0, 'options': ['круг', 'овал', 'прямоугольник']},
            {'id': 'radius', 'label': 'Радиус окна', 'kind': 'number', 'min': 0, 'max': 100, 'default': 82, 'unit': '%'},
            {'id': 'aspect', 'label': 'Соотношение сторон', 'kind': 'number', 'min': 0, 'max': 100, 'default': 50, 'unit': '%'},
            {'id': 'center_x', 'label': 'Центр X', 'kind': 'number', 'min': 0, 'max': 100, 'default': 50, 'unit': '%'},
            {'id': 'center_y', 'label': 'Центр Y', 'kind': 'number', 'min': 0, 'max': 100, 'default': 50, 'unit': '%'},
            {'id': 'edge_darkness', 'label': 'Затемнение вне окна', 'kind': 'number', 'min': 0, 'max': 100, 'default': 100, 'unit': '%'},
            {'id': 'edge_softness', 'label': 'Мягкость границы', 'kind': 'number', 'min': 0, 'max': 100, 'default': 36, 'unit': '%'},
            {'id': 'overscan', 'label': 'Запас кадрирования', 'kind': 'number', 'min': 0, 'max': 100, 'default': 16, 'unit': '%'},
            {'id': 'edge_blur', 'label': 'Размытие краёв', 'kind': 'number', 'min': 0, 'max': 100, 'default': 28, 'unit': '%'},
            {'id': 'sharpness_falloff', 'label': 'Падение резкости', 'kind': 'number', 'min': 0, 'max': 100, 'default': 34, 'unit': '%'},
            {'id': 'aberration', 'label': 'Хроматическая аберрация', 'kind': 'number', 'min': 0, 'max': 100, 'default': 12, 'unit': '%'},
            {'id': 'bloom', 'label': 'Свечение', 'kind': 'number', 'min': 0, 'max': 100, 'default': 22, 'unit': '%'},
            {'id': 'contrast', 'label': 'Контраст', 'kind': 'number', 'min': 0, 'max': 100, 'default': 46, 'unit': '%'},
            {'id': 'mix', 'label': 'Смешивание', 'kind': 'number', 'min': 0, 'max': 100, 'default': 100, 'unit': '%'},
        ],
        'presets': [
            {'id': 'peephole', 'label': 'Дверной глазок', 'description': 'Круглая апертура, тёмные края и мягкая оптика.', 'values': {'projection': 2, 'strength': 90, 'shape': 0, 'radius': 82, 'edge_darkness': 100, 'edge_softness': 36, 'overscan': 16, 'edge_blur': 28, 'sharpness_falloff': 34, 'aberration': 12, 'bloom': 22, 'contrast': 46, 'mix': 100}},
            {'id': 'fisheye', 'label': 'Круглый fisheye', 'description': 'Сильное искажение с сохранением круглой границы.', 'values': {'projection': 1, 'strength': 78, 'shape': 0, 'radius': 88, 'edge_darkness': 82, 'edge_softness': 28, 'overscan': 22, 'edge_blur': 18, 'sharpness_falloff': 26, 'aberration': 9, 'bloom': 12, 'contrast': 50, 'mix': 100}},
            {'id': 'soft_wide', 'label': 'Мягкий широкий угол', 'description': 'Лёгкая оптика без жёсткой чёрной маски.', 'values': {'projection': 0, 'strength': 34, 'shape': 1, 'radius': 100, 'edge_darkness': 22, 'edge_softness': 72, 'overscan': 12, 'edge_blur': 24, 'sharpness_falloff': 18, 'aberration': 5, 'bloom': 14, 'contrast': 52, 'mix': 86}},
        ],
    },
    {
        'id': 'studio_vintage',
        'tag': 'frei0r.studio_vintage',
        'label': 'Старая камера',
        'version': '0.1.0',
        'groups': [
            {'id': 'format', 'label': 'Формат камеры', 'controls': ['format', 'amount', 'mix']},
            {'id': 'signal', 'label': 'Сигнал', 'controls': ['chroma_bleed', 'luma_ring', 'hue_drift', 'interlace', 'scanlines']},
            {'id': 'motion', 'label': 'Движение ленты', 'controls': ['flicker', 'jitter', 'timebase', 'dropouts']},
            {'id': 'texture', 'label': 'Фактура', 'controls': ['grain', 'dust', 'bloom', 'vignette', 'seed']},
        ],
        'parameters': [
            {'id': 'format', 'label': 'Тип камеры', 'kind': 'choice', 'min': 0, 'max': 4, 'default': 1, 'options': ['VHS', 'Hi8', 'Super 8', 'CRT', 'старая веб-камера']},
            {'id': 'amount', 'label': 'Общая сила', 'kind': 'number', 'min': 0, 'max': 100, 'default': 62, 'unit': '%'},
            {'id': 'chroma_bleed', 'label': 'Размывание цвета', 'kind': 'number', 'min': 0, 'max': 100, 'default': 46, 'unit': '%'},
            {'id': 'luma_ring', 'label': 'Световые контуры', 'kind': 'number', 'min': 0, 'max': 100, 'default': 28, 'unit': '%'},
            {'id': 'hue_drift', 'label': 'Плавание оттенка', 'kind': 'number', 'min': 0, 'max': 100, 'default': 22, 'unit': '%'},
            {'id': 'interlace', 'label': 'Чересстрочность', 'kind': 'number', 'min': 0, 'max': 100, 'default': 26, 'unit': '%'},
            {'id': 'scanlines', 'label': 'Строки развертки', 'kind': 'number', 'min': 0, 'max': 100, 'default': 30, 'unit': '%'},
            {'id': 'flicker', 'label': 'Мерцание', 'kind': 'number', 'min': 0, 'max': 100, 'default': 18, 'unit': '%'},
            {'id': 'jitter', 'label': 'Горизонтальный сдвиг', 'kind': 'number', 'min': 0, 'max': 100, 'default': 24, 'unit': '%'},
            {'id': 'timebase', 'label': 'Плавание ленты', 'kind': 'number', 'min': 0, 'max': 100, 'default': 18, 'unit': '%'},
            {'id': 'dropouts', 'label': 'Выпадения сигнала', 'kind': 'number', 'min': 0, 'max': 100, 'default': 12, 'unit': '%'},
            {'id': 'grain', 'label': 'Зерно', 'kind': 'number', 'min': 0, 'max': 100, 'default': 36, 'unit': '%'},
            {'id': 'dust', 'label': 'Пыль и царапины', 'kind': 'number', 'min': 0, 'max': 100, 'default': 20, 'unit': '%'},
            {'id': 'bloom', 'label': 'Свечение', 'kind': 'number', 'min': 0, 'max': 100, 'default': 18, 'unit': '%'},
            {'id': 'vignette', 'label': 'Виньетка', 'kind': 'number', 'min': 0, 'max': 100, 'default': 24, 'unit': '%'},
            {'id': 'seed', 'label': 'Вариант движения', 'kind': 'number', 'min': 0, 'max': 100, 'default': 13.7, 'unit': ''},
            {'id': 'mix', 'label': 'Смешивание', 'kind': 'number', 'min': 0, 'max': 100, 'default': 100, 'unit': '%'},
        ],
        'presets': [
            {'id': 'home_vhs', 'label': 'Домашняя VHS', 'description': 'Мягкий цвет, умеренное плавание ленты и зерно.', 'values': {'format': 0, 'amount': 52, 'chroma_bleed': 46, 'luma_ring': 24, 'hue_drift': 18, 'interlace': 22, 'scanlines': 26, 'flicker': 14, 'jitter': 18, 'timebase': 16, 'dropouts': 8, 'grain': 32, 'dust': 14, 'bloom': 16, 'vignette': 22, 'seed': 13.7, 'mix': 100}},
            {'id': 'worn_tape', 'label': 'Изношенная кассета', 'description': 'Сильная нестабильность, выпадения и цветовой сдвиг.', 'values': {'format': 0, 'amount': 78, 'chroma_bleed': 66, 'luma_ring': 38, 'hue_drift': 34, 'interlace': 36, 'scanlines': 42, 'flicker': 30, 'jitter': 38, 'timebase': 32, 'dropouts': 32, 'grain': 54, 'dust': 28, 'bloom': 20, 'vignette': 30, 'seed': 13.7, 'mix': 100}},
            {'id': 'old_camera', 'label': 'Старая камера', 'description': 'Тёплый мягкий образ с лёгким bloom и виньеткой.', 'values': {'format': 1, 'amount': 48, 'chroma_bleed': 30, 'luma_ring': 20, 'hue_drift': 16, 'interlace': 12, 'scanlines': 14, 'flicker': 10, 'jitter': 10, 'timebase': 8, 'dropouts': 4, 'grain': 24, 'dust': 12, 'bloom': 28, 'vignette': 34, 'seed': 13.7, 'mix': 100}},
        ],
    },
]

def specialized_header():
    lines = [
        '/* Generated by scripts/generate_ui.py. Do not edit. */',
        '#ifndef SFX_SPECIALIZED_CONTRACT_H',
        '#define SFX_SPECIALIZED_CONTRACT_H',
        '#define SFX_SPECIALIZED_SCHEMA 1',
    ]
    for item in SPECIALIZED:
        prefix = item['id'].upper()
        lines.append(f'#define {prefix.upper()}_PARAM_COUNT {len(item["parameters"])}')
        for index, parameter in enumerate(item['parameters']):
            lines.append(f'#define {prefix.upper()}_PARAM_{parameter["id"].upper()} {index}')
        lines.append(f'#define {prefix.upper()}_PRESET_COUNT {len(item["presets"])}')
    lines.append('#endif')
    return '\n'.join(lines) + '\n'

def write_specialized(dest: Path):
    kd = dest/'kdenlive'
    kd.mkdir(parents=True, exist_ok=True)
    payload = {'schema': 1, 'version': '0.1.0', 'filters': SPECIALIZED}
    (kd/'specialized.json').write_text(json.dumps(payload, ensure_ascii=False, indent=2)+'\n', encoding='utf-8', newline='\n')
    (dest/'include'/'specialized_contract.h').write_text(specialized_header(), encoding='utf-8', newline='\n')
    for item in SPECIALIZED:
        effect = ET.Element('effect', {'xmlns': 'https://www.kdenlive.org', 'tag': item['tag'],
                            'id': item['id'], 'type': 'video', 'version': item['version']})
        ET.SubElement(effect, 'name').text = item['label']
        ET.SubElement(effect, 'description').text = ('Детальный эффект монтажной студии. '
                                                     'Предсказуемый SDR / 8 бит.')
        ET.SubElement(effect, 'author').text = 'Studio FX contributors'
        controls = []
        for index, parameter in enumerate(item['parameters']):
            maximum = parameter['max']
            default = parameter['default'] / maximum if maximum else 0
            kind = parameter['kind']
            attrs = {'name': str(index), 'type': 'constant', 'default': format(default, '.17g'),
                     'value': format(default, '.17g'), 'min': str(parameter['min']),
                     'max': str(maximum), 'factor': str(maximum), 'decimals': '1'}
            if kind == 'choice':
                attrs['type'] = 'list'
                attrs['paramlist'] = ';'.join(str(value) for value in range(maximum + 1))
                attrs['paramlistdisplay'] = ','.join(parameter['options'])
                attrs['decimals'] = '0'
            node = ET.SubElement(effect, 'parameter', attrs)
            ET.SubElement(node, 'name').text = parameter['label']
            control = {'key': parameter['id'], 'index': index, 'kind': kind,
                       'label': parameter['label'], 'value': parameter['default'],
                       'lo': parameter['min'], 'hi': maximum, 'normalized': True}
            if kind == 'choice':
                control['options'] = [{'name': name, 'value': value}
                                      for value, name in enumerate(parameter['options'])]
            else:
                control.update(decimals=1, suffix=(' ' + parameter.get('unit', '')).rstrip())
            controls.append(control)
        threads = ET.SubElement(effect, 'parameter', {'name': 'threads', 'type': 'fixed', 'default': '1', 'value': '1'})
        ET.SubElement(threads, 'name').text = 'Потоки MLT'
        ET.indent(effect, space='    ')
        (kd/(item['id']+'.xml')).write_bytes(ET.tostring(effect, encoding='utf-8', xml_declaration=True)+b'\n')
        groups = [{'key': group['id'], 'label': group['label'], 'keys': group['controls'],
                   'collapsed': index != 0} for index, group in enumerate(item['groups'])]
        presets = []
        for preset in item['presets']:
            values = {}
            for key, value in preset['values'].items():
                parameter_index = next(i for i, p in enumerate(item['parameters']) if p['id'] == key)
                maximum = item['parameters'][parameter_index]['max']
                values[str(parameter_index)] = value / maximum if maximum else 0
            presets.append({'id': preset['id'], 'name': preset['label'],
                            'description': preset['description'], 'values': values})
        schema = {'version': 2, 'effect': item['id'], 'groups': groups,
                  'controls': controls, 'presets': presets}
        (kd/(item['id']+'.json')).write_text(json.dumps(schema, ensure_ascii=False, indent=2)+'\n',
                                             encoding='utf-8', newline='\n')

def recipes():
    out=[]
    for cat, category, rows in GROUPS:
        for title, alg, variant, amount, size, soft, anim in rows:
            i=len(out)
            values=[i/127, amount,size,.5,1., .30 if i==4 else .5,.5,soft,float(anim),.137,0.]
            controls=[1,4]
            if alg not in ('MIRROR',): controls.insert(1,2)
            if alg in ('VIGNETTE','LENS','DEFORM','LEAK'): controls.extend([5,6])
            if alg in ('VIGNETTE','LENS','DEFORM'): controls.append(7)
            if anim: controls.extend([3,8])
            out.append(dict(slot=i,id=f'studiofx.{i:03d}',name=title,category_id=cat,
                category=category,algorithm=alg,variant=variant,values=values,
                controls=controls,animated=bool(anim),cost='medium' if alg in ('GLOW','EDGE','EMBOSS','KALEIDO') else 'light'))
    return out

def generate(dest: Path=ROOT):
    rs=recipes()
    header='/* Generated by scripts/generate_ui.py. Do not edit. */\n#ifndef SFX_CONTRACT_H\n#define SFX_CONTRACT_H\n'
    header+=f'#define SFX_PARAM_COUNT {len(PARAMS)}\n#define SFX_RECIPE_COUNT {len(rs)}\n#define SFX_SLOT_SCALE 127.0\n'
    header+='enum SfxAlgorithm { '+', '.join('SFX_'+a for a in ALGORITHMS)+' };\n'
    header+='typedef struct { int algorithm,variant; double defaults[SFX_PARAM_COUNT]; } SfxRecipe;\n'
    header+='static const char *const sfx_names[SFX_PARAM_COUNT] = {'+','.join(json.dumps(p[0]) for p in PARAMS)+'};\n'
    header+='static const SfxRecipe sfx_recipes[SFX_RECIPE_COUNT] = {\n'
    for r in rs:
        header+=' {SFX_'+r['algorithm']+','+str(r['variant'])+',{'+','.join(format(v,'.17g') for v in r['values'])+'}},\n'
    header+='};\n#endif\n'
    (dest/'include').mkdir(parents=True,exist_ok=True)
    (dest/'include'/'sfx_contract.h').write_text(header,encoding='utf-8',newline='\n')
    kd=dest/'kdenlive'; kd.mkdir(parents=True,exist_ok=True)
    defaults=rs[0]['values']
    e=ET.Element('effect',{'xmlns':'https://www.kdenlive.org','tag':'frei0r.studiofx','id':'studiofx','type':'video','version':VERSION})
    ET.SubElement(e,'name').text='FactMontage: Эффекты'
    ET.SubElement(e,'description').text='Видеоэффекты монтажной студии. SDR / 8 бит.'
    ET.SubElement(e,'author').text='Studio FX contributors'
    for i,(_,label,_) in enumerate(PARAMS):
        value=format(defaults[i],'.17g')
        attrs={'name':str(i),'type':'fixed','default':value,'value':value}
        if i not in (0,10): attrs.update(type='constant',min='0',max='100',factor='100',decimals='0')
        if i==3: attrs.update(min='0.25',max='2',factor='2',decimals='2')
        if i==8: attrs={'name':str(i),'type':'bool','default':value,'value':value}
        p=ET.SubElement(e,'parameter',attrs)
        ET.SubElement(p,'name').text=label
    threads=ET.SubElement(e,'parameter',{'name':'threads','type':'fixed','default':'1','value':'1'})
    ET.SubElement(threads,'name').text='Потоки MLT'
    ET.indent(e,space='    ')
    (kd/'studiofx.xml').write_bytes(ET.tostring(e,encoding='utf-8',xml_declaration=True)+b'\n')
    (kd/'catalog.json').write_text(json.dumps(dict(schema=1,version=VERSION,parameters=PARAMS,recipes=rs),ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    options=[dict(name=r['name'],value=r['slot'],description=r['category'],preview='studiofx/'+r['id'],
                  category_id=r['category_id'],category=r['category'],animated=r['animated'],controls=r['controls']) for r in rs]
    controls=[
        dict(key='recipe',index=0,kind='choice',label='Готовый эффект',value=0,normalized=True,hi=127,options=options),
        dict(key='amount',index=1,kind='number',label='Сила',value=60,lo=0,hi=100,decimals=0,suffix=' %',normalized=True),
        dict(key='mix',index=4,kind='number',label='Смешивание',value=100,lo=0,hi=100,decimals=0,suffix=' %',normalized=True),
        dict(key='size',index=2,kind='number',label='Размер / область',value=50,lo=0,hi=100,decimals=0,suffix=' %',normalized=True),
        dict(key='softness',index=7,kind='number',label='Мягкость',value=65,lo=0,hi=100,decimals=0,suffix=' %',normalized=True),
        dict(key='center_x',index=5,kind='number',label='Центр X',value=50,lo=0,hi=100,decimals=0,suffix=' %',normalized=True),
        dict(key='center_y',index=6,kind='number',label='Центр Y',value=50,lo=0,hi=100,decimals=0,suffix=' %',normalized=True),
        dict(key='animate',index=8,kind='bool',label='Анимация',value=0),
        dict(key='speed',index=3,kind='number',label='Скорость',value=1,lo=.25,hi=2,decimals=2,suffix='×',normalized=True),
        dict(key='seed',index=9,kind='number',label='Вариант движения',value=13.7,lo=0,hi=100,decimals=0,suffix='',normalized=True),
    ]
    groups=[
        dict(key='catalog',label='Вид эффекта',keys=['recipe'],collapsed=False),
        dict(key='strength',label='Сила и смешивание',keys=['amount','mix'],collapsed=True),
        dict(key='shape',label='Область и центр',keys=['size','softness','center_x','center_y'],collapsed=True),
        dict(key='motion',label='Движение',keys=['animate','speed','seed'],collapsed=True),
    ]
    (kd/'studiofx.json').write_text(json.dumps(dict(version=2,effect='studiofx',groups=groups,controls=controls,recipes=rs),ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    lines=['# Studio FX — 80 работающих рецептов','', 'Сгенерировано из Effects/scripts/generate_ui.py. Названия алгоритмов — реализации src/studiofx.c.','']
    for _,category,_ in GROUPS:
        lines+=['## '+category,'']
        for r in rs:
            if r['category']==category: lines.append(f"- **{r['name']}** — `{r['id']}`; {r['algorithm']}/{r['variant']}; {'анимированный' if r['animated'] else 'статичный'}.")
        lines.append('')
    (kd/'CATALOG_RU.md').write_text('\n'.join(lines),encoding='utf-8',newline='\n')
    write_specialized(dest)
    return len(rs),len(ALGORITHMS)

def check():
    tracked=['include/sfx_contract.h','include/specialized_contract.h','kdenlive/studiofx.xml','kdenlive/studiofx.json','kdenlive/catalog.json','kdenlive/CATALOG_RU.md','kdenlive/specialized.json',
             'kdenlive/studio_lens.xml','kdenlive/studio_lens.json','kdenlive/studio_vintage.xml','kdenlive/studio_vintage.json']
    with tempfile.TemporaryDirectory() as folder:
        generated=Path(folder)
        generate(generated)
        stale=[name for name in tracked if not (ROOT/name).exists() or (ROOT/name).read_bytes()!=(generated/name).read_bytes()]
    if stale: raise SystemExit('Outdated generated files: '+', '.join(stale))
    print('StudioFX generated files: OK')

if __name__=='__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--output',type=Path,default=ROOT);parser.add_argument('--check',action='store_true')
    args=parser.parse_args()
    if args.check: check()
    else:
        count,families=generate(args.output)
        print(f'Generated {count} recipes / {families} algorithm families')
