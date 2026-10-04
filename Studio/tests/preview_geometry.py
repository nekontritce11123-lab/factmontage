#!/usr/bin/env python3
"""Native preview must retain positioned image layers at reduced resolution."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET


def properties(node, values):
    for name, value in values.items():
        ET.SubElement(node, 'property', name=name).text = str(value)


def check(renderer):
    with tempfile.TemporaryDirectory(prefix='sunimo-preview-geometry-') as directory:
        root = Path(directory)
        profile = root / 'profile'
        profile.write_text('description=SUNIMO preview geometry regression\nframe_rate_num=60\nframe_rate_den=1\nwidth=1920\nheight=1080\n'
                           'progressive=1\nsample_aspect_num=1\nsample_aspect_den=1\n'
                           'display_aspect_num=16\ndisplay_aspect_den=9\ncolorspace=709\n')
        scene = ET.Element('mlt', producer='timeline')
        ET.SubElement(scene, 'profile', width='1920', height='1080', frame_rate_num='60',
                      frame_rate_den='1', progressive='1', sample_aspect_num='1',
                      sample_aspect_den='1', display_aspect_num='16', display_aspect_den='9', colorspace='709')
        background = ET.SubElement(scene, 'producer', id='background', **{'in': '0', 'out': '1'})
        properties(background, {'mlt_service': 'color', 'resource': 'white'})
        for name, color, rect in [('red', (255, 0, 0), '720 270 480 270 1'),
                                  ('green', (0, 255, 0), '1200 540 240 270 1')]:
            image = root / (name + '.ppm')
            image.write_bytes(b'P6\n16 16\n255\n' + bytes(color) * 256)
            producer = ET.SubElement(scene, 'producer', id=name, **{'in': '0', 'out': '1'})
            properties(producer, {'mlt_service': 'qimage', 'resource': image})
            transform = ET.SubElement(producer, 'filter')
            properties(transform, {'mlt_service': 'qtblend', 'rect': rect, 'distort': 1})
            if name == 'green':
                corners = ET.SubElement(producer, 'filter')
                properties(corners, {'mlt_service': 'frei0r.c0rners', '0': .333333, '1': .333333,
                                     '2': .666667, '3': .333333, '4': .666667, '5': .666667,
                                     '6': .333333, '7': .666667, '12': 1})
        timeline = ET.SubElement(scene, 'tractor', id='timeline', **{'in': '0', 'out': '1'})
        for name in ('background', 'red', 'green'):
            ET.SubElement(timeline, 'track', producer=name)
        for track in (1, 2):
            transition = ET.SubElement(timeline, 'transition')
            properties(transition, {'mlt_service': 'qtblend', 'a_track': 0, 'b_track': track, 'always_active': 1})
        source = root / 'scene.mlt'
        ET.ElementTree(scene).write(source, encoding='utf-8', xml_declaration=True)
        for scale in (1, 4):
            output = root / str(scale)
            output.mkdir()
            args = 'vcodec=ffv1 pix_fmt=bgra an=1 threads=1 real_time=-1'
            if scale != 1:
                args += ' s=480x270'
            subprocess.run([renderer, 'preview-chunks', str(source), str(output), '0,1', '0',
                            str(profile), 'mkv', args], check=True, timeout=30)
            width, height = 1920 // scale, 1080 // scale
            for frame in (0, 1):
                pixels = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(output / f'{frame}.mkv'),
                                                  '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], timeout=15)
                assert len(pixels) == width * height * 3
                for x, y, expected in [(960, 405, (255, 0, 0)), (1320, 675, (0, 255, 0))]:
                    offset = ((y // scale) * width + x // scale) * 3
                    actual = tuple(pixels[offset:offset + 3])
                    assert all(abs(a - b) < 5 for a, b in zip(actual, expected)), (scale, frame, expected, actual)
        print('PASS: Transform/Corners image layers survive full and quarter-size preview, including consecutive chunks.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--renderer', default='kdenlive_render')
    check(parser.parse_args().renderer)
