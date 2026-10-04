#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure native hold rendering on THIS host, not Steam Deck. No browser/MLT overhead."""
from __future__ import annotations
import argparse,json,platform,statistics,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from tools.native import Native
from tools.scene import with_config

def main():
 p=argparse.ArgumentParser();p.add_argument('--frames',type=int,default=90);p.add_argument('--json',type=Path);a=p.parse_args()
 if not 5<=a.frames<=1000:p.error('frames must be 5..1000')
 data=(ROOT/'examples/life_v02.stxt').read_bytes();results=[]
 for W,H in ((640,360),(1920,1080)):
  for label,mode,source in [('Неподвижно',0,47),('Волна',9,47),('Глитч',9,64),('Эхо',9,54)]:
   with Native() as n:
    n.load(with_config(data,{0:10,6:0,7:0,10:mode,11:.85,12:1,18:source,22:0}))
    for i in range(5):n.render(i*.5,W,H)
    times=[]
    for i in range(a.frames):
     start=time.perf_counter();n.render(.5+i/a.frames*7,W,H);times.append((time.perf_counter()-start)*1000)
    times.sort();r={'mode':label,'width':W,'height':H,'frames':a.frames,'median_ms':round(statistics.median(times),3),'p95_ms':round(times[min(len(times)-1,int(.95*len(times)))],3)};results.append(r);print(r)
 report={'platform':platform.platform(),'notice':'This container, NOT a Steam Deck. 10 glyphs, no motion blur. Includes Python frame copying, excludes HTTP, MLT, video decoding and encoding. Not a real-device FPS guarantee.','results':results}
 if a.json:a.json.write_text(json.dumps(report,ensure_ascii=False,indent=2))
if __name__=='__main__':main()
