#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""v0.2 lifecycle showcase. Every animated pixel comes from the actual C++ renderer.
Pillow is used only for fixed labels; ffmpeg encodes MP4. Fonts are never packaged.
"""
from __future__ import annotations
import argparse,json,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from tools.native import Native
from tools.scene import with_config

def main():
 from PIL import Image,ImageDraw,ImageFont
 p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--font',default='/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf');a=p.parse_args()
 if a.output.exists():p.error('Output exists')
 ffmpeg=shutil.which('ffmpeg')
 if not ffmpeg:p.error('ffmpeg not installed')
 W,H,FPS,D=960,540,30,7.5
 title=ImageFont.truetype(a.font,21);small=ImageFont.truetype(a.font,14);tag=ImageFont.truetype(a.font,13)
 catalog={p['id']:p for p in json.loads((ROOT/'web/catalog.json').read_text())}
 # The middle phase gets 5.3 s, not just a token pause between two transitions.
 scenes=[(47,False),(21,False),(31,False),(64,False),(10,False),(54,False),(91,False),(47,True)]
 data=(ROOT/'examples/life_v02.stxt').read_bytes();a.output.parent.mkdir(parents=True,exist_ok=True)
 cmd=[ffmpeg,'-hide_banner','-loglevel','error','-n','-f','rawvideo','-pixel_format','rgba','-video_size',f'{W}x{H}','-framerate',str(FPS),'-i','pipe:0','-an','-c:v','libx264','-preset','fast','-crf','19','-pix_fmt','yuv420p','-movflags','+faststart',str(a.output)]
 process=subprocess.Popen(cmd,stdin=subprocess.PIPE)
 try:
  with Native() as n:
   for index,(id,alone) in enumerate(scenes):
    recipe=catalog[id];life=recipe['life']
    bg=Image.new('RGBA',(W,H),(17,20,27,255));draw=ImageDraw.Draw(bg)
    draw.text((34,28),'FactMontage / Text',font=title,fill=(246,240,230))
    draw.text((34,61),'Появление → свой характер жизни → исчезновение',font=small,fill=(163,169,184))
    draw.line((34,94,W-34,94),fill=(48,53,64))
    heading='БЕЗ ВХОДА И ВЫХОДА' if alone else f'{id:03d}  {recipe["name"]}'
    draw.text((34,H-112),heading,font=title,fill=(246,240,230))
    draw.text((34,H-80),life['name'],font=small,fill=(255,195,105))
    note='Тот же характер движения работает отдельно от переходов' if alone else 'Средняя часть: 5,3 с жизни текста · выход: реверс'
    draw.text((34,H-57),note,font=tag,fill=(154,161,177))
    draw.text((W-90,H-80),f'{index+1:02d} / 08',font=small,fill=(163,169,184))
    n.load(with_config(data,{0:D,1:1.2,2:1,6:0 if alone else id,7:0 if alone else -1,8:recipe['group'],9:recipe['order'],10:9,11:.85,12:1,14:0,15:0,16:0,17:0,18:id if alone else 0,22:0,23:FPS,24:1}))
    background=bg.tobytes()
    for f in range(round(D*FPS)):
     t=f/FPS;frame=Image.frombytes('RGBA',(W,H),n.render(t,W,H,background));dr=ImageDraw.Draw(frame)
     phase='ЖИЗНЬ БЕЗ ПЕРЕХОДОВ' if alone else 'ПОЯВЛЕНИЕ' if t<1.2 else 'ЖИЗНЬ ТЕКСТА' if t<D-1 else 'ИСЧЕЗНОВЕНИЕ'
     dr.text((34,116),phase,font=tag,fill=(255,195,105) if alone or 1.2<=t<D-1 else (145,153,171))
     dr.line((34,H-25,W-34,H-25),fill=(48,53,64),width=3)
     dr.line((34,H-25,34+(W-68)*t/D,H-25),fill=(255,195,105),width=3)
     process.stdin.write(frame.tobytes())
     if f==90:frame.save(a.output.parent/f'life_{id:03d}_{"only" if alone else "full"}.png')
    print(id,'life only' if alone else recipe['name'],flush=True)
  process.stdin.close()
  if process.wait(timeout=60):raise RuntimeError('ffmpeg failed')
 except BaseException:process.kill();process.wait();raise
 print(a.output)
if __name__=='__main__':main()
