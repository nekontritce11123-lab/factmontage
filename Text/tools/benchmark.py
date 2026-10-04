#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Benchmark the actual native renderer on THIS computer, excluding MLT/browser overhead."""
from __future__ import annotations
import argparse, json, platform, statistics, sys, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from tools.native import Native
from tools.scene import with_config

def main():
 p=argparse.ArgumentParser();p.add_argument('--frames',type=int,default=90);p.add_argument('--json',type=Path);a=p.parse_args()
 if not 5<=a.frames<=1000:p.error('--frames must be 5..1000')
 data=(ROOT/'examples/demo.stxt').read_bytes();results=[]
 for w,h,mode,params in [(768,432,'Подъём / preview',{}),(1920,1080,'Подъём / 1080p',{}),(1920,1080,'Поворот / 1080p',{6:31}),(1920,1080,'Расфокус / 1080p',{6:51}),(1920,1080,'Временное размытие ×3',{22:.6,13:1}),(1920,1080,'Временное размытие ×5',{22:.6,13:2})]:
  with Native() as n:
   start=time.perf_counter();n.load(with_config(data,{6:2,7:-1,10:0,11:0,**params}));load=(time.perf_counter()-start)*1000
   for i in range(5):n.render(.15+i*.07,w,h)
   timings=[]
   for i in range(a.frames):
    start=time.perf_counter();n.render((i%a.frames)/a.frames*3,w,h);timings.append((time.perf_counter()-start)*1000)
   timings.sort();r={'mode':mode,'width':w,'height':h,'frames':a.frames,'median_ms':round(statistics.median(timings),3),'p95_ms':round(timings[min(len(timings)-1,int(.95*len(timings)) )],3),'load_ms':round(load,3)};results.append(r);print(r)
 report={'platform':platform.platform(),'machine':platform.machine(),'libc':platform.libc_ver(),'notice':'Измерения этой машины, НЕ Steam Deck. Без расходов MLT, декодирования и кодирования. 10 букв/знаков, исходник 1920×1080. Python wrapper copies returned frame.','results':results}
 if a.json:a.json.write_text(json.dumps(report,ensure_ascii=False,indent=2))
if __name__=='__main__':main()
