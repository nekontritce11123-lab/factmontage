#!/usr/bin/env python3
"""Bind build/test evidence to exact prepared inputs and installed files."""
import argparse
import hashlib
import json
import stat
from pathlib import Path


def inventory(root):
    result = {}
    for path in sorted(root.rglob('*')):
        name = path.relative_to(root).as_posix()
        if path.is_symlink():
            result[name] = 'link:' + str(path.readlink())
        elif path.is_file():
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(chunk)
            result[name] = f'{stat.S_IMODE(path.stat().st_mode):o}:' + digest.hexdigest()
    if not result:
        raise ValueError(f'Empty build directory: {root}')
    return result


def snapshot(work, level='build'):
    result = {name: inventory(work/name) for name in ('studio-input', 'studio/files')}
    result['metadata'] = hashlib.sha256((work/'studio/metadata').read_bytes()).hexdigest()
    if level == 'tests':
        result['test-results'] = inventory(work/'test-results')
    return result


def verify(work, level):
    receipt = json.loads((work/(level + '-verified.json')).read_text(encoding='utf-8'))
    if receipt != snapshot(work, level):
        raise ValueError('Build inputs or installed files changed; rebuild and repeat checks.')
    return receipt


def record(work, level):
    if level == 'tests':
        verify(work, 'build')
    elif json.loads((work/'build-started.json').read_text(encoding='utf-8')) != inventory(work/'studio-input'):
        raise ValueError('Prepared inputs changed during the build; rebuild before recording success.')
    data = snapshot(work, level)
    target = work/(level + '-verified.json')
    temporary = target.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, sort_keys=True, indent=2) + '\n', encoding='utf-8')
    temporary.replace(target)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=('begin', 'record', 'verify'))
    parser.add_argument('level', choices=('build', 'tests'))
    parser.add_argument('work', type=Path)
    args = parser.parse_args()
    if args.action == 'begin':
        (args.work/'build-started.json').write_text(json.dumps(inventory(args.work/'studio-input')), encoding='utf-8')
        for name in ('build-verified.json', 'tests-verified.json'):
            (args.work/name).unlink(missing_ok=True)
    else:
        (record if args.action == 'record' else verify)(args.work, args.level)
