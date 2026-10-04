#!/usr/bin/env python3
"""Identical 1080p60 sources for baseline/studio, using synchronous real MLT frames."""
import argparse
import json
from pathlib import Path
import platform
import shlex
import subprocess
import time
import xml.etree.ElementTree as ET

FILTERS = {
    'cards': ('frei0r.card3d', {'0': 0, '1': .25, '2': 0, '3': .16666666666666666, '4': .68,
        '5': .34, '6': 0, '7': 1, '8': .2, '9': 1, '10': .35, '11': 1, '12': .2, '13': .8,
        '14': 1, '15': 0, '16': .34, '17': (131 / 60) / 21600, 'threads': 1}),
    'camera': ('studio.camera', {'mode': 4, 'live': 1, 'zoom': 140, 'start_x': 30, 'end_x': 70}),
    'background': ('studio.background', {'method': 0, 'output': 1, 'key_r': 0, 'key_g': 1, 'key_b': 0,
        'tolerance': .13, 'transition': .08, 'despill': .65, 'feather': 1.2}),
    'effects': ('frei0r.studiofx', {'0': 10/127, '1': .46, '2': .47, '3': .5, '4': 1,
        '5': .5, '6': .5, '7': .65, '8': 1, '9': .137, '10': 0, 'threads': 1}),
    'color': ('studio.color', {'studio_input_validated': 1, 'studio_color_algorithm': 'sdr-primary-v1',
        'exposure': .2, 'saturation': 105}),
}


def props(element, values):
    for name, value in values.items():
        ET.SubElement(element, 'property', name=name).text = str(value)


def scene(path, source, layers, filters, transition=False):
    root = ET.Element('mlt', producer='scene')
    ET.SubElement(root, 'profile', width='1920', height='1080', frame_rate_num='60', frame_rate_den='1',
        progressive='1', sample_aspect_num='1', sample_aspect_den='1', display_aspect_num='16', display_aspect_den='9', colorspace='709')
    for layer in range(layers):
        producer = ET.SubElement(root, 'producer', id=f'clip{layer}')
        props(producer, {'mlt_service': 'avformat', 'resource': source, 'set.test_audio': 1})
        for name in filters:
            service, values = FILTERS[name]
            props(ET.SubElement(producer, 'filter'), {'mlt_service': service, **values})
        playlist = ET.SubElement(root, 'playlist', id=f'track{layer}')
        ET.SubElement(playlist, 'entry', producer=f'clip{layer}', attrib={'in': '0', 'out': '131'})
    tractor = ET.SubElement(root, 'tractor', id='scene', attrib={'in': '0', 'out': '131'})
    for layer in range(layers):
        ET.SubElement(tractor, 'track', producer=f'track{layer}')
    if layers == 2:
        values = {'mlt_service': 'frei0r.sunimo_transition', '0': '0=0;131=1', '1': 0, '2': .45, '3': 0, '4': .35} if transition else {'mlt_service': 'composite', 'geometry': '0=10%/10%:80%x80%:70', 'fill': 1}
        props(ET.SubElement(tractor, 'transition', attrib={'in': '0', 'out': '131'}), {'a_track': 0, 'b_track': 1, **values})
    ET.ElementTree(root).write(path, encoding='utf-8', xml_declaration=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--measure', type=Path, help='Reuse the SDK-built measurement executable with a packaged runtime')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    work = args.output.parent / ('benchmark-baseline' if args.baseline else 'benchmark-studio')
    work.mkdir(parents=True, exist_ok=True)
    executable = args.measure or work/'measure'
    if args.measure is None:
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'mlt-framework-7'], text=True))
        subprocess.run(['c++', '-O2', '-std=c++17', str(Path(__file__).resolve().parents[1]/'tests/mlt_benchmark.cpp'), '-o', str(executable), *flags], check=True)
    source = work/'1080p60.mkv'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=1920x1080:rate=60', '-frames:v', '132', '-c:v', 'ffv1', '-threads', '2', str(source)], check=True)
    results = []
    cases = {'control': []} if args.baseline else {'control': [], **{name: [name] for name in FILTERS}, 'combined': ['background', 'camera', 'effects', 'color', 'cards'], 'transition': []}
    for name, filters in cases.items():
        for layers in (1, 2):
            if name == 'transition' and layers == 1:
                continue
            xml = work/f'{name}-{layers}.mlt'
            scene(xml, source, layers, filters, name == 'transition')
            for divisor in (1, 2, 4):
                result = subprocess.run([str(executable), str(xml), str(1920//divisor), str(1080//divisor), '60'], text=True, capture_output=True, timeout=300, check=True)
                item = dict(scene=name, layers=layers, preview_divisor=divisor, **json.loads(result.stdout))
                results.append(item)
                print(json.dumps(item), flush=True)
            if name in ('control', 'combined', 'transition'):
                start = time.monotonic()
                subprocess.run(['melt', str(xml), '-consumer', f'avformat:{work/name}.mkv', 'vcodec=ffv1', 'an=1', 'threads=2', 'real_time=-1'], capture_output=True, check=True, timeout=300)
                results.append(dict(scene=name, layers=layers, export_1080p60_seconds=round(time.monotonic()-start, 3), frames=132))
    report = dict(scope='Synchronous MLT render+decode. Over-budget frames are NOT GUI dropped frames. WSL numbers do NOT accept Deck.',
        host=platform.node(), system=platform.platform(), cpu=next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')), 'unknown'),
        baseline=args.baseline, results=results)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')


if __name__ == '__main__':
    main()
