#!/usr/bin/env python3
"""Export a new, standalone source tree without the private repository history."""
import argparse
import contextlib
import importlib.util
import io
from pathlib import Path, PurePosixPath
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ARCHIVES = {
    'card3d': 'Card_3D', 'studio-camera': 'Camera', 'studio-background': 'Background',
    'studio-transitions': 'Transitions', 'studiofx': 'Effects', 'studio-color': 'Color',
    'studio-audio': 'Audio', 'studio-subtitles': 'Subtitles', 'studio-text': 'Text',
}
DOCUMENTS = (
    'BUILD.md', 'BUILD_RU.md', 'INSTALL.md', 'INSTALL_RU.md', 'PROJECTS.md', 'PROJECTS_RU.md',
    'RELEASE_NOTES.md', 'RELEASE_NOTES_RU.md', 'TESTING.md',
)


def validate_name(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name:
        raise ValueError(f'Unsafe source path: {name}')
    return path.as_posix()


def export_sources(target):
    target = Path(target).resolve()
    if target.exists():
        raise ValueError(f'Export destination already exists: {target}')
    spec = importlib.util.spec_from_file_location('studio_prepare_export', ROOT / 'Studio/scripts/prepare.py')
    prepare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prepare)
    files = {}
    with tempfile.TemporaryDirectory(prefix='factmontage-inputs-') as folder:
        with contextlib.redirect_stdout(io.StringIO()):
            prepare.main(output=folder)
        for stem, prefix in [*ARCHIVES.items(), ('studio-changes', '')]:
            with tarfile.open(Path(folder) / (stem + '-source.tar.gz')) as archive:
                for entry in archive.getmembers():
                    if not entry.isfile():
                        raise ValueError(f'Unexpected source entry: {entry.name}')
                    name = validate_name('/'.join(part for part in (prefix, entry.name) if part))
                    data = archive.extractfile(entry).read()
                    if name in files and files[name] != data:
                        raise ValueError(f'Conflicting source entry: {name}')
                    files[name] = data
    for name in DOCUMENTS:
        path = ROOT / 'docs' / name
        files['docs/' + name] = path.read_bytes()
    for module in ARCHIVES.values():
        path = ROOT / module / 'README_RU.md'
        if path.is_file():
            files[module + '/README_RU.md'] = path.read_bytes()
    for directory in ('docs/assets', 'docs/screenshots', 'docs/demo', '.github/ISSUE_TEMPLATE', '.github/workflows', 'Text/web'):
        for path in sorted((ROOT / directory).rglob('*')):
            if path.is_file() and path.suffix.lower() in {'.md', '.svg', '.png', '.json', '.py', '.yml', '.yaml', '.js', '.css', '.html'}:
                files[path.relative_to(ROOT).as_posix()] = path.read_bytes()
    forbidden = {'.git', '.beads', '.build', 'archive', 'releases'}
    if any(PurePosixPath(name).parts[0] in forbidden for name in files):
        raise ValueError('Private state entered the public source export')
    target.mkdir(parents=True)
    for name, data in sorted(files.items()):
        path = target / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        if name == 'dev' or path.suffix == '.sh':
            path.chmod(0o755)
    return sorted(files)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    names = export_sources(args.destination)
    print(f'Exported {len(names)} files to {args.destination}')
