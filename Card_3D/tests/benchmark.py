#!/usr/bin/env python3
"""Measure release filter only, not Kdenlive playback or video decoding."""
import json, platform, statistics, time, hashlib
import numpy as np
from PIL import Image
from host import Plugin, ROOT


def main():
    cpu = next((s.split(':', 1)[1].strip() for s in open('/proc/cpuinfo') if s.startswith('model name')), platform.processor())
    results = []
    source = Image.open(ROOT/'demo/source_panel.png').convert('RGBA')
    for w, h in [(640,360), (1280,720), (1920,1080)]:
        src = np.array(source.resize((w,h), Image.Resampling.LANCZOS))
        for dynamic in (False, True):
            with Plugin(w,h) as p:
                start = time.perf_counter(); p.render(src,3.0); cold=(time.perf_counter()-start)*1000
                for k in range(3): p.render(src, 3+k/30)
                measurements=[]
                for k in range(12):
                    if dynamic:
                        # Modify a small patch BEFORE timing: force exact-source cache invalidation.
                        src[h//2:h//2+4,w//2:w//2+4,0]=(k*19)%255
                    start=time.perf_counter(); p.render(src,4+k/30); measurements.append((time.perf_counter()-start)*1000)
            results.append(dict(width=w,height=h,source='changing' if dynamic else 'static',measured_frames=12,cold_ms=round(cold,3),median_ms=round(statistics.median(measurements),3),p95_ms=round(float(np.percentile(measurements,95)),3)))
    report=dict(scope='Release Frei0r filter and ctypes output allocation only. Not Kdenlive GUI, not Steam Deck; no decode or encode included.',cpu=cpu,system=platform.platform(),binary_sha256=hashlib.sha256((ROOT/'bin/linux-x86_64/card3d.so').read_bytes()).hexdigest(),settings='Default parameters, including frame, shadow and extrusion. Entrance completed; organic motion active.',results=results)
    (ROOT/'docs/benchmark.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False,indent=2))
if __name__=='__main__':main()
