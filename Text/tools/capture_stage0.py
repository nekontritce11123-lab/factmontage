#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Capture the existing Qt -> STXT -> native renderer boundary, not Deck acceptance.

No builds, downloads, project edits, installation or Beads mutation. The parent
bounds each compiler/native worker process and preserves failures in evidence.json.
"""
import argparse
import ctypes as C
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys
import time
import unicodedata

TIMES = (2., 3., 4., 5., 6.)
SIZES = ((1920, 1080), (480, 270))
CONFIG = {'duration':0, 'inDuration':1, 'outDuration':2, 'inPreset':6,
          'outPreset':7, 'grouping':8, 'lifeAmount':11, 'lifeSpeed':12,
          'lifeSource':18, 'fpsNum':23, 'fpsDen':24, 'size':28}


def inspect_scene(data):
    """Read the current Qt compiler's v2 layout; never guess other versions."""
    if len(data) < 32:
        raise ValueError('truncated STXT header')
    magic, version, meta_size, count, width, height, cfg_count = struct.unpack_from('<8s6I', data)
    if magic != b'SUNMTX1\0':
        raise ValueError('invalid STXT magic')
    if version != 2:
        raise ValueError('unsupported diagnostic STXT version (expected Qt v2)')
    if cfg_count != 32:
        raise ValueError('unexpected STXT config length')
    if not (1 <= count <= 16384 and 64 <= width <= 4096 and 64 <= height <= 4096):
        raise ValueError('invalid STXT dimensions or sprite count')
    if meta_size > 2*1024*1024 or len(data) < 160+meta_size:
        raise ValueError('truncated or oversized STXT metadata/config')
    cfg = struct.unpack_from('<32f', data, 32)
    if not all(math.isfinite(v) for v in cfg):
        raise ValueError('STXT config must be finite')
    metadata = json.loads(data[160:160+meta_size])
    if not isinstance(metadata, dict):
        raise ValueError('STXT metadata must be an object')
    offset = 160+meta_size
    for _ in range(count):
        if len(data) < offset+48:
            raise ValueError('truncated sprite header')
        sprite = struct.unpack_from('<6f6I', data, offset)
        w, h, size = sprite[6], sprite[7], sprite[11]
        if not w or not h or size != w*h*4:
            raise ValueError('invalid sprite texture dimensions')
        offset += 48+size
        if offset > len(data):
            raise ValueError('truncated sprite payload')
    if offset != len(data):
        raise ValueError('unexpected trailing STXT bytes')
    return {'version':version, 'reference':[width,height], 'sprite_count':count,
            'config':{k:cfg[i] for k,i in CONFIG.items()}, 'metadata':metadata}


def setting_mismatches(actual, expected):
    def same(a, b):
        if isinstance(a, str) and isinstance(b, str):
            return unicodedata.normalize('NFC', a) == unicodedata.normalize('NFC', b)
        if isinstance(a, (int,float)) and isinstance(b, (int,float)):
            return math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-7)
        return a == b
    return [key for key,value in expected.items()
            if not same(actual['metadata'].get(key), value)
            or (key in CONFIG and not same(actual['config'][key], value))]


def control_cases(catalog):
    recipes = [('none',0,'Без движения'), ('hover',2,'Парение после подъёма'),
               ('wave',46,'Лента на воздухе'), ('tracking',10,'Дыхание интервала')]
    common = {'text':'Ёжик смотрит на ёлку\nВторая строка движется',
              'font':'DejaVu Sans', 'size':96, 'bold':True, 'color':'#fff9ef',
              'width':84, 'x':50, 'y':50, 'duration':8, 'inDuration':1,
              'outDuration':1, 'inPreset':2, 'outPreset':-1, 'fpsNum':60,
              'fpsDen':1, 'lifeSpeed':1}
    cases = []
    for slug, recipe_id, name in recipes:
        if recipe_id:
            matches = [r for r in catalog if r.get('life',{}).get('name') == name]
            if len(matches) != 1 or matches[0].get('id') != recipe_id:
                raise ValueError(f'missing, ambiguous or renumbered recipe: {name}')
        settings = dict(common, lifeSource=recipe_id, lifeAmount=.35 if recipe_id else 0)
        cases.append({'name':name, 'slug':slug, 'settings':settings})
    return cases


def run_logged(command, log, timeout):
    record = {'command':command, 'timeout_seconds':timeout, 'log':log.name}
    start = time.monotonic()
    with log.open('w', encoding='utf-8') as output:
        try:
            process = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, timeout=timeout, check=False)
            record.update(status='PASS' if process.returncode == 0 else 'FAIL',
                          returncode=process.returncode)
        except subprocess.TimeoutExpired:
            output.write('\nTIMEOUT: process terminated; this is not PASS.\n')
            record.update(status='TIMEOUT', returncode=None)
        except OSError as error:
            output.write(str(error)+'\n')
            record.update(status='NOT RUN', returncode=None)
    record['elapsed_seconds'] = round(time.monotonic()-start, 3)
    return record


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False)+'\n', encoding='utf-8')


def native_worker(scene_path, library_path, output):
    """Runs out of process: a broken native library cannot hang the collector."""
    from PIL import Image
    lib = C.CDLL(str(library_path))
    definitions = {
        'smt_create':([],C.c_void_p), 'smt_destroy':([C.c_void_p],None),
        'smt_load':([C.c_void_p,C.c_void_p,C.c_size_t],C.c_int),
        'smt_render':([C.c_void_p,C.c_double,C.c_uint,C.c_uint,C.c_void_p,C.c_void_p],C.c_int),
        'smt_last_error':([C.c_void_p],C.c_char_p), 'f0r_init':([],C.c_int),
        'f0r_deinit':([],None), 'f0r_construct':([C.c_uint,C.c_uint],C.c_void_p),
        'f0r_destruct':([C.c_void_p],None),
        'f0r_set_param_value':([C.c_void_p,C.c_void_p,C.c_int],None),
        'f0r_update':([C.c_void_p,C.c_double,C.c_void_p,C.c_void_p],None)}
    for name,(args,result) in definitions.items():
        function = getattr(lib,name)
        function.argtypes, function.restype = args,result
    if lib.f0r_init() != 1:
        raise RuntimeError('frei0r initialization failed')
    instance = lib.smt_create()
    try:
        if not instance:
            raise RuntimeError('smt_create failed')
        data = scene_path.read_bytes()
        source = C.create_string_buffer(data)
        if lib.smt_load(instance,source,len(data)) != 1:
            raise RuntimeError(lib.smt_last_error(instance))
        frames = []
        for width,height in SIZES:
            host = lib.f0r_construct(width,height)
            try:
                if not host:
                    raise RuntimeError('frei0r construction failed')
                path = C.c_char_p(os.fsencode(scene_path))
                lib.f0r_set_param_value(host,C.byref(path),0)
                rgba = C.create_string_buffer(width*height*4)
                host_rgba = C.create_string_buffer(width*height*4)
                hashes = {}
                for seconds in (*TIMES,3.):
                    if lib.smt_render(instance,seconds,width,height,None,rgba) != 1:
                        raise RuntimeError(lib.smt_last_error(instance))
                    lib.f0r_update(host,seconds,None,host_rgba)
                    if lib.smt_last_error(host):
                        raise RuntimeError(lib.smt_last_error(host))
                    pixels = rgba.raw
                    if pixels != host_rgba.raw:
                        raise RuntimeError(f'C API/frei0r mismatch at {seconds}s {width}x{height}')
                    digest = sha256(pixels)
                    if seconds in hashes:
                        if hashes[seconds] != digest:
                            raise RuntimeError('nondeterministic seek')
                        continue
                    hashes[seconds] = digest
                    image = Image.frombytes('RGBA',(width,height),pixels)
                    bbox = image.getchannel('A').getbbox()
                    if bbox is None:
                        raise RuntimeError('empty/fully transparent control frame')
                    filename = f'{width}x{height}-t{int(seconds)}.png'
                    image.save(output/filename)
                    frames.append({'size':[width,height], 'caller_time_seconds':seconds,
                                   'rgba_sha256':digest, 'png_sha256':sha256((output/filename).read_bytes()),
                                   'alpha_bbox':list(bbox), 'file':filename})
            finally:
                if host:
                    lib.f0r_destruct(host)
        write_json(output/'frames.json', {'frames':frames, 'repeat_seek':'PASS',
                   'c_api_matches_frei0r':'PASS', 'mlt_clock':'NOT RUN'})
    finally:
        if instance:
            lib.smt_destroy(instance)
        lib.f0r_deinit()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',type=Path,required=True)
    parser.add_argument('--library',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args(argv)
    output = args.output.resolve()
    output.mkdir(parents=True,exist_ok=False)  # Never overwrite previous evidence.
    report = {'schema':1, 'scope':'synthetic Qt compiler + C API/frei0r baseline; not Kdenlive or Deck acceptance',
              'collector_sha256':sha256(Path(__file__).read_bytes()),
              'python':platform.python_version(), 'platform':platform.platform(),
              'source_revision':None, 'capture_status':'NOT RUN', 'cases':[],
              'not_run':['preserve dirty Deck worktree', 'identify installed SUNIMO binaries',
                         'copy and trace user project in the panel', 'observe MLT-supplied time',
                         'record results in the existing Beads database'],
              'stage_0_gate':'NOT RUN'}
    try:
        root = Path(__file__).resolve().parents[2]
        report['source_revision'] = subprocess.check_output(
            ['git','-C',str(root),'rev-parse','HEAD'],timeout=5,text=True).strip()
        compiler,library = args.compiler.resolve(),args.library.resolve()
        for name,path in (('compiler',compiler),('library',library)):
            if not path.is_file():
                raise FileNotFoundError(f'{name} unavailable: {path}; prepare the build explicitly')
            report[name] = {'path':str(path), 'sha256':sha256(path.read_bytes())}
        catalog = json.loads((root/'Text/web/catalog.json').read_text(encoding='utf-8'))
        os.environ.setdefault('QT_QPA_PLATFORM','offscreen')
        for case in control_cases(catalog):
            folder = output/case['slug']
            folder.mkdir()
            record = dict(case)
            record['status'] = 'NOT RUN'
            report['cases'].append(record)
            settings = folder/'settings.json'
            scene = folder/'scene.stxt'
            write_json(settings,case['settings'])
            compile_result = run_logged([str(compiler),str(settings),str(scene),'1920','1080'],folder/'compile.log',30)
            record['compile'] = compile_result
            if compile_result['status'] != 'PASS':
                record['status'] = compile_result['status']
                continue
            try:
                record['scene'] = inspect_scene(scene.read_bytes())
                record['scene_sha256'] = sha256(scene.read_bytes())
                record['setting_mismatches'] = setting_mismatches(record['scene'],case['settings'])
                if record['setting_mismatches']:
                    raise ValueError('requested settings differ from saved metadata/binary config')
                render_result = run_logged([sys.executable,str(Path(__file__).resolve()),'--worker',
                    str(scene),str(library),str(folder)],folder/'render.log',90)
                record['render'] = render_result
                record['status'] = render_result['status']
                if render_result['status'] == 'PASS':
                    record['rendered'] = json.loads((folder/'frames.json').read_text(encoding='utf-8'))
                    record['motion_observed'] = {f'{w}x{h}': len({f['rgba_sha256']
                        for f in record['rendered']['frames'] if f['size'] == [w,h]}) > 1 for w,h in SIZES}
                    if case['slug'] == 'none' and any(record['motion_observed'].values()):
                        raise ValueError('no-motion control changes during the hold phase')
            except (OSError,ValueError) as error:
                record.update(status='FAIL',error=str(error))
            write_json(output/'evidence.json',report)
        report['capture_status'] = 'PASS' if len(report['cases']) == 4 and all(
            c['status'] == 'PASS' for c in report['cases']) else 'FAIL'
    except (OSError,ValueError,subprocess.SubprocessError) as error:
        report['error'] = str(error)
    finally:
        write_json(output/'evidence.json',report)
    print(f"Native capture: {report['capture_status']}; full stage 0: NOT RUN (see evidence.json)")
    return 0 if report['capture_status'] == 'PASS' else 1


if __name__ == '__main__':
    if len(sys.argv) == 5 and sys.argv[1] == '--worker':
        native_worker(*(Path(p).resolve() for p in sys.argv[2:]))
    else:
        raise SystemExit(main())
