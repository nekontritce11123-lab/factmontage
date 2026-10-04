#!/usr/bin/env python3
"""Bounded, offline module checks; packaging remains an explicit release operation.

Only bootstrap and release may invoke dependency preparation. Native FAST tests
are not Kdenlive/MLT/GUI product acceptance. Never turn a missing dependency into PASS.
"""
from __future__ import annotations
import argparse
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
REGISTRY = ROOT / 'Studio/modules.json'


class DevError(RuntimeError):
    pass


def load_modules(path: Path = REGISTRY) -> dict:
    data = json.loads(path.read_text(encoding='utf-8'))
    if data.get('schema_version') != 1 or not isinstance(data.get('modules'), dict):
        raise DevError('Unsupported module registry schema')
    modules = data['modules']
    paths = set()
    for name, module in modules.items():
        source = module.get('path', '')
        if not name.isidentifier() or not source or PurePosixPath(source).is_absolute() or '..' in PurePosixPath(source).parts:
            raise DevError(f'Invalid module: {name!r}')
        if source in paths:
            raise DevError(f'Duplicate module path: {source}')
        paths.add(source)
        for dependency in module.get('depends_on', []):
            if dependency not in modules or dependency == name:
                raise DevError(f'Invalid dependency of {name}: {dependency}')
        for entry in [module.get('generator', ''), *module.get('contracts', [])]:
            if entry and (PurePosixPath(entry).is_absolute() or '..' in PurePosixPath(entry).parts):
                raise DevError(f'Unsafe test path: {entry}')
    return modules


def change_scopes(paths: list[str], modules: dict) -> tuple[list[str], bool, bool]:
    """Select native engines, host integration and runner checks separately."""
    selected = set()
    host = tooling = False
    for path in paths:
        path = path.replace('\\', '/')
        if path.startswith(('.beads/', 'docs/', 'archive/', 'releases/')) or (path.endswith(('.md', '.txt')) and path.rsplit('/', 1)[-1] != 'CMakeLists.txt'):
            continue
        matches = [name for name, module in modules.items() if path.startswith(module['path'] + '/')]
        if matches:
            selected.update(matches)
            if path in ('Text/qt/scenecompiler.hpp', 'Text/qt/styles.hpp'):
                host = True
        elif path == 'Studio/modules.json':
            selected.update(modules)
            tooling = True
        elif path.startswith('Studio/overlay/') or path in ('Studio/tests/studioregressiontest.cpp',
                                                           'Studio/tests/public_demo.hpp',
                                                           'Studio/scripts/integrate.py', 'Studio/scripts/build.sh'):
            host = True
        elif path.startswith(('Studio/scripts/', 'Studio/tests/test_')):
            tooling = True
        elif path.startswith('Studio/'):
            host = True
        elif path == 'dev':
            tooling = True
        else:
            # Unknown source remains conservative until assigned to a scope.
            selected.update(modules)
    # Reverse dependencies: changing Audio also invalidates Subtitles.
    while True:
        expanded = selected | {name for name, module in modules.items()
                               if any(dep in selected for dep in module.get('depends_on', []))}
        if expanded == selected:
            break
        selected = expanded
    return [name for name in modules if name in selected], host, tooling


def affected_modules(paths: list[str], modules: dict) -> list[str]:
    return change_scopes(paths, modules)[0]


def git_output(*args: str, cwd: Path = ROOT) -> bytes:
    try:
        return subprocess.check_output(['git', *args], cwd=cwd, stderr=subprocess.PIPE, timeout=15)
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode('utf-8', errors='replace').strip()
        raise DevError(f'Cannot determine changed files: {detail or exc}. Use an explicit module or all.') from exc
    except (subprocess.TimeoutExpired, FileNotFoundError) as exc:
        raise DevError(f'Cannot determine changed files: {exc}. Use an explicit module or all.') from exc


def changed_paths(base: str | None, cwd: Path = ROOT) -> list[str]:
    # No implicit HEAD~1: a clean checkout is not the previous commit's changes.
    # CI must provide the PR merge base / push before SHA explicitly.
    paths = set()
    if base:
        if base.startswith('-') or not base.strip():
            raise DevError('Invalid base ref')
        reference = git_output('rev-parse', '--verify', f'{base}^{{commit}}', cwd=cwd).decode().strip()
        paths.update(git_output('diff', '--name-only', '-z', '--no-renames', reference, 'HEAD', '--', cwd=cwd).decode().split('\0'))
    paths.update(git_output('diff', '--name-only', '-z', '--no-renames', 'HEAD', '--', cwd=cwd).decode().split('\0'))
    paths.update(git_output('ls-files', '--others', '--exclude-standard', '-z', cwd=cwd).decode().split('\0'))
    return sorted(paths - {''})


@dataclass
class CommandResult:
    command: list[str]
    cwd: str
    seconds: float
    returncode: int
    status: str
    log: str


class Runner:
    def __init__(self, directory: Path, timeout: float, dry_run: bool = False):
        self.directory, self.timeout, self.dry_run = directory, timeout, dry_run
        self.results: list[CommandResult] = []

    def run(self, command: list[str | Path], *, cwd: Path = ROOT, env: dict | None = None, timeout: float | None = None) -> None:
        args = [str(value) for value in command]
        print('+ ' + shlex.join(args), flush=True)
        if self.dry_run:
            return
        self.directory.mkdir(parents=True, exist_ok=True)
        logfile = self.directory / f'{len(self.results) + 1:03d}.log'
        start = time.monotonic()
        status = 'PASS'
        returncode = 0
        try:
            with logfile.open('w', encoding='utf-8') as log:
                process = subprocess.Popen(args, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT,
                                           start_new_session=(os.name == 'posix'))
                try:
                    returncode = process.wait(timeout=timeout or self.timeout)
                    status = 'PASS' if returncode == 0 else 'FAIL'
                except (subprocess.TimeoutExpired, KeyboardInterrupt) as exc:
                    status = 'TIMEOUT' if isinstance(exc, subprocess.TimeoutExpired) else 'INTERRUPTED'
                    # Kill the process group, not only cmake while compiler children keep running.
                    if os.name == 'posix':
                        try:
                            os.killpg(process.pid, signal.SIGTERM)
                        except ProcessLookupError:
                            pass
                    else:
                        process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        if os.name == 'posix':
                            try:
                                os.killpg(process.pid, signal.SIGKILL)
                            except ProcessLookupError:
                                pass
                        else:
                            process.kill()
                        process.wait()
                    returncode = 124 if status == 'TIMEOUT' else 130
        except OSError as exc:
            logfile.write_text(str(exc) + '\n', encoding='utf-8')
            status, returncode = 'ERROR', 127
        elapsed = round(time.monotonic() - start, 3)
        self.results.append(CommandResult(args, str(cwd), elapsed, returncode, status, str(logfile)))
        print(f'  {status} ({elapsed:.3f}s) — {logfile}', flush=True)
        if returncode:
            print(logfile.read_text(encoding='utf-8', errors='replace')[-16000:], file=sys.stderr)
            raise DevError(f'{status}: {args[0]} exited {returncode}. Log: {logfile}')


def common_environment() -> dict:
    return dict(os.environ, PYTHONUTF8='1', PYTHONDONTWRITEBYTECODE='1',
                QT_QPA_PLATFORM='offscreen', CTEST_OUTPUT_ON_FAILURE='1')


def build_module(name: str, module: dict, args: argparse.Namespace, runner: Runner) -> Path:
    source = ROOT / module['path']
    build = args.build_root / name
    env = common_environment()
    # RelWithDebInfo preserves production-like optimization. Native tests explicitly
    # undefine NDEBUG in CMake so assertions cannot silently disappear.
    configure = ['cmake', '-S', source, '-B', build, '-G', 'Ninja',
                 '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DBUILD_TESTING=ON', *module.get('cmake', [])]
    runner.run(configure, env=env)
    runner.run(['cmake', '--build', build, '--parallel', str(args.jobs)], env=env)
    return build


def test_module(name: str, module: dict, args: argparse.Namespace, runner: Runner) -> None:
    source = ROOT / module['path']
    env = common_environment()
    if module.get('generator'):
        runner.run([sys.executable, '-B', source / module['generator'], '--check'], env=env)
    for contract in module.get('contracts', []):
        runner.run([sys.executable, '-B', source / contract], env=env)
    if args.contracts_only:
        return
    build = build_module(name, module, args, runner)
    runner.run(['ctest', '--test-dir', build, '--output-on-failure', '--no-tests=error',
                '--timeout', str(int(args.timeout)), '--output-junit', str(build / 'tests.xml')], env=env)


def source_identity() -> dict:
    try:
        return {'sha': git_output('rev-parse', 'HEAD').decode().strip(),
                'dirty': bool(git_output('status', '--porcelain', '--', '.', ':(exclude).beads'))}
    except DevError:
        return {'sha': None, 'dirty': None}


def write_report(runner: Runner, selected: list[str], status: str, command: str,
                 source: dict | None = None, host: dict | None = None) -> None:
    if runner.dry_run:
        return
    runner.directory.mkdir(parents=True, exist_ok=True)
    report = {'schema_version': 1, 'command': command, 'status': status, 'selected_modules': selected,
              'source': source if source is not None else source_identity(), 'platform': sys.platform,
              'finished_utc': datetime.now(timezone.utc).isoformat(),
              'registry_sha256': hashlib.sha256(REGISTRY.read_bytes()).hexdigest(),
              'commands': [asdict(item) for item in runner.results],
              'not_certified': ['Kdenlive GUI', 'Save/Reopen', 'Export', 'Flatpak', 'Steam Deck performance']}
    if host is not None:
        report['host'] = host
    target = runner.directory / 'report.json'
    temporary = target.with_suffix('.tmp')
    temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(target)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for command in ('test', 'build'):
        child = sub.add_parser(command)
        child.add_argument('module', nargs='?', default='changed' if command == 'test' else 'all')
        child.add_argument('--base', help='Compare committed changes to this ref, plus the working tree')
        child.add_argument('--build-root', type=Path, default=Path(os.environ.get('STUDIO_DEV_BUILD_ROOT', ROOT / '.build/dev')))
        child.add_argument('--jobs', type=int, default=int(os.environ.get('STUDIO_BUILD_JOBS', '2')))
        child.add_argument('--timeout', type=float, default=180, help='Maximum seconds per command (process group killed on timeout)')
        child.add_argument('--dry-run', action='store_true')
        child.add_argument('--report-dir', type=Path)
        child.add_argument('--contracts-only', action='store_true', help='Python/contracts only; no native certification')
    sub.add_parser('list')
    bootstrap = sub.add_parser('bootstrap', help='Explicit download of pinned Kdenlive sources for integration')
    bootstrap.add_argument('--timeout', type=float, default=300)
    integration = sub.add_parser('integration', help='Sync, incrementally build and test the prepared Kdenlive host in the pinned SDK')
    integration.add_argument('--host-build', type=Path, required=True)
    integration.add_argument('--timeout', type=float, default=3600)
    integration.add_argument('--jobs', type=int, default=2)
    integration.add_argument('--test', default='studioregressiontest')
    integration.add_argument('--case', help='Run one named studioregressiontest case while debugging')
    integration.add_argument('--build-only', action='store_true', help='Compile fresh host targets without claiming model test PASS')
    integration.add_argument('--report-dir', type=Path)
    run = sub.add_parser('run', help='Run an explicitly prepared developer executable; does not install anything')
    run.add_argument('--executable', type=Path, required=True)
    release = sub.add_parser('release', help='Explicit full pinned build/check/package (may download dependencies)')
    release.add_argument('version')
    release.add_argument('--timeout', type=float, default=3600)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    modules = load_modules()
    if args.command == 'list':
        for name, module in modules.items():
            print(f'{name:12} {module["path"]}')
        return 0
    if args.command == 'run':
        executable = args.executable.expanduser().resolve()
        if not executable.is_file() or not os.access(executable, os.X_OK):
            raise DevError(f'Executable unavailable: {executable}. Build it explicitly first.')
        os.execv(str(executable), [str(executable)])
    timeout = getattr(args, 'timeout', 180)
    if timeout <= 0 or not __import__('math').isfinite(timeout):
        raise DevError('timeout must be finite and positive')
    if args.command == 'build' and args.contracts_only:
        raise DevError('--contracts-only is only available for test; build does not run tests')
    if hasattr(args, 'jobs') and not 1 <= args.jobs <= 64:
        raise DevError('jobs must be between 1 and 64')
    root = ROOT / '.build/reports'
    directory = getattr(args, 'report_dir', None) or root / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%f') + f'-{os.getpid()}')
    runner = Runner(directory.resolve(), timeout, getattr(args, 'dry_run', False))
    selected = []
    status = 'NOT RUN'
    host_evidence = None
    report_source = None
    try:
        if args.command in ('test', 'build'):
            args.build_root = args.build_root.expanduser().resolve()
            requested = args.module.lower()
            aliases = {module['path'].lower(): name for name, module in modules.items()}
            requested = aliases.get(requested, requested)
            host_required = tooling_required = False
            if requested == 'all':
                selected = list(modules)
            elif requested == 'changed':
                selected, host_required, tooling_required = change_scopes(changed_paths(args.base), modules)
            elif requested in modules:
                selected = [requested]
            elif requested == 'studio' and args.command == 'test':
                selected = []
            else:
                raise DevError(f'Unknown module: {args.module}. Use ./dev list.')
            if selected:
                print('Modules: ' + ', '.join(selected), flush=True)
            for name in selected:
                if args.command == 'test':
                    test_module(name, modules[name], args, runner)
                else:
                    build_module(name, modules[name], args, runner)
            if args.command == 'test' and (requested in ('all', 'studio') or
                                           (requested == 'changed' and (tooling_required or len(selected) == len(modules)))):
                for script in ('test_dev.py', 'test_package.py', 'test_preparation.py', 'test_verification.py', 'test_publication.py'):
                    runner.run([sys.executable, '-B', ROOT / 'Studio/tests' / script], env=common_environment())
            if not selected and requested != 'studio':
                status = ('DRY RUN' if runner.dry_run else
                          'HOST INTEGRATION NOT RUN' if host_required else
                          'TOOLING CHECKS PASS' if tooling_required and args.command == 'test' else 'NO CHANGES')
                if status == 'NO CHANGES':
                    print('No affected modules. This is NOT product acceptance; use --base REF for a committed range.')
            else:
                if runner.dry_run:
                    status = 'DRY RUN'
                elif args.command == 'build':
                    status = 'BUILT — HOST BUILD NOT RUN' if host_required else 'BUILT — TESTS NOT RUN'
                else:
                    status = ('TOOLING CHECKS PASS — HOST INTEGRATION NOT RUN' if requested == 'studio' else
                              'CONTRACTS PASS — HOST INTEGRATION NOT RUN' if host_required and args.contracts_only else
                              'MODULE CHECKS PASS — HOST INTEGRATION NOT RUN' if host_required else
                              'CONTRACTS PASS' if args.contracts_only else 'MODULE CHECKS PASS')
        elif args.command == 'bootstrap':
            runner.run([sys.executable, '-B', ROOT / 'Studio/scripts/fetch-upstream.py'], env=common_environment())
            status = 'PREPARED'
        elif args.command == 'integration':
            import importlib.util
            import re
            spec = importlib.util.spec_from_file_location('studio_host_dev', ROOT / 'Studio/scripts/host_dev.py')
            host_dev = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(host_dev)
            if not re.fullmatch(r'[A-Za-z0-9_-]+', args.test):
                raise DevError('Invalid host test target')
            if args.case and args.test != 'studioregressiontest':
                raise DevError('--case is supported only for studioregressiontest')
            source_sha = host_dev.source_head(ROOT)
            report_source = {'root': str(ROOT.resolve()), 'sha': source_sha, 'dirty': None}
            print(f"Source: {report_source['root']} @ {source_sha or 'unversioned'}", flush=True)
            context = host_dev.prepare(ROOT, args.host_build)
            if args.report_dir is None:
                runner.directory = context['work'] / 'dev-host/reports' / runner.directory.name
            host_evidence = dict(context['evidence'], input_sha256=context['fingerprint'],
                                 synced_files=context['sync'])
            app_was_ready = host_dev.app_ready(context)
            host_dev.check_test_binary(context, args.test)
            host_evidence['app_reused'] = app_was_ready
            manifest = context['manifest']
            build = '/run/build/kdenlive/_flatpak_build'
            app_build_steps = 0
            if not app_was_ready:
                runner.run(['flatpak-builder', '--run', 'sdk-check', manifest, 'cmake', '--build', build,
                            '--target', 'kdenlive', '--parallel', str(args.jobs)],
                           cwd=context['work'], env=common_environment())
                host_dev.verify_inputs(context)
                host_dev.record_app(context)
                app_log = Path(runner.results[-1].log).read_text(encoding='utf-8', errors='replace')
                app_build_steps = sum(line.startswith('[') and '/' in line[:16]
                                      for line in app_log.splitlines())
            build_script = (f'test -d /app/include; '
                            f'cmake --build {build} --target kdenliveLib kdenlive_render {args.test} --parallel {args.jobs}; '
                            f'pending=$(ninja -C {build} -n kdenliveLib kdenlive_render {args.test}); '
                            'printf "%s\\n" "$pending"; '
                            'case "$pending" in *"ninja: no work to do."*) ;; *) exit 1;; esac')
            runner.run(['flatpak-builder', '--run', 'sdk-check', manifest, 'sh', '-ec', build_script],
                       cwd=context['work'], env=common_environment())
            host_dev.verify_inputs(context)
            binary = context['build'] / 'bin' / args.test
            host_evidence['test_binary_sha256'] = host_dev.digest(binary)
            build_log = Path(runner.results[-1].log).read_text(encoding='utf-8', errors='replace')
            steps = [line for line in build_log.splitlines()
                     if line.startswith('[') and '/' in line[:16]]
            host_evidence['build_steps'] = app_build_steps + len(steps)
            if app_was_ready and any(args.test not in line for line in steps):
                runner.run(['flatpak-builder', '--run', 'sdk-check', manifest, 'cmake', '--build', build,
                            '--target', 'kdenlive', '--parallel', str(args.jobs)],
                           cwd=context['work'], env=common_environment())
                host_dev.verify_inputs(context)
                app_was_ready = False
                host_evidence['app_reused'] = False
                app_log = Path(runner.results[-1].log).read_text(encoding='utf-8', errors='replace')
                host_evidence['build_steps'] += sum(line.startswith('[') and '/' in line[:16]
                                                    for line in app_log.splitlines())
                host_dev.record_app(context)
            host_dev.record_test_binary(context, args.test)
            host_evidence['app_binary_sha256'] = host_dev.digest(context['build'] / 'bin/kdenlive')
            if args.build_only:
                status = 'HOST BUILT — MODEL TEST NOT RUN'
            else:
                qa_environment = [f'{key}={value}' for key, value in os.environ.items() if key.startswith('STUDIO_QA_')]
                if args.case:
                    command = ['flatpak-builder', '--run', 'sdk-check', manifest, 'env',
                               *qa_environment,
                               'FREI0R_PATH=/app/lib/frei0r-1', 'MLT_REPOSITORY=/app/lib/mlt-7',
                               'QT_QPA_PLATFORM=offscreen', 'sh', '-ec',
                               f'export PATH={build}/bin:$PATH; exec "$@"', 'studio-test', f'{build}/bin/{args.test}',
                               '--warn', 'NoTests', args.case]
                else:
                    test_script = (f'export FREI0R_PATH=/app/lib/frei0r-1 MLT_REPOSITORY=/app/lib/mlt-7 '
                                   f'QT_QPA_PLATFORM=offscreen PATH={build}/bin:$PATH; '
                                   f'ctest --test-dir {build} --output-on-failure --no-tests=error '
                                   f'--timeout {min(300, int(timeout))} --output-junit host-model-tests.xml '
                                   f"-R '^{args.test}$'")
                    command = ['flatpak-builder', '--run', 'sdk-check', manifest, 'env', *qa_environment, 'sh', '-ec', test_script]
                runner.run(command, cwd=context['work'], env=common_environment(),
                           timeout=min(300, timeout))
                host_dev.verify_inputs(context)
                if host_dev.digest(binary) != host_evidence['test_binary_sha256']:
                    raise DevError('Host test binary changed during verification')
                host_evidence['test_case'] = args.case
                status = 'HOST TEST PASS'
        elif args.command == 'release':
            import re
            if not re.fullmatch(r'[0-9]+\.[0-9]+(-rc[0-9]+)?', args.version):
                raise DevError('Invalid release version')
            for mode in ('baseline', 'studio'):
                runner.run(['bash', ROOT / 'Studio/scripts/build.sh', mode])
            runner.run(['bash', ROOT / 'Studio/scripts/check-sdk.sh'])
            runner.run(['bash', ROOT / 'Studio/scripts/package.sh', args.version])
            status = 'PACKAGE CREATED — GUI acceptance separate'
    except (DevError, OSError, ValueError):
        status = 'FAIL'
        raise
    finally:
        if args.command == 'integration':
            write_report(runner, selected, status, args.command, report_source, host_evidence)
        else:
            write_report(runner, selected, status, args.command)
    print(f'{status}. GUI, Save/Reopen, Export, Flatpak and Deck are not certified by module tests.')
    return 3 if args.command in ('test', 'build') and host_required and not runner.dry_run else 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (DevError, OSError, ValueError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        raise SystemExit(1)
