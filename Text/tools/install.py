#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""User-only install. Never reset Flatpak permissions, delete other effects, or use sudo."""
from __future__ import annotations
import argparse, ctypes, datetime, hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from tools.scene import atomic_write
ROOT=Path(__file__).resolve().parents[1]
APP='org.kde.kdenlive'
NAME='sunimo_text_studio'
HOME=Path.home()
STATE=HOME/'.local/share/sunimo-text-studio'

def run(args:list[str],check:bool=True)->str:
    p=subprocess.run(args,capture_output=True,text=True,timeout=35)
    if check and p.returncode:raise RuntimeError(f'Команда завершилась с ошибкой: {" ".join(args)}\n{p.stderr.strip()}\n{p.stdout.strip()}')
    return p.stdout.strip()

def merged_paths(own:str,existing:str,defaults:list[str])->str:
    result=[]
    for p in [own,*existing.split(':'),*defaults]:
        if p and p not in result:result.append(p)
    return ':'.join(result)

def effective_path()->str:
    return run(['flatpak','run','--command=sh',APP,'-c','printf "%s" "$FREI0R_PATH"'])

def targets(mode:str):
    data=HOME/'.var/app'/APP/'data' if mode=='flatpak' else Path(os.environ.get('XDG_DATA_HOME',str(HOME/'.local/share')))
    plugin=(data/'frei0r-1'/NAME) if mode=='flatpak' else HOME/'.frei0r-1/lib'/NAME
    effects=data/'kdenlive/effects'
    return plugin,effects

def digest(path:Path)->str:return hashlib.sha256(path.read_bytes()).hexdigest()

def install(mode:str):
    binary=ROOT/'bin'/f'{NAME}.so'
    if not binary.is_file():raise RuntimeError('Нет бинарной библиотеки. Сначала bash build.sh.')
    if os.uname().machine!='x86_64':raise RuntimeError('Готовая сборка рассчитана на Linux x86_64. На другой архитектуре сначала пересоберите исходники.')
    # Prevent an unusable preview installation on an older host libc.
    try:ctypes.CDLL(str(binary))
    except OSError as e:raise RuntimeError(f'Библиотека не загружается в основной системе: {e}\nПересоберите под совместимую среду: docs/INSTALL_RU.md.')
    previous=''
    if mode=='flatpak':
        if not shutil.which('flatpak'):raise RuntimeError('Flatpak не найден. Для обычного Kdenlive: bash install.sh --native')
        run(['flatpak','info',APP]);previous=effective_path()
    plugin,effects=targets(mode);plugin.mkdir(parents=True,exist_ok=True)
    candidate=plugin/f'.{NAME}.candidate.so'
    atomic_write(candidate,binary.read_bytes())
    installed=[]
    try:
        if mode=='flatpak':
            # Test dependencies INSIDE the installed runtime, not only on SteamOS host.
            cmd='if command -v ldd >/dev/null 2>&1; then ldd "$1"; else for l in /lib64/ld-linux-x86-64.so.2 /usr/lib/ld-linux-x86-64.so.2; do if test -x "$l"; then exec "$l" --list "$1"; fi; done; exit 77; fi'
            result=run(['flatpak','run','--command=sh',APP,'-c',cmd,'sunimo-check',str(candidate)])
            if 'not found' in result or 'version `' in result:raise RuntimeError('Среда Kdenlive не принимает библиотеку:\n'+result+'\nСм. docs/INSTALL_RU.md')
            print('Зависимости в среде Kdenlive:',result,sep='\n')
        stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        manifest_path=STATE/f'install-{mode}.json'
        prior=json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
        record={'mode':mode,'version':'0.1.0','files':[],'own_path':str(plugin),'before_path':prior.get('before_path',previous),'time':stamp}
        effects.mkdir(parents=True,exist_ok=True)
        # Only our two files are touched; preserve the previous versions separately.
        for src,dst in [(binary,plugin/f'{NAME}.so'),(ROOT/'kdenlive'/f'{NAME}.xml',effects/f'{NAME}.xml')]:
            previous_record=next((f for f in prior.get('files',[]) if f['path']==str(dst)),None)
            backup=previous_record.get('backup') if previous_record else None
            if dst.exists() and not previous_record:
                b=STATE/'backups'/stamp/dst.name;b.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(dst,b);backup=str(b)
            installed.append((dst,dst.read_bytes() if dst.exists() else None))
            atomic_write(dst,src.read_bytes())
            record['files'].append({'path':str(dst),'sha256':digest(dst),'backup':backup})
        if mode=='flatpak':
            defaults=[str(plugin.parent),str(HOME/'.frei0r-1/lib'),'/app/lib/frei0r-1','/app/lib/x86_64-linux-gnu/frei0r-1','/usr/lib/frei0r-1']
            search=merged_paths(str(plugin),previous,defaults)
            exports=HOME/'Videos/SUNIMO Text';exports.mkdir(parents=True,exist_ok=True)
            before=run(['flatpak','override','--user','--show',APP],check=False)
            STATE.mkdir(parents=True,exist_ok=True)
            atomic_write(STATE/f'flatpak-before-{stamp}.txt',before.encode())
            # Grants READ access only to exported text scenes; preserves unrelated overrides.
            run(['flatpak','override','--user','--env=FREI0R_PATH='+search,'--filesystem='+str(exports)+':ro',APP])
            record['after_path']=search
        atomic_write(manifest_path,json.dumps(record,ensure_ascii=False,indent=2).encode())
    except BaseException:
        for path,old in reversed(installed):
            if old is None:path.unlink(missing_ok=True)
            else:atomic_write(path,old)
        raise
    finally:
        candidate.unlink(missing_ok=True)
    print('\nУстановлено:',plugin/f'{NAME}.so')
    print('Полностью закройте и снова запустите Kdenlive. Панель: bash launch.sh')
    print('Это первая сборка. Проверка реального MLT: bash doctor.sh --mlt-smoke')

def uninstall(mode:str):
    manifest_path=STATE/f'install-{mode}.json'
    if not manifest_path.is_file():raise RuntimeError('Нет записи об установке; чужие файлы не удалены.')
    record=json.loads(manifest_path.read_text())
    for f in record['files']:
        path=Path(f['path'])
        if path.exists() and digest(path)!=f['sha256']:
            print('Пропущен изменённый после установки файл:',path);continue
        if f.get('backup') and Path(f['backup']).is_file():atomic_write(path,Path(f['backup']).read_bytes());print('Восстановлена предыдущая версия:',path)
        else:path.unlink(missing_ok=True)
    if mode=='flatpak' and shutil.which('flatpak'):
        current=effective_path();own=record['own_path']
        # Remove only our dedicated directory. Never reset global/user app permissions.
        if own not in record.get('before_path','').split(':'):
            remaining=':'.join(p for p in current.split(':') if p and p!=own)
            run(['flatpak','override','--user','--env=FREI0R_PATH='+remaining,APP])
    manifest_path.unlink()
    print('Наши файлы удалены или восстановлены из резервной копии. Остальные эффекты не менялись.')
    print('Экспортированные .stxt/.mlt, резервные копии и разрешение чтения папки титров сохранены.')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',action='store_true');p.add_argument('--uninstall',action='store_true')
    a=p.parse_args()
    if hasattr(os,'geteuid') and os.geteuid()==0:
        print('Не запускайте установку через sudo/root. Используйте обычного пользователя deck.',file=sys.stderr);return 1
    try:(uninstall if a.uninstall else install)('native' if a.native else 'flatpak')
    except (OSError,RuntimeError,ValueError,subprocess.SubprocessError) as e:print(str(e),file=sys.stderr);return 1
    return 0
if __name__=='__main__':sys.exit(main())
