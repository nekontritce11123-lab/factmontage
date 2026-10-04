#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Optional showcase generator; Pillow + ffmpeg needed only for labels/encoding.
Animation frames come directly from the actual C++ engine, not an HTML imitation.
No font files are copied into the package.
"""
from __future__ import annotations
import argparse, json, os, shutil, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from tools.native import Native
from tools.scene import with_config

def main():
 from PIL import Image,ImageDraw,ImageFont
 p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--font',default='/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf');a=p.parse_args()
 if a.output.exists():raise RuntimeError('Output exists')
 ffmpeg=shutil.which('ffmpeg')
 if not ffmpeg:raise RuntimeError('ffmpeg not found')
 W,H,FPS=960,540,30;DURATION=2.8
 font=ImageFont.truetype(a.font,22);small=ImageFont.truetype(a.font,14);big=ImageFont.truetype(a.font,28)
 catalog={p['id']:p for p in json.loads((ROOT/'web/catalog.json').read_text())}
 ids=[2,3,10,21,31,35,42,47,51,63,74,100]
 data=(ROOT/'examples/demo.stxt').read_bytes()
 command=[ffmpeg,'-hide_banner','-loglevel','error','-n','-f','rawvideo','-pixel_format','rgba','-video_size',f'{W}x{H}','-framerate',str(FPS),'-i','pipe:0','-an','-c:v','libx264','-preset','fast','-crf','19','-pix_fmt','yuv420p','-movflags','+faststart',str(a.output)]
 process=subprocess.Popen(command,stdin=subprocess.PIPE)
 try:
  for index,id in enumerate(ids):
   recipe=catalog[id]
   bg=Image.new('RGBA',(W,H),(17,20,27,255));draw=ImageDraw.Draw(bg)
   draw.rounded_rectangle((36,28,75,67),radius=12,fill=(255,195,105));draw.text((47,31),'S',font=big,fill=(28,27,24))
   draw.text((89,29),'FactMontage / Text',font=font,fill=(238,237,234));draw.text((90,58),'Нативный C++-рендер · без имитации в браузере',font=small,fill=(134,141,155))
   draw.line((36,91,W-36,91),fill=(48,53,64),width=1)
   draw.text((36,H-99),f'{id:03d}   {recipe["name"]}',font=font,fill=(244,238,227))
   draw.text((36,H-67),recipe['category']+'  ·  выход: реверс',font=small,fill=(143,150,164))
   draw.text((W-96,H-68),f'{index+1:02d} / 12',font=small,fill=(255,195,105))
   with Native() as n:
    n.load(with_config(data,{0:DURATION,1:1.05,2:.8,6:id,7:-1,8:recipe['group'],9:recipe['order'],10:0,11:0,14:0,15:0,16:0,17:0,22:0,23:FPS,24:1}))
    bgbytes=bg.tobytes()
    for f in range(round(DURATION*FPS)):
     t=f/FPS;frame=Image.frombytes('RGBA',(W,H),n.render(t,W,H,bgbytes));dr=ImageDraw.Draw(frame)
     phase='ПОЯВЛЕНИЕ' if t<1.05 else 'ЧТЕНИЕ' if t<DURATION-.8 else 'ИСЧЕЗНОВЕНИЕ'
     dr.text((36,112),phase,font=small,fill=(110,119,136))
     dr.rounded_rectangle((36,H-28,W-36,H-25),radius=1,fill=(47,53,66))
     dr.rounded_rectangle((36,H-28,36+(W-72)*t/DURATION,H-25),radius=1,fill=(255,195,105))
     process.stdin.write(frame.tobytes())
     if index==0 and f==12:frame.save(a.output.with_suffix('.png'))
    for _ in range(6):process.stdin.write(bgbytes)
   print(id,recipe['name'],flush=True)
  process.stdin.close()
  if process.wait(timeout=120):raise RuntimeError('ffmpeg failed')
 except BaseException:
  process.kill();process.wait();raise
 print(a.output)
if __name__=='__main__':main()
