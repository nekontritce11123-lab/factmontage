#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline alpha MOV output. Uses the same renderer; ffmpeg is optional and external."""
from __future__ import annotations
import argparse, math, shutil, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from tools.scene import metadata
from tools.native import Native

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('scene',type=Path);p.add_argument('output',type=Path);p.add_argument('--ffmpeg',default='ffmpeg');a=p.parse_args()
 if a.output.suffix.lower()!='.mov':p.error('Use a .mov output (qtrle with alpha).')
 if a.output.exists():p.error('Output already exists; choose a new filename.')
 ffmpeg=shutil.which(a.ffmpeg)
 if not ffmpeg:raise RuntimeError('ffmpeg не найден. Этот необязательный экспорт требует внешнего FFmpeg.')
 data=a.scene.read_bytes();_,c,w,h=metadata(data);frames=math.ceil(c[0]*c[23]/c[24]-1e-5);a.output.parent.mkdir(parents=True,exist_ok=True)
 temporary=a.output.with_name(a.output.stem+'.rendering.mov')
 if temporary.exists():raise RuntimeError('Временный файл уже существует: '+str(temporary))
 command=[ffmpeg,'-hide_banner','-loglevel','error','-n','-f','rawvideo','-pixel_format','rgba','-video_size',f'{w}x{h}','-framerate',f'{int(c[23])}/{int(c[24])}','-i','pipe:0','-an','-c:v','qtrle','-pix_fmt','argb',str(temporary)]
 process=subprocess.Popen(command,stdin=subprocess.PIPE)
 try:
  with Native() as n:
   n.load(data)
   for frame in range(frames):process.stdin.write(n.render(frame*c[24]/c[23],w,h))
  process.stdin.close()
  if process.wait(timeout=120):raise RuntimeError('FFmpeg завершился с ошибкой.')
  temporary.rename(a.output)
 except BaseException:
  process.kill();process.wait();temporary.unlink(missing_ok=True);raise
 print(a.output)
if __name__=='__main__':
 try:main()
 except (OSError,RuntimeError,subprocess.SubprocessError) as e:print(str(e),file=sys.stderr);sys.exit(1)
