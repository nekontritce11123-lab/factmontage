#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Optional, on-device actual MLT smoke: discovery and transparent render via a nested .mlt."""
from __future__ import annotations
import argparse, os, shutil, subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from tools.scene import metadata,with_config,mlt_document,atomic_write

def main():
 p=argparse.ArgumentParser();p.add_argument('--native',action='store_true');a=p.parse_args()
 if a.native:
  command=[shutil.which('melt') or 'melt'];env={**os.environ,'QT_QPA_PLATFORM':'offscreen','FREI0R_PATH':str(ROOT/'bin')+':'+os.environ.get('FREI0R_PATH','/usr/lib/frei0r-1')}
 else:
  if not shutil.which('flatpak'):raise RuntimeError('Flatpak не найден')
  command=['flatpak','run','--env=QT_QPA_PLATFORM=offscreen','--command=melt','org.kde.kdenlive'];env=None
 query=subprocess.run(command+['-query','filter=frei0r.sunimo_text_studio'],text=True,capture_output=True,env=env,timeout=30)
 output=query.stdout+query.stderr;print(output)
 if query.returncode or 'SUNIMO Text Studio' not in output:raise RuntimeError('MLT не подтвердил загрузку фильтра. Проверьте FREI0R_PATH, зависимости .so и наличие melt в сборке Kdenlive.')
 base=Path.home()/'Videos/SUNIMO Text';base.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='smoke-',dir=base) as tmp:
  folder=Path(tmp);scene=folder/'test.stxt';mlt=folder/'test.mlt';out=folder/'test.mov'
  data=with_config((ROOT/'examples/demo.stxt').read_bytes(),{0:1,1:.3,2:.3,10:0,11:0,23:24,24:1});atomic_write(scene,data)
  _,cfg,w,h=metadata(data);atomic_write(mlt,mlt_document(scene,cfg,w,h))
  # arg consumer entry order is significant in melt. No shell interpolation.
  args=command+[str(mlt),'in=0','out=23','-consumer','avformat:'+str(out),'vcodec=qtrle','pix_fmt=argb','an=1','real_time=-1']
  result=subprocess.run(args,env=env,capture_output=True,text=True,timeout=120)
  print(result.stdout[-2000:]);print(result.stderr[-4000:])
  if result.returncode or not out.is_file() or out.stat().st_size<1000:raise RuntimeError('MLT обнаружил фильтр, но контрольный рендер не прошёл.')
  dest=base/'SUNIMO_smoke.mov';shutil.copy2(out,dest);print('Контрольный ролик:',dest)
  print('Откройте его поверх цветного фона и проверьте буквы/прозрачность. Наличие файла само по себе не доказывает правильную картинку.')
if __name__=='__main__':
 try:main()
 except (OSError,RuntimeError,subprocess.SubprocessError) as e:print(str(e),file=sys.stderr);sys.exit(1)
