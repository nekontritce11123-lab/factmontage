"""Sync Studio sources into an existing pinned Flatpak Kdenlive build."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def read_cache(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line or line.startswith(('#', '//')) or ':' not in line or '=' not in line:
            continue
        key, value = line.split('=', 1)
        values[key.split(':', 1)[0]] = value
    return values


def pinned_source(root: Path, cache: Path, lock: dict) -> Path:
    archive = cache / 'kdenlive-26.08.0.tar.xz'
    if not archive.exists():
        original = root / 'Studio/upstream/kdenlive-26.08.0.tar.xz'
        if not original.is_file():
            raise ValueError('Pinned Kdenlive archive missing; run ./dev bootstrap explicitly.')
        cache.mkdir(parents=True, exist_ok=True)
        temporary = archive.with_suffix('.tmp')
        shutil.copyfile(original, temporary)
        temporary.replace(archive)
    if digest(archive) != lock['archive_sha256']:
        raise ValueError('Pinned Kdenlive archive hash differs from sources.json')
    source = cache / 'pristine/kdenlive-26.08.0'
    if not source.is_dir():
        source.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as bundle:
            bundle.extractall(source.parent, filter='data')
    if not (source / 'CMakeLists.txt').is_file():
        raise ValueError('Incomplete pristine Kdenlive source')
    return source


def patched_source(cache: Path, pristine: Path, integration: Path, archive_hash: str) -> tuple[Path, dict[str, str]]:
    integration_hash = digest(integration)
    expected = cache / f'patched-{integration_hash[:16]}'
    marker = expected / '.studio-dev-patch.json'
    if not expected.exists():
        with tempfile.TemporaryDirectory(dir=cache, prefix='patch-') as folder:
            candidate = Path(folder) / 'source'
            shutil.copytree(pristine, candidate)
            subprocess.run([sys.executable, str(integration)], cwd=candidate, check=True, timeout=120)
            patches = {}
            for original in pristine.rglob('*'):
                if original.is_file():
                    relative = original.relative_to(pristine)
                    updated = candidate / relative
                    if not updated.is_file():
                        raise ValueError(f'Integration removed pinned source: {relative}')
                    if digest(original) != digest(updated):
                        patches[relative.as_posix()] = digest(updated)
            for updated in candidate.rglob('*'):
                if updated.is_file() and not (pristine / updated.relative_to(candidate)).exists():
                    patches[updated.relative_to(candidate).as_posix()] = digest(updated)
            marker_data = {'archive_sha256': archive_hash, 'integration_sha256': integration_hash,
                           'patches': patches}
            (candidate / marker.name).write_text(json.dumps(marker_data, sort_keys=True), encoding='utf-8')
            candidate.rename(expected)
    if not marker.is_file():
        raise ValueError(f'Incomplete integration snapshot: {expected}')
    data = json.loads(marker.read_text(encoding='utf-8'))
    if data['archive_sha256'] != archive_hash or data['integration_sha256'] != integration_hash:
        raise ValueError('Integration snapshot has a different pinned input')
    for relative, expected_hash in data['patches'].items():
        if digest(expected / relative) != expected_hash:
            raise ValueError(f'Integration snapshot changed: {relative}')
    return expected, data['patches']


def sync_files(cache: Path, build_source: Path, files: dict[str, Path], pristine: Path | None = None,
               patches: dict[str, str] | None = None) -> list[str]:
    cache.mkdir(parents=True, exist_ok=True)
    state_file = cache / 'synced-sources.json'
    previous = json.loads(state_file.read_text(encoding='utf-8')) if state_file.exists() else {}
    if previous and previous['build_source'] != str(build_source):
        raise ValueError('Source sync state belongs to another Kdenlive build')
    for relative, expected_hash in previous.get('files', {}).items():
        target = build_source / relative
        if not target.is_file() or digest(target) != expected_hash:
            raise ValueError(f'Prepared Kdenlive source changed outside dev loop: {relative}')
    if not previous and pristine is not None and patches is not None:
        for original in pristine.rglob('*'):
            if original.is_file():
                relative = original.relative_to(pristine).as_posix()
                target = build_source / relative
                if not target.is_file() or (relative not in patches and digest(original) != digest(target)):
                    raise ValueError(f'Kdenlive cache differs from pinned source: {relative}')
    changed = []
    backup = cache / 'initial-builder-sources'
    for relative, source in files.items():
        target = build_source / relative
        expected_hash = digest(source)
        if target.is_file() and digest(target) == expected_hash:
            continue
        if not previous and target.is_file():
            saved = backup / relative
            if not saved.exists():
                saved.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(target, saved)
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(target.name + '.studio-tmp')
        shutil.copyfile(source, temporary)
        temporary.replace(target)
        changed.append(relative)
    for relative in previous.get('files', {}).keys() - files.keys():
        target = build_source / relative
        original = pristine / relative if pristine else None
        if original and original.is_file():
            temporary = target.with_name(target.name + '.studio-tmp')
            shutil.copyfile(original, temporary)
            temporary.replace(target)
        else:
            target.unlink()
        changed.append(relative)
    state = {'build_source': str(build_source), 'files': {name: digest(path) for name, path in files.items()}}
    temporary = state_file.with_suffix('.tmp')
    temporary.write_text(json.dumps(state, sort_keys=True, indent=2) + '\n', encoding='utf-8')
    temporary.replace(state_file)
    return changed


def prepare(root: Path, host_build: Path) -> dict:
    root, host_build = root.resolve(), host_build.resolve()
    if sys.platform != 'linux' or os.geteuid() == 0 or str(host_build).startswith('/mnt/'):
        raise ValueError('Host integration requires a non-root Linux user and a Linux build directory')
    work = host_build.parents[3]
    build_source = host_build.parent
    active = work / '.flatpak-builder/build/kdenlive'
    if not active.exists() or active.resolve() != build_source or host_build != build_source / '_flatpak_build':
        raise ValueError('Use the active prepared Kdenlive build from STUDIO_BUILD_ROOT')
    values = read_cache(host_build / 'CMakeCache.txt')
    required = {'CMAKE_HOME_DIRECTORY': '/run/build/kdenlive', 'CMAKE_INSTALL_PREFIX': '/app',
                'CMAKE_BUILD_TYPE': 'Release', 'BUILD_TESTING': 'ON', 'FETCH_OTIO': 'OFF',
                'CMAKE_GENERATOR': 'Ninja'}
    for key, expected in required.items():
        if values.get(key) != expected:
            raise ValueError(f'Kdenlive build mismatch: {key}={values.get(key)!r}, expected {expected!r}')
    lock = json.loads((root / 'Studio/upstream/sources.json').read_text(encoding='utf-8-sig'))
    upstream = root / 'Studio/upstream/org.kde.kdenlive.json'
    if digest(upstream).lower() != lock['manifest_sha256'].lower():
        raise ValueError('Pinned upstream manifest differs from sources.json')
    baseline = work / 'upstream/baseline.json'
    baseline_hash = digest(baseline)
    recorded = (work / 'baseline.manifest.sha256').read_text(encoding='utf-8').split()[0]
    if recorded != baseline_hash or not (work / 'baseline/files/bin/kdenlive').is_file():
        raise ValueError('Prepared baseline fingerprint differs or baseline binary is missing')
    spec = importlib.util.spec_from_file_location('studio_prepare', root / 'Studio/scripts/prepare.py')
    prepare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prepare)
    expected_baseline = json.dumps(prepare.baseline_manifest(), ensure_ascii=False, indent=2) + '\n'
    if hashlib.sha256(expected_baseline.encode('utf-8')).hexdigest() != baseline_hash:
        raise ValueError('Current pinned sources no longer match the prepared baseline')
    manifest = work / 'studio-input/local.VideoStudio.Kdenlive.json'
    installed = json.loads(manifest.read_text(encoding='utf-8'))
    kdenlive = next(module for module in installed['modules'] if module['name'] == 'kdenlive')
    if installed['runtime-version'] != '6.10' or kdenlive['sources'][0]['sha256'] != lock['archive_sha256']:
        raise ValueError('Prepared Flatpak stage uses another Kdenlive source or runtime')
    sdk = subprocess.check_output(['flatpak', 'info', '--show-commit', 'org.kde.Sdk//6.10'], text=True).strip()
    runtime = subprocess.check_output(['flatpak', 'info', '--show-commit', 'org.kde.Platform//6.10'], text=True).strip()
    if sdk != lock['kde_sdk_commit'] or runtime != lock['kde_runtime_commit']:
        raise ValueError('Installed Flatpak SDK/runtime differs from pinned sources.json')
    if not (work / 'sdk-check/files/include').is_dir():
        raise ValueError('Prepared SDK build stage with development headers missing')
    cache = work / 'dev-host'
    pristine = pinned_source(root, cache, lock)
    integration = root / 'Studio/scripts/integrate.py'
    expected, patches = patched_source(cache, pristine, integration, lock['archive_sha256'])
    files = {name: expected / name for name in patches}
    overlay = root / 'Studio/overlay'
    for path in overlay.rglob('*'):
        if path.is_file() and '__pycache__' not in path.parts:
            files['src/assets/studio/' + path.relative_to(overlay).as_posix()] = path
    files['src/assets/studio/audio/parameters.hpp'] = root / 'Audio/generated/parameters.hpp'
    files['src/assets/studio/text/scenecompiler.hpp'] = root / 'Text/qt/scenecompiler.hpp'
    files['src/assets/studio/text/styles.hpp'] = root / 'Text/qt/styles.hpp'
    files['tests/studioregressiontest.cpp'] = root / 'Studio/tests/studioregressiontest.cpp'
    files['tests/public_demo.hpp'] = root / 'Studio/tests/public_demo.hpp'
    files['factmontage.svg'] = root / 'Studio/branding/factmontage.svg'
    files['studio-integrate.py'] = integration
    changed = sync_files(cache, build_source, files, pristine, patches)
    runtime_files = {
        'share/kdenlive/effects/card3d.xml': root / 'Card_3D/kdenlive/card3d.xml',
        'share/kdenlive/transitions/studio_transition.xml': root / 'Transitions/kdenlive/studio_transition.xml',
        'share/kdenlive/studio/studio_transition.json': root / 'Transitions/kdenlive/studio_transition.json',
    }
    for sound in sorted((root / 'Transitions/sfx').glob('transition_*.wav')):
        runtime_files[f'share/kdenlive/studio/sfx/v1/{sound.name}'] = sound
    runtime_root = work / 'sdk-check/files'
    changed += sync_files(cache / 'runtime', runtime_root, runtime_files)
    hashes = {name: digest(path) for name, path in files.items()}
    runtime_hashes = {name: digest(path) for name, path in runtime_files.items()}
    cmake = {key: values.get(key) for key in (*required, 'CMAKE_CXX_COMPILER', 'CMAKE_C_COMPILER',
                                              'CMAKE_CXX_FLAGS', 'CMAKE_C_FLAGS',
                                              'CMAKE_EXE_LINKER_FLAGS', 'CMAKE_SHARED_LINKER_FLAGS')}
    evidence = {'source_files': hashes, 'runtime_files': runtime_hashes, 'sdk_commit': sdk, 'runtime_commit': runtime,
                'upstream_archive_sha256': lock['archive_sha256'], 'manifest_sha256': digest(manifest),
                'baseline_manifest_sha256': baseline_hash, 'cmake': cmake, 'host_build': str(host_build)}
    fingerprint = hashlib.sha256(json.dumps(evidence, sort_keys=True).encode()).hexdigest()
    app_evidence = dict(evidence, source_files={name: value for name, value in hashes.items()
                                              if name not in ('tests/studioregressiontest.cpp', 'tests/public_demo.hpp', 'studio-integrate.py')})
    app_evidence.pop('runtime_files')
    app_fingerprint = hashlib.sha256(json.dumps(app_evidence, sort_keys=True).encode()).hexdigest()
    return {'work': work, 'build': host_build, 'manifest': manifest.relative_to(work).as_posix(),
            'files': files, 'hashes': hashes, 'runtime_files': runtime_files, 'runtime_root': runtime_root,
            'runtime_hashes': runtime_hashes, 'sync': changed, 'fingerprint': fingerprint,
            'app_fingerprint': app_fingerprint,
            'evidence': evidence, 'root_inputs': {str(path): digest(path) for path in
                            (integration, root / 'Studio/upstream/sources.json', upstream)}}


def verify_inputs(context: dict) -> None:
    for relative, source in context['files'].items():
        expected = context['hashes'][relative]
        if digest(source) != expected or digest(context['build'].parent / relative) != expected:
            raise ValueError(f'Host source changed during build: {relative}')
    for path, expected in context['root_inputs'].items():
        if digest(Path(path)) != expected:
            raise ValueError(f'Host configuration changed during build: {path}')
    for relative, source in context['runtime_files'].items():
        expected = context['runtime_hashes'][relative]
        if digest(source) != expected or digest(context['runtime_root'] / relative) != expected:
            raise ValueError(f'Host runtime asset changed during build: {relative}')
    values = read_cache(context['build'] / 'CMakeCache.txt')
    for key, expected in context['evidence']['cmake'].items():
        if values.get(key) != expected:
            raise ValueError(f'Host CMake configuration changed during build: {key}')


def source_head(root: Path) -> str | None:
    try:
        return subprocess.check_output(['git', '-c', f'safe.directory={root}',
                                        '-c', f'safe.directory={root / ".git"}', '-C', str(root),
                                        'rev-parse', 'HEAD'], text=True, stderr=subprocess.DEVNULL,
                                       timeout=5).strip()
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return None


def app_ready(context: dict) -> bool:
    state = context['work'] / 'dev-host/app-build.json'
    binary = context['build'] / 'bin/kdenlive'
    if not state.is_file():
        return False
    data = json.loads(state.read_text(encoding='utf-8'))
    if binary.is_file() and data.get('binary_sha256') != digest(binary):
        raise ValueError('Kdenlive binary changed outside dev loop')
    return binary.is_file() and data.get('app_input_sha256') == context['app_fingerprint']


def record_app(context: dict) -> None:
    state = context['work'] / 'dev-host/app-build.json'
    data = {'app_input_sha256': context['app_fingerprint'],
            'binary_sha256': digest(context['build'] / 'bin/kdenlive')}
    temporary = state.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, sort_keys=True, indent=2) + '\n', encoding='utf-8')
    temporary.replace(state)


def check_test_binary(context: dict, target: str) -> None:
    state = context['work'] / 'dev-host' / f'{target}-build.json'
    binary = context['build'] / 'bin' / target
    if state.is_file() and binary.is_file():
        recorded = json.loads(state.read_text(encoding='utf-8'))
        if recorded.get('binary_sha256') != digest(binary):
            raise ValueError(f'{target} binary changed outside dev loop')


def record_test_binary(context: dict, target: str) -> None:
    state = context['work'] / 'dev-host' / f'{target}-build.json'
    data = {'input_sha256': context['fingerprint'],
            'binary_sha256': digest(context['build'] / 'bin' / target)}
    temporary = state.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, sort_keys=True, indent=2) + '\n', encoding='utf-8')
    temporary.replace(state)
