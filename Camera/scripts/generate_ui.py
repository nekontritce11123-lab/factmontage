#!/usr/bin/env python3
"""Generate the camera's Kdenlive XML and native-panel schema from one table."""
from pathlib import Path
import argparse
import json
import xml.etree.ElementTree as E

ROOT = Path(__file__).resolve().parents[1]
PARAMETERS = [
    dict(key='mode', label='Движение', kind='choice', labels=['Без изменения масштаба', 'Приблизить', 'Отдалить', 'Приблизить и вернуть', 'Проезд', 'Зум по кругу'],
         descriptions=['Сохраняет выбранный масштаб.', 'Плавно приближает к цели.', 'Плавно отдаляет от цели.', 'Приближает, удерживает и возвращает.', 'Перемещается между двумя точками.', 'Плавно приближает и отдаляет.'], value=0,
         tip='Выберите одно готовое движение камеры.'),
    dict(key='live', label='Живая камера', kind='choice', labels=['Выключена', 'Спокойная', 'Заметная'],
         descriptions=['Без дополнительного дрейфа.', 'Очень мягкое живое движение.', 'Более заметное живое движение.'], value=1,
         tip='Медленный повторяемый дрейф без покадровой случайной тряски.'),
    dict(key='zoom', label='Масштаб', kind='number', lo=100, hi=200, value=120, suffix='%', decimals=1,
         tip='100% не оставляет пространства для управляемого проезда; увеличьте масштаб.'),
    dict(key='start_x', label='Начало · X', kind='number', lo=0, hi=100, value=35, suffix='%', decimals=1),
    dict(key='start_y', label='Начало · Y', kind='number', lo=0, hi=100, value=50, suffix='%', decimals=1),
    dict(key='end_x', label='Конец · X', kind='number', lo=0, hi=100, value=65, suffix='%', decimals=1),
    dict(key='end_y', label='Конец · Y', kind='number', lo=0, hi=100, value=50, suffix='%', decimals=1),
    dict(key='timing', label='Продолжительность', kind='choice', labels=['Весь клип', 'Заданное время'], value=0,
         tip='По умолчанию движение растягивается на весь клип.', visible_when=dict(key='mode', not_value=5)),
    dict(key='duration', label='Время движения', kind='number', lo=.25, hi=60, value=.65, suffix=' с', decimals=2,
         tip='После заданного времени камера сохраняет конечное положение.',
         visible_when=[dict(key='mode', not_value=5), dict(key='timing', equals=1)]),
    dict(key='cycle_period', label='Полный цикл', kind='number', lo=2, hi=12, value=6, suffix=' с', decimals=1,
         tip='Время полного движения «приближение → возвращение».', visible_when=dict(key='mode', equals=5)),
    dict(key='tracking', label='Слежение за головой', kind='choice', labels=['Выключено', 'Автоматически'],
         descriptions=['Камера следует выбранной точке.', 'Удерживает заранее проанализированную голову в кадре.'], value=0,
         tip='Анализ выполняется заранее, поэтому перемотка и экспорт дают одинаковый результат.'),
    dict(key='track_path', label='Файл трека', kind='file', value='', filter='Трек головы (*.scam)',
         tip='Трек .scam хранит только координаты головы и не содержит видео.', visible_when=dict(key='tracking', equals=1)),
    dict(key='track_offset', label='Смещение трека', kind='number', lo=-7200, hi=7200, value=0, suffix=' с', decimals=3,
         tip='Сдвигает траекторию относительно начала эффекта.', visible_when=dict(key='tracking', equals=1)),
]
GROUPS = [
    dict(key='movement', label='Движение камеры', keys=['mode'], collapsed=False),
    dict(key='framing', label='Кадрирование', keys=['zoom', 'start_x', 'start_y', 'end_x', 'end_y'], collapsed=False),
    dict(key='time', label='Время', keys=['timing', 'duration', 'cycle_period'], collapsed=False),
    dict(key='live', label='Живая камера', keys=['live'], collapsed=True),
    dict(key='tracking', label='Трекинг головы', keys=['tracking', 'track_path', 'track_offset'], collapsed=True,
         clip_only=True),
]
PREVIEWS = {'mode': [f'camera_mode_{i}' for i in range(6)], 'live': [f'camera_live_{i}' for i in range(3)]}

def xml_text():
    root = E.Element('effect', tag='studio.camera', id='studio_camera', type='video', LC_NUMERIC='C', requires_in_out='1')
    E.SubElement(root, 'name').text = 'FactMontage: Камера'
    E.SubElement(root, 'description').text = 'Плавная камера, циклический зум, проезд и заранее рассчитанный трекинг головы.'
    E.SubElement(root, 'author').text = 'SUNIMO contributors'
    for spec in PARAMETERS:
        attrs = dict(name=spec['key'], default=str(spec['value']))
        if spec['kind'] == 'choice': attrs.update(type='list', paramlist=';'.join(map(str, range(len(spec['labels'])))))
        elif spec['kind'] == 'file': attrs.update(type='url')
        else: attrs.update(type='constant', min=str(spec['lo']), max=str(spec['hi']), factor='1', suffix=spec.get('suffix', ''), decimals=str(spec.get('decimals', 1)))
        node = E.SubElement(root, 'parameter', attrs)
        E.SubElement(node, 'name').text = spec['label']; E.SubElement(node, 'comment').text = spec.get('tip', '')
        if spec['kind'] == 'choice': E.SubElement(node, 'paramlistdisplay').text = ','.join(spec['labels'])
    for name in ('studio_time_origin', 'studio_time_span', 'variant'): E.SubElement(root, 'parameter', name=name, type='fixed', default='0')
    E.indent(root, space='  ')
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + E.tostring(root, encoding='unicode') + '\n'

def schema_text():
    controls=[]
    for spec in PARAMETERS:
        item=dict(spec)
        if spec['kind'] == 'choice':
            descriptions = spec.get('descriptions', [''] * len(spec['labels']))
            previews = PREVIEWS.get(spec['key'], [])
            item['options'] = [dict(value=i, name=name, description=descriptions[i],
                                    preview=previews[i] if previews else '', animated=bool(previews))
                               for i, name in enumerate(spec['labels'])]
            item.pop('labels')
            item.pop('descriptions', None)
        controls.append(item)
    return json.dumps(dict(version=2,effect='studio_camera',service='studio.camera',groups=GROUPS,controls=controls),ensure_ascii=False,indent=2)+'\n'

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--check',action='store_true');args=parser.parse_args()
    outputs={ROOT/'kdenlive/studio_camera.xml':xml_text(),ROOT/'kdenlive/studio_camera.json':schema_text()}
    if args.check:
        stale=[str(path) for path,data in outputs.items() if not path.exists() or path.read_text(encoding='utf-8')!=data]
        if stale:raise SystemExit('Generated files are stale: '+', '.join(stale))
    else:
        for path,data in outputs.items():path.write_text(data,encoding='utf-8',newline='\n')

if __name__=='__main__':main()
