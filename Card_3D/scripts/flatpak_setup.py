#!/usr/bin/env python3
"""Local, reversible Flatpak setup. Python standard library only.
Does not install packages, download anything, modify projects, reset caches,
replace other plugins, or grant any new Flatpak file-system permissions.
"""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APP = 'org.kde.kdenlive'
NAME = 'card3d'
BIN_NAME = NAME + '.so'
XML_NAME = NAME + '.xml'
HOME = Path.home().resolve()
STATE = Path(os.environ.get('XDG_STATE_HOME', str(HOME / '.local/state'))) / NAME
MANIFEST = STATE / 'installation.json'

class SetupError(RuntimeError): pass

def run(args: list[str], check: bool = True) -> str:
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise SetupError(str(exc)) from exc
    if check and p.returncode:
        raise SetupError((p.stderr or p.stdout or f'Код ошибки {p.returncode}').strip())
    return p.stdout

def fp(*args: str, check: bool = True) -> str:
    return run(['flatpak', *args], check)

def under_home(path: Path) -> bool:
    try: path.resolve().relative_to(HOME); return True
    except ValueError: return False

def environment() -> dict[str, str]:
    text = fp('run', '--command=sh', APP, '-c',
              'printf "CARD3D_DATA=%s\\n" "$XDG_DATA_HOME"; '
              'printf "CARD3D_FREI0R=%s\\n" "${FREI0R_PATH-}"; '
              'printf "CARD3D_MLT=%s\\n" "${MLT_FREI0R_PLUGIN_PATH-}"')
    values = dict(line.split('=', 1) for line in text.splitlines() if line.startswith('CARD3D_') and '=' in line)
    if not values.get('CARD3D_DATA'):
        raise SetupError('Не удалось определить папку данных внутри Flatpak. Ничего не установлено.')
    if not under_home(Path(values['CARD3D_DATA'])):
        raise SetupError('Папка данных находится вне домашнего каталога. Автоматическая установка остановлена.')
    return values

def preflight(require_closed: bool = True) -> None:
    if hasattr(os, 'geteuid') and os.geteuid() == 0:
        raise SetupError('Запускайте от своего пользователя, без sudo.')
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'amd64'):
        raise SetupError('В архиве собран бинарник только для Linux x86-64 / Steam Deck.')
    if not under_home(STATE):
        raise SetupError('Каталог журнала установки должен находиться внутри домашней папки.')
    if not shutil.which('flatpak'):
        raise SetupError('Flatpak не найден. Этот установщик предназначен для Kdenlive Flatpak.')
    fp('info', APP)
    if require_closed:
        running = fp('ps', '--columns=application', check=False).splitlines()
        if APP in (line.strip() for line in running):
            raise SetupError('Сначала сохраните проект и полностью закройте Kdenlive. Установщик его не закрывает.')

def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def atomic_bytes(path: Path, data: bytes, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix='.card3d-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as out:
            out.write(data); out.flush(); os.fsync(out.fileno())
        os.chmod(tmp, mode); os.replace(tmp, path)
    finally:
        if os.path.exists(tmp): os.unlink(tmp)

def save_manifest(data: dict) -> None:
    atomic_bytes(MANIFEST, (json.dumps(data, ensure_ascii=False, indent=2)+'\n').encode())

def read_manifest() -> dict | None:
    if not MANIFEST.is_file(): return None
    return json.loads(MANIFEST.read_text(encoding='utf-8'))

def set_frei0r(value: str) -> None:
    option = f'--env=FREI0R_PATH={value}' if value else '--unset-env=FREI0R_PATH'
    fp('override', '--user', option, APP)

def restore_environment(state: dict) -> None:
    if not state.get('path_added'): return
    current = environment().get('CARD3D_FREI0R', '')
    # Restore the original value only when ours is still the latest whole value.
    # Otherwise remove only our one path, preserving later user additions.
    if current == state['new_frei0r']:
        set_frei0r(state['old_frei0r'])
    elif state['path_added'] in current.split(':'):
        remaining = ':'.join(x for x in current.split(':') if x != state['path_added'])
        set_frei0r(remaining)

def restore_files(state: dict) -> list[str]:
    warnings = []
    for entry in state.get('files', []):
        target = Path(entry['target'])
        if not under_home(target) or target.name not in (BIN_NAME, XML_NAME):
            raise SetupError('Неверный путь в журнале установки. Удаление остановлено.')
        if target.exists():
            if target.is_symlink() or digest(target) != entry['installed_sha256']:
                warnings.append(f'Файл изменён после установки, оставлен: {target}')
                continue
            backup = Path(entry['backup']) if entry.get('backup') else None
            if backup:
                if not under_home(backup) or not backup.is_file():
                    raise SetupError(f'Не найдена резервная копия: {backup}')
                atomic_bytes(target, backup.read_bytes(), entry.get('old_mode', 0o644))
            else: target.unlink()
    return warnings

def probe() -> tuple[bool | None, str]:
    # Does not start the editor. Not every Kdenlive Flatpak contains melt.
    out = fp('run', '--command=sh', APP, '-c',
             'for b in melt melt-7; do '
             'if command -v "$b" >/dev/null 2>&1; then '
             'printf "CARD3D_ENGINE_AVAILABLE\\n"; '
             '"$b" --version 2>&1; '
             'QT_QPA_PLATFORM=offscreen "$b" -query filter=frei0r.card3d 2>&1; '
             'exit 0; fi; done; printf "CARD3D_ENGINE_UNAVAILABLE\\n"')
    if 'CARD3D_ENGINE_UNAVAILABLE' in out: return None, out
    good = 'identifier: frei0r.card3d' in out or 'title: Card 3D' in out
    return good, out

def install() -> None:
    preflight()
    for p in (ROOT/'bin/linux-x86_64'/BIN_NAME, ROOT/'kdenlive'/XML_NAME):
        if not p.is_file(): raise SetupError(f'Не хватает файла архива: {p.name}. Распакуйте архив полностью.')
    with (ROOT/'bin/linux-x86_64'/BIN_NAME).open('rb') as binary:
        header = binary.read(20)
    if len(header) < 20 or header[:6] != b'\x7fELF\x02\x01' or header[16:20] != b'\x03\x00\x3e\x00':
        raise SetupError('Нужна библиотека ELF Linux x86-64. Windows DLL или другая архитектура не подходит.')
    previous = read_manifest()
    if previous and previous.get('active'):
        files = previous.get('files', [])
        if files and all(Path(e['target']).is_file() and digest(Path(e['target'])) == e['installed_sha256'] for e in files):
            payload = {BIN_NAME: ROOT/'bin/linux-x86_64'/BIN_NAME,
                       XML_NAME: ROOT/'kdenlive'/XML_NAME}
            if all(e['installed_sha256'] == digest(payload[Path(e['target']).name]) for e in files):
                print('Эта установка уже зарегистрирована. Для диагностики: bash CHECK.sh')
                return
            raise SetupError('В архиве другая сборка. Сначала удалите прежнюю через UNINSTALL.sh, затем повторите установку.')
        raise SetupError('Есть предыдущая установка с изменёнными файлами. Сначала запустите UNINSTALL.sh и проверьте его сообщения.')
    env = environment()
    data = Path(env['CARD3D_DATA'])
    old = env.get('CARD3D_FREI0R', '')
    inherited = old or env.get('CARD3D_MLT', '')
    # Reuse the existing writable plugin path when possible. This preserves the
    # user's already-working Camera Shake Organic configuration without changes.
    plugin_dir = None
    for raw in inherited.split(':'):
        if not raw or '$' in raw or '\n' in raw: continue
        candidate = Path(raw)
        # Do not place v2 inside a directory owned by v1's reversible installer.
        # Removing v1 must not remove the only search path used by v2.
        if any(name in candidate.parts for name in ('sunimo_organic_perspective', 'sunimo_card_studio')):
            continue
        if candidate.is_absolute() and under_home(candidate) and candidate.is_dir() and os.access(candidate, os.W_OK):
            plugin_dir = candidate; break
    added = ''
    new = old
    if plugin_dir is None:
        plugin_dir = data / NAME / 'plugins'
        added = str(plugin_dir)
        fallback = ':'.join(['/app/lib/frei0r-1','/app/lib64/frei0r-1',
                            '/usr/lib/frei0r-1','/usr/lib64/frei0r-1',
                            '/usr/lib/x86_64-linux-gnu/frei0r-1','/usr/local/lib/frei0r-1',
                            str(HOME/'.frei0r-1/lib')])
        new = added + ':' + (inherited or fallback)
    xml_dir = data / 'kdenlive/effects'
    pairs = [(ROOT/'bin/linux-x86_64'/BIN_NAME,plugin_dir/BIN_NAME,0o755),
             (ROOT/'kdenlive'/XML_NAME,xml_dir/XML_NAME,0o644)]
    STATE.mkdir(parents=True, exist_ok=True)
    state = {'version':'1.0','active':False,'app':APP,'files':[],
             'old_frei0r':old,'new_frei0r':new,'path_added':added}
    try:
        for source,target,mode in pairs:
            if not under_home(target) or target.is_symlink():
                raise SetupError(f'Небезопасный или символьный путь, файл не заменён: {target}')
            entry = {'target':str(target),'installed_sha256':digest(source),'backup':None}
            if target.exists():
                backup = STATE/'backups'/target.name
                entry['old_mode'] = target.stat().st_mode & 0o777
                atomic_bytes(backup,target.read_bytes(),entry['old_mode'])
                entry['backup'] = str(backup)
            state['files'].append(entry)
            save_manifest(state)
            atomic_bytes(target,source.read_bytes(),mode)
        if added: set_frei0r(new)
        fp('run','--command=sh',APP,'-c','test -r "$1" && test -r "$2"','sh',str(pairs[0][1]),str(pairs[1][1]))
        loaded, detail = probe()
        atomic_bytes(STATE/'loader-check.txt',detail.encode())
        if loaded is False:
            raise SetupError('MLT не обнаружил новый фильтр. Изменения будут отменены. Подробности: '+str(STATE/'loader-check.txt'))
        state['active']=True; save_manifest(state)
        print('\nУстановлено: Card 3D')
        print('Плагин: '+str(pairs[0][1]))
        print('Интерфейс: '+str(pairs[1][1]))
        if loaded: print('Проверка загрузки MLT: успешно.')
        else: print('В этой сборке нет melt: загрузку MLT проверить автоматически нельзя. Файлы доступны внутри Flatpak.')
        print('Запустите Kdenlive. В поиске эффектов введите CARD3D.')
        print('Проекты, другие плагины и разрешения доступа Flatpak не изменялись.')
    except Exception:
        try:
            restore_environment(state)
            for warning in restore_files(state): print(warning,file=sys.stderr)
            state['active']=False; save_manifest(state)
        except Exception as exc:
            print('Не удалось полностью отменить установку: '+str(exc),file=sys.stderr)
        raise

def uninstall() -> None:
    preflight()
    state=read_manifest()
    if not state or not state.get('active'):
        print('Активная установка не зарегистрирована. Другие файлы не затронуты.'); return
    changed = [str(e['target']) for e in state.get('files', [])
               if Path(e['target']).exists() and
               (Path(e['target']).is_symlink() or digest(Path(e['target'])) != e['installed_sha256'])]
    if changed:
        print('После установки изменены файлы. Автоматическое удаление не выполнялось:')
        print('\n'.join(changed))
        return
    warnings=restore_files(state)
    if warnings:
        for warning in warnings: print(warning)
        print('Путь поиска оставлен, чтобы не отключить изменённый файл. Проверьте файлы вручную.')
        return
    restore_environment(state)
    state['active']=False; save_manifest(state)
    print('Плагин удалён; прежние одноимённые файлы восстановлены, если они были.')
    print('Остальные плагины и настройки не сбрасывались. Журнал и резервные копии: '+str(STATE))

def check() -> None:
    preflight(require_closed=False)
    env=environment(); print('Сведения Flatpak:')
    print(fp('info',APP).strip())
    print('\nПути:'); print(json.dumps(env,ensure_ascii=False,indent=2))
    state=read_manifest()
    if not state: print('Журнал установки не найден.'); return
    print('\nФайлы:')
    for entry in state.get('files',[]):
        path=Path(entry['target'])
        print(str(path)+': '+('SHA256 OK' if path.is_file() and digest(path)==entry['installed_sha256'] else 'ОТСУТСТВУЕТ / ИЗМЕНЁН'))
    loaded,details=probe()
    print('\nПроверка загрузчика:\n'+details)
    print('Результат: '+('MLT видит плагин' if loaded else 'нет melt для проверки' if loaded is None else 'MLT не видит плагин'))

if __name__=='__main__':
    actions={'install':install,'uninstall':uninstall,'check':check}
    try:
        if len(sys.argv)!=2 or sys.argv[1] not in actions: raise SetupError('Команда: install, uninstall или check')
        actions[sys.argv[1]]()
    except (SetupError,OSError,ValueError) as exc:
        print('\nОшибка: '+str(exc),file=sys.stderr); sys.exit(1)
