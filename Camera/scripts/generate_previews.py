#!/usr/bin/env python3
"""Render the panel previews through the real studio.camera MLT filter."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent / 'Studio' / 'previews'
MODULE = Path(sys.argv[1]).resolve()


def run(args, env=None):
    result = subprocess.run(args, env=env, text=True, capture_output=True, timeout=180)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


with tempfile.TemporaryDirectory(prefix='studio-camera-previews-') as folder:
    work = Path(folder)
    repository = work / 'repository'
    repository.mkdir()
    system = Path(subprocess.check_output(
        ['pkg-config', '--variable=moduledir', 'mlt-framework-7'], text=True).strip())
    for source in system.glob('*.so'):
        (repository / source.name).symlink_to(source)
    (repository / MODULE.name).symlink_to(MODULE)
    data = work / 'data' / 'studio'
    data.mkdir(parents=True)
    (data / 'filter_camera.yml').write_bytes((ROOT / 'mlt/filter_camera.yml').read_bytes())
    profile = work / 'profile'
    profile.write_text('description=Studio camera previews\nframe_rate_num=30\nframe_rate_den=1\nwidth=480\nheight=270\nprogressive=1\nsample_aspect_num=1\nsample_aspect_den=1\ndisplay_aspect_num=16\ndisplay_aspect_den=9\ncolorspace=709\n')
    env = os.environ.copy()
    env.update(MLT_REPOSITORY=str(repository), MLT_DATA=str(work / 'data'), QT_QPA_PLATFORM='offscreen')
    variants = []
    for mode in range(6):
        variants.append((f'camera_mode_{mode}', dict(mode=mode, live=0, zoom=150, cycle_period=3)))
    for live in range(3):
        variants.append((f'camera_live_{live}', dict(mode=0, live=live, zoom=125)))
    OUT.mkdir(parents=True, exist_ok=True)
    metadata = []
    for name, settings in variants:
        video = work / f'{name}.mkv'
        args = ['melt', '-profile', str(profile), 'qimage:' + str(ROOT / 'demo/camera_test.png'),
                'in=0', 'out=89', '-attach', 'studio.camera', 'start_x=30', 'start_y=50',
                'end_x=70', 'end_y=50', 'timing=0']
        args += [f'{key}={value}' for key, value in settings.items()]
        args += ['-consumer', 'avformat:' + str(video), 'vcodec=ffv1', 'an=1', 'real_time=-1']
        run(args, env)
        run(['ffmpeg', '-y', '-v', 'error', '-i', str(video), '-vf',
             'fps=12,scale=320:-1:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse',
             '-loop', '0', str(OUT / f'{name}.gif')])
        run(['ffmpeg', '-y', '-v', 'error', '-i', str(video), '-frames:v', '1',
             '-vf', 'scale=320:-1:flags=lanczos', str(OUT / f'{name}.png')])
        digest = hashlib.sha256((OUT / f'{name}.gif').read_bytes()).hexdigest()
        metadata.append(dict(name=name, seconds=3, settings=settings, sha256=digest))
    (OUT / 'camera_previews.json').write_text(
        json.dumps({'previews': metadata}, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'Rendered and hashed {len(variants)} Studio Camera previews through MLT')
