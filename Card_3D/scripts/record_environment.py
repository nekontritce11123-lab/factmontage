#!/usr/bin/env python3
"""Record actual tools and binary identity, without guessing unavailable versions."""
from pathlib import Path
import hashlib
import json
import platform
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]


def version(args):
    if not shutil.which(args[0]):return 'unavailable'
    try:
        result=subprocess.run(args,capture_output=True,text=True,timeout=30)
        return (result.stdout+result.stderr).strip() if result.returncode==0 else 'unavailable: '+(result.stdout+result.stderr).strip()
    except (OSError,subprocess.TimeoutExpired) as e:return 'unavailable: '+str(e)


if __name__=='__main__':
    report={'system':platform.platform(),'python':sys.version,'compiler':version(['g++','--version']),
            'mlt':version(['melt','--version']),'ffmpeg':version(['ffmpeg','-version']),
            'kdenlive_flatpak':version(['flatpak','info','org.kde.kdenlive']),
            'flatpak_runtime':version(['flatpak','info','--show-runtime','org.kde.kdenlive'])}
    binary=ROOT/'bin/linux-x86_64/card3d.so'
    report['linux_binary_sha256']=hashlib.sha256(binary.read_bytes()).hexdigest() if binary.is_file() else 'not built'
    (ROOT/'docs/environment.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False,indent=2))
