#!/usr/bin/env python3
"""Small offline previews for the native panel, from the actual public renderer."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image
from host import Plugin, ROOT, library_path
from make_assets import panel


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    schema = json.loads((ROOT/'kdenlive/studio.json').read_text(encoding='utf-8'))
    source = np.array(panel().resize((320, 180), Image.Resampling.LANCZOS))
    records = []
    for spec in schema['controls']:
        for option in (item for item in spec.get('options', []) if item.get('preview')):
            choice, name = option['value'], option['preview']
            settings = dict(PLACEMENT=0, SIZE=82, ENTRANCE=0, MOTION=0)
            settings[spec['key']] = choice
            seconds = 8 if spec['key'] == 'MOTION' and choice else 3
            frames = []
            with Plugin(320, 180) as renderer:
                renderer.set(**settings)
                for frame in range(seconds*10):
                    backdrop = Image.new('RGBA', (320, 180), (224, 229, 237, 255))
                    backdrop.alpha_composite(Image.fromarray(renderer.render(source, frame/10)))
                    frames.append(backdrop.convert('RGB'))
            frames[min(20, len(frames)-1)].save(args.output/(name+'.png'))
            dest = args.output/(name+'.gif')
            frames[0].save(dest, save_all=True, append_images=frames[1:], duration=100, loop=0, disposal=2)
            with Image.open(dest) as check:
                duration = 0
                for frame in range(check.n_frames):
                    check.seek(frame)
                    check.load()
                    duration += check.info.get('duration', 0)
                assert check.size == (320, 180) and duration == seconds*1000
            records.append(dict(name=name, seconds=seconds, settings=settings, sha256=hashlib.sha256(dest.read_bytes()).hexdigest()))
    assert len(records) == 11
    report = dict(binary_sha256=hashlib.sha256(library_path().read_bytes()).hexdigest(), previews=records)
    (args.output/'previews.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print('PASS: 11 previews decoded; correct dimensions and durations')


if __name__ == '__main__':
    main()
