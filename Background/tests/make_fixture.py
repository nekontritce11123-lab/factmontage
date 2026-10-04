#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic render fixture, NOT RVM quality or Kdenlive GUI evidence.
Optional Pillow is used only to create/inspect the fixture, not by the plugin.
"""
import argparse
import json
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('cli', type=Path)
parser.add_argument('output_directory', type=Path)
args = parser.parse_args()
args.output_directory.mkdir(parents=True, exist_ok=True)
w, h = 640, 360
im = Image.new('RGBA', (w, h), (12, 190, 35, 255))
d = ImageDraw.Draw(im)
# A geometric foreground: rounded cards, thin lines, and a cut-out in the source.
d.rounded_rectangle((195, 65, 430, 321), radius=24, fill=(225, 58, 72, 255))
d.rounded_rectangle((220, 96, 405, 168), radius=15, fill=(244, 204, 73, 255))
d.ellipse((240, 198, 296, 254), fill=(75, 91, 207, 255))
d.rectangle((335, 204, 385, 247), fill=(12, 190, 35, 255))
for i in range(8):
    d.line((205+i*25, 65, 181+i*30, 22), fill=(220, 65, 84, 255), width=1+i%3)
d.text((18, 20), 'SYNTHETIC CHROMA TEST / NOT RVM', fill=(220, 65, 84, 255))
source = args.output_directory / 'chroma-input.png'
im.save(source)
common = [str(args.cli.resolve()), 'png', '--input', str(source), '--key_r', str(12/255), '--key_g', str(190/255), '--key_b', str(35/255), '--feather', '1.2']
for name, mode in [('chroma-transparent.png', 0), ('chroma-mask.png', 3), ('chroma-fill.png', 2)]:
    target = args.output_directory / name
    subprocess.run(common + ['--output', str(target), '--output_mode', str(mode)], check=True, timeout=20)
result = Image.open(args.output_directory / 'chroma-transparent.png').convert('RGBA')
assert result.getpixel((600, 320))[3] == 0, 'background should be transparent'
assert result.getpixel((300, 280))[3] == 255, 'foreground should be opaque'
assert result.getpixel((360, 224))[3] == 0, 'cut-out should be transparent'
report = {'kind': 'synthetic_chroma_core_test_not_human_AI_or_Kdenlive', 'size': [w,h], 'checks_passed': ['background alpha 0', 'foreground alpha 255', 'interior cut-out alpha 0'], 'renderer': 'real compiled sbg-cli'}
(args.output_directory / 'fixture-check.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
print(json.dumps(report, ensure_ascii=False))
