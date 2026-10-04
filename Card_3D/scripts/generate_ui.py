#!/usr/bin/env python3
"""Public Card 3D contract. Numbers use value / hi, never an XML offset."""
from pathlib import Path
import argparse
import json
import xml.etree.ElementTree as E

ROOT = Path(__file__).resolve().parents[1]
P = [
    dict(key='STYLE', label='Вид карточки', labels=['Панель', 'Бумага', 'Фото'],
         descriptions=['Тонкая рамка и объёмный торец.', 'Светлая бумага с живым краем.', 'Белая фоторамка без фактуры.'], value=0,
         tip='Готовые рамка, край, тень и торец. Сменить вид можно без потери расположения.'),
    dict(key='ENTRANCE', label='Появление', labels=['Без появления', 'Мягкий выезд', 'Выезд с вытягиванием', 'Из глубины', 'Раскрытие'],
         descriptions=['Сразу показывает карточку.', 'Спокойно вводит карточку в кадр.', 'Добавляет мягкую деформацию при въезде.', 'Появляется из глубины кадра.', 'Раскрывает карточку в движении.'], value=1,
         tip='Как карточка появляется в начале клипа. «Без появления» показывает её сразу.'),
    dict(key='CORNER', label='Откуда', labels=['Справа снизу', 'Слева снизу', 'Справа сверху', 'Слева сверху'], value=1,
         tip='Угол входа. Не меняет конечное расположение. При «Без появления» не используется.'),
    dict(key='PLACEMENT', label='Расположение', labels=['По центру', 'Слева', 'Справа', 'Сверху слева', 'Сверху справа', 'Снизу слева', 'Снизу справа'], value=1,
         tip='Где останется карточка после появления. Размер и запас для тени учитываются автоматически.'),
    dict(key='SIZE', label='Размер', lo=15, hi=100, value=68, suffix='%', decimals=1,
         tip='Размер относительно доступной области кадра. Пропорции источника сохраняются.'),
    dict(key='DURATION', label='Длительность появления', lo=.25, hi=2.5, value=.85, suffix=' с', decimals=2,
         tip='Меньше — быстрее. При «Без появления» не используется. Для задержки передвиньте клип на таймлайне.'),
    dict(key='MOTION', label='Движение', labels=['Без движения', 'Спокойное', 'Живое'],
         descriptions=['Оставляет карточку неподвижной.', 'Добавляет едва заметную перспективу.', 'Делает перспективу выразительнее.'], value=1,
         tip='Готовая перспектива после входа. «Без движения» оставляет карточку прямо и неподвижно.'),
    dict(key='SOURCE', label='Источник', labels=['Изображение', 'Видео'], value=0,
         tip='«Изображение» убирает прозрачные поля. «Видео» сохраняет границы кадра, чтобы меняющаяся прозрачность не меняла масштаб. Чёрные поля не удаляются.'),
    dict(key='ROUNDING', label='Скругление', lo=0, hi=20, value=4, suffix='%', decimals=1,
         tip='Скругляет внешний силуэт карточки. Значение сохраняется при смене вида.'),
    dict(key='BACKGROUND', label='Фон карточки', labels=['С фоном', 'Только обводка'], value=0,
         tip='«С фоном» заполняет прозрачные участки цветом материала. «Только обводка» сохраняет альфа-канал источника.'),
    dict(key='SHADOW', label='Интенсивность тени', lo=0, hi=100, value=35, suffix='%', decimals=0,
         tip='Непрозрачность мягкой тени. 0% полностью отключает тень.'),
    dict(key='POSITION_MODE', label='Способ расположения', labels=['Готовая позиция', 'Вручную'], value=0,
         tip='Готовые позиции выбираются выше. Ручной режим включается при перетаскивании в мониторе или изменении ползунков.'),
    dict(key='X', label='Положение по горизонтали', lo=0, hi=100, value=0, suffix='%', decimals=1,
         tip='Положение карточки слева направо. Ползунок синхронизирован с рамкой в мониторе.'),
    dict(key='Y', label='Положение по вертикали', lo=0, hi=100, value=50, suffix='%', decimals=1,
         tip='Положение карточки сверху вниз. Ползунок синхронизирован с рамкой в мониторе.'),
    dict(key='EXIT_ENABLED', label='Анимировать уход', kind='bool', lo=0, hi=1, value=1,
         tip='В конце клипа карточка уходит обратным ходом выбранной анимации.'),
    dict(key='EXIT_ANIMATION', label='Анимация ухода',
         labels=['Как у появления', 'Мягкий выезд', 'Выезд с вытягиванием', 'Из глубины', 'Раскрытие'], value=0,
         tip='«Как у появления» автоматически повторяет текущий вход в обратную сторону.'),
    dict(key='EXIT_DURATION', label='Длительность ухода', lo=.25, hi=2.5, value=.85, suffix=' с', decimals=2,
         tip='Меньше — быстрее. Уход всегда заканчивается на последнем кадре клипа.'),
    dict(key='EXIT_AT', label='Конец клипа', kind='number', lo=0, hi=21600, value=0, suffix=' с', decimals=3, internal=True,
         tip='Внутреннее время последнего кадра; Kdenlive обновляет его при обрезке и разрезании.'),
]
for i, p in enumerate(P):
    p['index'] = i
    p.setdefault('kind', 'list' if 'labels' in p else 'number')
    if p['kind'] == 'list': p.update(lo=0, hi=len(p['labels']) - 1)
    p['normalized'] = p['value'] / p['hi']

# The panel only rearranges this public contract; it adds no renderer controls.
GROUPS = [
    dict(key='style', label='Вид карточки', keys=['STYLE'], collapsed=False),
    dict(key='appearance', label='Оформление', keys=['BACKGROUND', 'ROUNDING', 'SHADOW'], collapsed=False),
    dict(key='placement', label='Размер и положение', keys=['SIZE', 'PLACEMENT', 'POSITION_MODE', 'X', 'Y'], collapsed=False),
    dict(key='entrance', label='Появление и уход',
         keys=['ENTRANCE', 'CORNER', 'DURATION', 'EXIT_ENABLED', 'EXIT_ANIMATION', 'EXIT_DURATION'], collapsed=True),
    dict(key='motion', label='Движение', keys=['MOTION'], collapsed=True),
    dict(key='source', label='Источник', keys=['SOURCE'], collapsed=False),
]


def studio_schema():
    controls = []
    for p in P:
        if p.get('internal'): continue
        q = dict(p)
        if 'labels' in p:
            descriptions = p.get('descriptions', [''] * len(p['labels']))
            previews = [f'{p["key"].lower()}_{i}' for i in range(len(p['labels']))] if p['key'] in ('STYLE', 'ENTRANCE', 'MOTION') else []
            q['options'] = [dict(value=i, name=name, description=descriptions[i],
                                 preview=previews[i] if previews else '', animated=bool(previews))
                            for i, name in enumerate(p['labels'])]
            q.pop('labels')
            q.pop('descriptions', None)
        if p['key'] == 'CORNER': q['positions'] = [[1, 1], [1, 0], [0, 1], [0, 0]]
        if p['key'] == 'PLACEMENT': q['positions'] = [[1, 1], [1, 0], [1, 2], [0, 0], [0, 2], [2, 0], [2, 2]]
        if p['key'] == 'DURATION': q['visible_when'] = dict(key='ENTRANCE', not_value=0)
        if p['key'] in ('EXIT_ANIMATION', 'EXIT_DURATION'): q['visible_when'] = dict(key='EXIT_ENABLED', equals=1)
        controls.append(q)
    return dict(version=2, effect='card3d', service='frei0r.card3d', groups=GROUPS, controls=controls)


def outputs():
    header = ['// Generated by scripts/generate_ui.py; do not edit.', '#pragma once',
              f'constexpr int PUBLIC_PARAMS={len(P)};',
              'enum PublicParam { ' + ', '.join('UI_' + p['key'] for p in P) + ' };',
              'struct PublicSpec { const char* name; const char* help; double def,lo,hi; bool menu; };',
              'static const PublicSpec PUBLIC_SPECS[PUBLIC_PARAMS]={']
    for p in P:
        header.append('{' + json.dumps(p['key']) + ',' + json.dumps(p['tip'], ensure_ascii=False) +
                      f', {p["normalized"]:.17g}, {p["lo"]:.17g}, {p["hi"]:.17g}, ' +
                      ('true' if p['kind'] == 'list' else 'false') + '},')
    header += ['};', '']
    root = E.Element('effect', tag='frei0r.card3d', id='card3d', type='video', LC_NUMERIC='C', requires_in_out='1')
    E.SubElement(root, 'name').text = 'FactMontage: Карточки'
    E.SubElement(root, 'description').text = 'Карточка с готовым оформлением, появлением и живой перспективой. Один эффект на слой.'
    E.SubElement(root, 'author').text = 'Card 3D contributors'
    for p in P:
        attrs = dict(name=str(p['index']), default=format(p['normalized'], '.17g'))
        if p.get('internal'):
            attrs.update(type='fixed')
        elif p['kind'] == 'list':
            attrs.update(type='list', paramlist=';'.join(format(i / p['hi'], '.17g') for i in range(len(p['labels']))))
        elif p['kind'] == 'bool':
            attrs.update(type='bool', min='0', max='1')
        else:
            attrs.update(type='constant', min=str(p['lo']), max=str(p['hi']), factor=str(p['hi']),
                         decimals=str(p['decimals']), suffix=p['suffix'])
        q = E.SubElement(root, 'parameter', attrs)
        E.SubElement(q, 'name').text = p['label']
        E.SubElement(q, 'comment').text = p['tip']
        if p['kind'] == 'list': E.SubElement(q, 'paramlistdisplay').text = ','.join(p['labels'])
    E.indent(root, space='  ')
    return {
        ROOT / 'kdenlive/studio.json': json.dumps(studio_schema(), ensure_ascii=False, indent=2) + '\n',
        ROOT / 'src/parameters.hpp': '\n'.join(header),
        ROOT / 'docs/parameters.json': json.dumps(P, ensure_ascii=False, indent=2) + '\n',
        ROOT / 'kdenlive/card3d.xml': '<?xml version="1.0" encoding="UTF-8"?>\n' + E.tostring(root, encoding='unicode') + '\n',
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    for path, data in outputs().items():
        if args.check:
            if not path.is_file() or path.read_text(encoding='utf-8') != data:
                raise SystemExit(f'Out of date: {path}')
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(data, encoding='utf-8', newline='\n')
    print(f'Public contract: {len(P)} parameters; generated files OK')


if __name__ == '__main__': main()
