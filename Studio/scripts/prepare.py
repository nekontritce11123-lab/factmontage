#!/usr/bin/env python3
"""Prepare reproducible local Flatpak input; this does not build or release it."""
from pathlib import Path
import gzip
import hashlib
import io
import json
import shutil
import tarfile
import argparse
import os

ROOT = Path(__file__).resolve().parents[1]
APP_ID = 'local.VideoStudio.Kdenlive'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def baseline_manifest():
    lock = json.loads((ROOT/'upstream/sources.json').read_text(encoding='utf-8-sig'))
    source = ROOT/'upstream/org.kde.kdenlive.json'
    assert sha(source) == lock['manifest_sha256'].lower(), 'Upstream manifest changed'
    manifest = json.loads(source.read_text(encoding='utf-8'))
    otio = next(m for m in manifest['modules'] if m['name'] == 'opentimelineio')
    otio['sources'][0]['commit'] = lock['opentimelineio_commit']
    assert manifest['runtime-version'] == '6.10'
    module = manifest['modules'][-1]
    assert module['name'] == 'kdenlive'
    assert module['sources'][0]['sha256'] == lock['archive_sha256']
    # SDK 6.10 Meson chooses lib64, but this Flatpak links/searches /app/lib.
    # Apply identically to baseline and Studio; source archives stay unchanged.
    inih = next(m for m in manifest['modules'] if m['name'] == 'inih')
    inih['config-opts'].append('--libdir=lib')
    libvpl = next(m for m in manifest['modules'] if m['name'] == 'oneapi-libvpl')
    libvpl['config-opts'].append('-DCMAKE_INSTALL_LIBDIR=lib')
    mlt = next(m for m in manifest['modules'] if m['name'] == 'mlt')
    mlt['config-opts'].append('-DCMAKE_INSTALL_LIBDIR=lib')
    frei0r = next(m for m in manifest['modules'] if m['name'] == 'frei0r-plugins')
    frei0r['config-opts'].append('-DCMAKE_INSTALL_LIBDIR=lib')
    ffmpeg = next(m for m in manifest['modules'] if m['name'] == 'ffmpeg')
    ffmpeg['build-options']['prepend-pkg-config-path'] = '/app/lib64/pkgconfig'
    # Upstream's Bigsh0t command assumes the frei0r directory already exists.
    # A clean SDK does not create it before this module.
    bigsh0t = next(m for m in manifest['modules'] if m['name'] == 'bigsh0t')
    bigsh0t['post-install'] = [
        'mkdir -p /app/lib/frei0r-1',
        'cp -v bigsh0t-*-linux/lib/frei0r-1/*.so /app/lib/frei0r-1',
    ]
    # SDK 6.10 puts vidstab in lib64 while Flatpak's loader searches /app/lib.
    # Keep the upstream build intact and expose only the SONAME MLT needs.
    manifest['modules'].append(dict(name='flatpak-lib-compat', buildsystem='simple',
        **{'build-commands': ['ln -s ../lib64/libvidstab.so.1.2 /app/lib/libvidstab.so.1.2']}))
    return manifest


def source_archive(target, files):
    """Write a byte-for-byte reproducible source archive."""
    with target.open('wb') as stream, gzip.GzipFile(fileobj=stream, mode='wb', filename='', mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as tar:
            for path, name in sorted(files, key=lambda entry: entry[1]):
                data = path.read_bytes()
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mode = 0o644
                tar.addfile(info, io.BytesIO(data))


def text_source_files():
    """Keep the Text source archive buildable with tests and the Qt compiler."""
    text = ROOT.parent/'Text'
    files = [(text/name, name) for name in
             ('CMakeLists.txt', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README_RU.md', 'plugin.json', 'studio.py')]
    files += [(path, path.relative_to(text).as_posix()) for path in sorted((text/'web').rglob('*'))
              if path.is_file() and '__pycache__' not in path.parts]
    files += [(text/'docs'/name, 'docs/'+name) for name in ('CATALOG_RU.md', 'LIFE_CATALOG_RU.md')]
    for folder in ('src', 'kdenlive', 'qt', 'tests', 'examples', 'scripts', 'tools', 'licenses'):
        files += [(path, path.relative_to(text).as_posix()) for path in sorted((text/folder).rglob('*'))
                  if path.is_file() and '__pycache__' not in path.parts]
    return files


def main(baseline=False, output=None):
    manifest = baseline_manifest()
    module = next(m for m in manifest['modules'] if m['name'] == 'kdenlive')
    out = Path(output) if output else ROOT/'build/package'
    if ROOT.resolve().is_relative_to(out.resolve()) or out.resolve().is_relative_to((ROOT/'overlay').resolve()):
        raise ValueError('Use a separate generated output directory, not the source tree itself')
    out.mkdir(parents=True, exist_ok=True)
    patch = '0001-Fix-CMake-build-error-with-latest-CMake-4.0-release.patch'
    shutil.copy2(ROOT/'upstream'/patch, out/patch)
    if baseline:
        (out/'baseline.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n', encoding='utf-8', newline='\n')
        return
    # The native head analyser reuses OpenCV already present in the upstream
    # manifest. Add only video decoding; no Python environment or web server.
    opencv = next(m for m in manifest['modules'] if m['name'] == 'opencv')
    opencv['config-opts'] = [option.replace('BUILD_LIST=tracking,dnn', 'BUILD_LIST=tracking,dnn,videoio')
                             for option in opencv['config-opts']]
    opencv['config-opts'] += ['-DWITH_FFMPEG=OFF', '-DWITH_GSTREAMER=ON', '-DCMAKE_INSTALL_LIBDIR=lib']
    # Only ship source and verified small previews, never Windows binaries/caches.
    card = ROOT.parent/'Card_3D'
    files = [(card/'CMakeLists.txt', 'CMakeLists.txt')]
    files += [(card/'docs/parameters.json', 'docs/parameters.json')]
    files += [(card/'docs/engine_parameters.json', 'docs/engine_parameters.json')]
    files += [(p, p.relative_to(card).as_posix()) for p in sorted((card/'src').rglob('*')) if p.is_file()]
    for folder in ('scripts', 'tests'):
        files += [(p, p.relative_to(card).as_posix()) for p in sorted((card/folder).rglob('*'))
                  if p.is_file() and '__pycache__' not in p.parts]
    files += [(p, p.name) for p in card.glob('LICENSE*') if p.is_file()]
    files += [(card/'kdenlive'/name, 'kdenlive/'+name) for name in ['card3d.xml', 'studio.json']]
    previews = json.loads((ROOT/'previews/previews.json').read_text(encoding='utf-8'))
    assert len(previews['previews']) == 11
    camera_previews = json.loads((ROOT/'previews/camera_previews.json').read_text(encoding='utf-8'))
    assert len(camera_previews['previews']) == 9
    for item in previews['previews'] + camera_previews['previews']:
        path = ROOT/'previews'/(item['name']+'.gif')
        assert sha(path) == item['sha256'], f'Preview changed: {path}'
    files += [(p, 'previews/'+p.name) for p in sorted((ROOT/'previews').iterdir())
              if p.suffix in ('.gif', '.png', '.json') and not p.name.startswith('transition_')]
    archive = out/'card3d-source.tar.gz'
    source_archive(archive, files)
    camera = ROOT.parent/'Camera'
    camera_files = [(camera/'CMakeLists.txt', 'CMakeLists.txt')]
    camera_files += [(camera/'demo/camera_test.png', 'demo/camera_test.png')]
    for folder in ('src', 'mlt', 'kdenlive', 'scripts', 'tests'):
        camera_files += [(p, p.relative_to(camera).as_posix()) for p in sorted((camera/folder).rglob('*'))
                         if p.is_file() and '__pycache__' not in p.parts]
    camera_files += [(p, p.name) for p in camera.glob('LICENSE*') if p.is_file()]
    camera_archive = out/'studio-camera-source.tar.gz'
    source_archive(camera_archive, camera_files)
    effects = ROOT.parent/'Effects'
    effect_files = [(effects/'CMakeLists.txt', 'CMakeLists.txt'), (effects/'README_RU.md', 'README_RU.md')]
    for folder in ('src', 'include', 'kdenlive', 'previews', 'third_party', 'scripts', 'tests'):
        effect_files += [(p, p.relative_to(effects).as_posix()) for p in sorted((effects/folder).rglob('*'))
                         if p.is_file() and '__pycache__' not in p.parts]
    effect_files += [(p, p.name) for p in effects.glob('LICENSE*') if p.is_file()]
    effects_archive = out/'studiofx-source.tar.gz'
    source_archive(effects_archive, effect_files)
    transitions = ROOT.parent/'Transitions'
    transition_files = [(transitions/'CMakeLists.txt', 'CMakeLists.txt')]
    for folder in ('src', 'scripts', 'tests', 'kdenlive', 'sfx'):
        transition_files += [(p, p.relative_to(transitions).as_posix()) for p in sorted((transitions/folder).rglob('*'))
                             if p.is_file() and '__pycache__' not in p.parts]
    transition_files += [(p, p.name) for p in transitions.glob('LICENSE*') if p.is_file()]
    transition_files += [(p, p.name) for p in transitions.glob('NOTICE*') if p.is_file()]
    transition_previews = json.loads((ROOT/'previews/transition_previews.json').read_text(encoding='utf-8'))
    assert len(transition_previews['items']) == 15
    for item in transition_previews['items']:
        for kind in ('png', 'gif'):
            path = ROOT/'previews'/item[kind]
            assert sha(path) == item[kind + '_sha256'], f'Preview changed: {path}'
            transition_files.append((path, 'previews/' + path.name))
    transition_files.append((ROOT/'previews/transition_previews.json', 'previews/transition_previews.json'))
    transitions_archive = out/'studio-transitions-source.tar.gz'
    source_archive(transitions_archive, transition_files)
    color = ROOT.parent/'Color'
    color_files = [(color/'CMakeLists.txt', 'CMakeLists.txt')]
    for folder in ('src', 'include', 'mlt', 'kdenlive', 'tests', 'scripts', 'presets'):
        color_files += [(p, p.relative_to(color).as_posix()) for p in sorted((color/folder).rglob('*'))
                       if p.is_file() and '__pycache__' not in p.parts]
    color_files += [(color/'SOURCES.json', 'SOURCES.json')]
    color_files += [(p, p.name) for p in color.glob('LICENSE*') if p.is_file()]
    color_files += [(p, p.name) for p in color.glob('NOTICE*') if p.is_file()]
    color_archive = out/'studio-color-source.tar.gz'
    source_archive(color_archive, color_files)
    audio = ROOT.parent/'Audio'
    audio_files = [(audio/name, name) for name in ('CMakeLists.txt', 'LICENSE', 'README_RU.md', 'SOURCES.json', 'THIRD_PARTY_NOTICES.md')]
    for folder in ('src', 'include', 'generated', 'scripts', 'tools', 'tests'):
        audio_files += [(p, p.relative_to(audio).as_posix()) for p in sorted((audio/folder).rglob('*'))
                        if p.is_file() and '__pycache__' not in p.parts]
    audio_archive = out/'studio-audio-source.tar.gz'
    source_archive(audio_archive, audio_files)
    subtitles = ROOT.parent/'Subtitles'
    subtitle_files = [(subtitles/name, name) for name in ('CMakeLists.txt', 'LICENSE', 'README_RU.md', 'SOURCES.json')]
    for folder in ('src', 'include', 'generated', 'scripts', 'tools', 'tests', 'licenses'):
        subtitle_files += [(p, p.relative_to(subtitles).as_posix()) for p in sorted((subtitles/folder).rglob('*'))
                          if p.is_file() and '__pycache__' not in p.parts]
    subtitle_files += [(path, 'Audio/' + name) for path, name in audio_files]
    subtitles_archive = out/'studio-subtitles-source.tar.gz'
    source_archive(subtitles_archive, subtitle_files)
    subtitle_sources = json.loads((subtitles/'SOURCES.json').read_text(encoding='utf-8'))
    text_archive = out/'studio-text-source.tar.gz'
    source_archive(text_archive, text_source_files())
    background = ROOT.parent/'Background'
    background_files = [(background/name, name) for name in
                        ('CMakeLists.txt', 'LICENSE', 'README_RU.md', 'SOURCES.json', 'THIRD_PARTY_NOTICES.md')]
    for folder in ('src', 'include', 'mlt', 'bin', 'kdenlive', 'previews', 'models', 'licenses', 'patches', 'scripts', 'tests'):
        background_files += [(p, p.relative_to(background).as_posix()) for p in sorted((background/folder).rglob('*'))
                             if p.is_file() and '__pycache__' not in p.parts]
    background_archive = out/'studio-background-source.tar.gz'
    source_archive(background_archive, background_files)
    background_sources = json.loads((background/'SOURCES.json').read_text(encoding='utf-8'))
    assert background_sources['onnx_runtime']['version'] == '1.21.0'
    assert background_sources['model']['sha256'] == '2047b95bea344af15575f4279d6fac44bb07a42e7d537cf8d3265f15fe95cc82'
    ort_patch = background/'patches/onnxruntime-1.21.0-gcc15-cstdint.patch'
    shutil.copy2(ort_patch, out/ort_patch.name)
    manifest['app-id'] = APP_ID
    manifest['default-branch'] = 'experimental'
    manifest['rename-desktop-file'] = 'org.kde.kdenlive.desktop'
    manifest['rename-appdata-file'] = 'org.kde.kdenlive.appdata.xml'
    # The original sandbox access is retained exactly; isolation comes from the ID.
    assert manifest['finish-args'] == json.loads((ROOT/'upstream/org.kde.kdenlive.json').read_text(encoding='utf-8'))['finish-args']
    kdenlive_index = manifest['modules'].index(module)
    manifest['modules'].insert(kdenlive_index, dict(name='card3d', buildsystem='cmake-ninja', builddir=True,
        sources=[dict(type='archive', path=archive.name, sha256=sha(archive), **{'strip-components': 0})],
        **{'config-opts': ['-DCMAKE_BUILD_TYPE=Release', '-DCARD3D_STUDIO_RESOURCES=ON']}))
    manifest['modules'].insert(kdenlive_index + 1, dict(name='studio-camera', buildsystem='cmake-ninja', builddir=True,
        sources=[dict(type='archive', path=camera_archive.name, sha256=sha(camera_archive), **{'strip-components': 0})],
        **{'config-opts': ['-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_LIBDIR=lib']}))
    manifest['modules'].insert(kdenlive_index + 2, dict(name='studiofx', buildsystem='cmake-ninja', builddir=True,
        sources=[dict(type='archive', path=effects_archive.name, sha256=sha(effects_archive), **{'strip-components': 0})],
        **{'config-opts': ['-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_LIBDIR=lib', '-DFREI0R_INCLUDE_DIR=/app/include']}))
    manifest['modules'].insert(kdenlive_index + 3, dict(name='studio-transitions', buildsystem='cmake-ninja', builddir=True,
        sources=[dict(type='archive', path=transitions_archive.name, sha256=sha(transitions_archive), **{'strip-components': 0})],
        **{'config-opts': ['-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_LIBDIR=lib', '-DTRANSITIONS_SYSTEM_FREI0R=ON']}))
    manifest['modules'].insert(kdenlive_index + 4, dict(name='studio-color', buildsystem='cmake-ninja', builddir=True,
        sources=[dict(type='archive', path=color_archive.name, sha256=sha(color_archive), **{'strip-components': 0})],
        **{'config-opts': ['-DCMAKE_BUILD_TYPE=Release', '-DSTUDIO_COLOR_MLT=ON', '-DSTUDIO_COLOR_DEVEL=OFF', '-DCMAKE_INSTALL_LIBDIR=lib']}))
    manifest['modules'].insert(kdenlive_index + 5, dict(name='studio-audio', buildsystem='cmake-ninja', builddir=True,
        sources=[dict(type='archive', path=audio_archive.name, sha256=sha(audio_archive), **{'strip-components': 0})],
        **{'config-opts': ['-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF']}))
    whisper_timing_patch = out/'whisper-vad-json-timestamps.patch'
    shutil.copyfile(ROOT/'patches'/whisper_timing_patch.name, whisper_timing_patch)
    manifest['modules'].insert(kdenlive_index + 6, dict(name='whisper-cpp', buildsystem='simple',
        **{'build-commands': [
            'patch --batch --forward --fuzz=0 -p1 < whisper-vad-json-timestamps.patch',
            'cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF '
            '-DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=ON -DWHISPER_BUILD_SERVER=OFF '
            '-DGGML_NATIVE=OFF -DGGML_OPENMP=OFF',
            'cmake --build build --target whisper-cli --parallel 2',
            'install -Dm755 build/bin/whisper-cli /app/bin/whisper-cli',
            'install -Dm644 LICENSE /app/share/licenses/whisper-cpp/LICENSE'
        ]}, sources=[dict(type='git', url=subtitle_sources['whisper_cpp']['source'], commit=subtitle_sources['whisper_cpp']['commit']),
                     dict(type='file', path=whisper_timing_patch.name, sha256=sha(whisper_timing_patch))]))
    manifest['modules'].insert(kdenlive_index + 7, dict(name='studio-subtitles', buildsystem='cmake-ninja', builddir=True,
        sources=[dict(type='archive', path=subtitles_archive.name, sha256=sha(subtitles_archive), **{'strip-components': 0}),
                 dict(type='file', url=subtitle_sources['model']['source'], sha256=subtitle_sources['model']['sha256'],
                      **{'dest-filename': subtitle_sources['model']['name']}),
                 dict(type='file', url=subtitle_sources['vad_model']['source'], sha256=subtitle_sources['vad_model']['sha256'],
                      **{'dest-filename': subtitle_sources['vad_model']['name']})],
        **{'config-opts': ['-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF',
                           '-DSTUDIO_SUBTITLE_MODEL_FILE=/run/build/studio-subtitles/' + subtitle_sources['model']['name'],
                           '-DSTUDIO_SUBTITLE_VAD_MODEL_FILE=/run/build/studio-subtitles/' + subtitle_sources['vad_model']['name']]}))
    manifest['modules'].insert(kdenlive_index + 8, dict(name='studio-text', buildsystem='cmake-ninja', builddir=True,
        sources=[dict(type='archive', path=text_archive.name, sha256=sha(text_archive), **{'strip-components': 0})],
        **{'config-opts': ['-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF']}))
    manifest['modules'].insert(kdenlive_index + 8, dict(name='onnxruntime-cpu', buildsystem='simple',
        **{'build-options': {'env': {'MAX_JOBS': '2'}, 'build-args': ['--share=network']}},
        **{'build-commands': [
            './build.sh --config Release --build_shared_lib --parallel 2 --skip_tests --compile_no_warning_as_error '
            '--use_preinstalled_eigen --eigen_path /run/build/onnxruntime-cpu/ort-eigen --cmake_extra_defines '
            'FETCHCONTENT_SOURCE_DIR_EIGEN=/run/build/onnxruntime-cpu/ort-eigen '
            'FETCHCONTENT_SOURCE_DIR_PROTOBUF=/run/build/onnxruntime-cpu/ort-protobuf '
            'CMAKE_INSTALL_PREFIX=/app CMAKE_INSTALL_LIBDIR=lib onnxruntime_BUILD_UNIT_TESTS=OFF '
            'onnxruntime_ENABLE_PYTHON=OFF onnxruntime_USE_CUDA=OFF onnxruntime_USE_ROCM=OFF',
            'cmake --install build/Linux/Release'
        ]}, sources=[dict(type='git', url='https://github.com/microsoft/onnxruntime.git',
                         commit=background_sources['onnx_runtime']['commit']),
                     dict(type='patch', path=ort_patch.name),
                     dict(type='archive', url=background_sources['onnx_runtime']['eigen']['source'],
                          sha256=background_sources['onnx_runtime']['eigen']['sha256'], dest='ort-eigen',
                          **{'strip-components': 1}),
                     dict(type='archive', url=background_sources['onnx_runtime']['protobuf']['source'],
                          sha256=background_sources['onnx_runtime']['protobuf']['sha256'], dest='ort-protobuf',
                          **{'strip-components': 1})]))
    manifest['modules'].insert(kdenlive_index + 9, dict(name='studio-background', buildsystem='cmake-ninja', builddir=True,
        sources=[dict(type='archive', path=background_archive.name, sha256=sha(background_archive), **{'strip-components': 0}),
                 dict(type='file', path=os.environ.get('STUDIO_HUMANSEG_ONNX', str(out/background_sources['model']['name'])), sha256=background_sources['model']['sha256'],
                      **{'dest-filename': background_sources['model']['name']})],
        **{'config-opts': ['-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_LIBDIR=lib', '-DSBG_WITH_MLT=ON', '-DSBG_WITH_ONNX=ON',
                           '-DONNXRUNTIME_ROOT=/app', '-DSBG_MODEL_FILE=/run/build/studio-background/' + background_sources['model']['name']]}))
    # Model tests run inside the Kdenlive module and already need FFmpeg's SONAME.
    compatibility = manifest['modules'].pop()
    assert compatibility['name'] == 'flatpak-lib-compat'
    manifest['modules'].insert(manifest['modules'].index(module), compatibility)
    # Build the unchanged external dependency before our frequently edited engines.
    onnx = next(item for item in manifest['modules'] if item['name'] == 'onnxruntime-cpu')
    manifest['modules'].remove(onnx)
    manifest['modules'].insert(kdenlive_index, onnx)
    if (out/'overlay').exists():
        shutil.rmtree(out/'overlay')
    shutil.copytree(ROOT/'overlay', out/'overlay')
    shutil.copy2(audio/'generated/parameters.hpp', out/'overlay/audio/parameters.hpp')
    (out/'overlay/text').mkdir(exist_ok=True)
    shutil.copy2(ROOT.parent/'Text/qt/scenecompiler.hpp', out/'overlay/text/scenecompiler.hpp')
    shutil.copy2(ROOT.parent/'Text/qt/styles.hpp', out/'overlay/text/styles.hpp')
    shutil.copy2(ROOT/'scripts/integrate.py', out/'studio-integrate.py')
    shutil.copy2(ROOT/'tests/studioregressiontest.cpp', out/'studioregressiontest.cpp')
    shutil.copy2(ROOT/'tests/public_demo.hpp', out/'public_demo.hpp')
    shutil.copy2(ROOT/'branding/factmontage.svg', out/'factmontage.svg')
    module['sources'] += [dict(type='dir', path='overlay', dest='src/assets/studio'),
                          dict(type='file', path='studio-integrate.py'),
                          dict(type='file', path='studioregressiontest.cpp', dest='tests'),
                          dict(type='file', path='public_demo.hpp', dest='tests'),
                          dict(type='file', path='factmontage.svg'),
                          dict(type='shell', commands=['python3 studio-integrate.py'])]
    module['run-tests'] = True
    module['test-rule'] = ''
    module['test-commands'] = [
        'test -s /app/share/kdenlive/transitions/slide.xml && '
        'test -s /app/share/kdenlive/transitions/wipe.xml && '
        'test -s /app/share/kdenlive/studio/studio.json && '
        'test -s /app/share/kdenlive/studio/studio_camera.json && '
        'test -e /app/lib/libvidstab.so.1.2',
        'MLT_REPOSITORY=/app/lib/mlt-7 melt -query transitions >/tmp/studio-build-transitions.txt '
        '2>/tmp/studio-build-mlt-errors.txt && '
        'test ! -s /tmp/studio-build-mlt-errors.txt && '
        'grep -Fq luma /tmp/studio-build-transitions.txt',
        'FREI0R_PATH=/app/lib/frei0r-1 MLT_REPOSITORY=/app/lib/mlt-7 STUDIO_UI_OUTPUT="$PWD/studio-ui" QT_QPA_PLATFORM=offscreen '
        'ctest --output-on-failure --no-tests=error --output-junit studio-model-tests.xml '
        "-R 'studioregressiontest|effectstest|effectsgrouptest|mixtest|trimmingtest|subtitlestest|documenttest|sequencetest'",
        'FREI0R_PATH=/app/lib/frei0r-1 MLT_REPOSITORY=/app/lib/mlt-7 STUDIO_UI_OUTPUT="$PWD/studio-ui" '
        'QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1.25 bin/studioregressiontest \'[StudioUI]~[.persistent]\' --warn NoTests',
        'FREI0R_PATH=/app/lib/frei0r-1 MLT_REPOSITORY=/app/lib/mlt-7 STUDIO_UI_OUTPUT="$PWD/studio-ui" '
        'QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1.5 bin/studioregressiontest \'[StudioUI]~[.persistent]\' --warn NoTests',
    ]
    # Existing upstream patch is relative to the manifest location.
    module['post-install'] += [
        "sed -i 's/^Name=.*/Name=FactMontage/; /^Name\\[/d' /app/share/applications/org.kde.kdenlive.desktop",
        "find /app/share/icons/hicolor -type f -path '*/apps/kdenlive.png' -delete",
        'install -Dm644 /run/build/kdenlive/factmontage.svg /app/share/icons/hicolor/scalable/apps/kdenlive.svg'
    ]
    # Keep the extension mount point in the exported app, including resumed builds.
    manifest['modules'].append(dict(name='studio-extension-anchor', buildsystem='simple',
        **{'build-commands': ['touch /app/extensions/Plugins/.keep']}))
    (out/(APP_ID+'.json')).write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n', encoding='utf-8', newline='\n')
    changes = [(ROOT.parent/name, name) for name in ('README.md', 'README_RU.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', '.gitattributes', '.gitignore', 'dev',
               'Studio/README_RU.md', 'Studio/modules.json', 'docs/BUILD.md', 'docs/BUILD_RU.md',
               'docs/INSTALL.md', 'docs/INSTALL_RU.md',
               'Studio/upstream/sources.json', 'Studio/upstream/org.kde.kdenlive.json', 'Studio/upstream/'+patch)]
    for folder in ('overlay', 'scripts', 'tests', 'previews', 'patches', 'branding'):
        changes += [(p, p.relative_to(ROOT.parent).as_posix()) for p in sorted((ROOT/folder).rglob('*'))
                    if p.is_file() and '__pycache__' not in p.parts]
    source_archive(out/'studio-changes-source.tar.gz', changes)
    print(f'Prepared {out}; no Flatpak has been built by this command')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    main(baseline=args.baseline, output=args.output)
