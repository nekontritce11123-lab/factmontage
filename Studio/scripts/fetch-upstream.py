#!/usr/bin/env python3
"""Fetch and verify the pristine editor used by integration tests."""
import hashlib
import json
from pathlib import Path
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    upstream = ROOT/'upstream'
    lock = json.loads((upstream/'sources.json').read_text(encoding='utf-8-sig'))
    manifest = upstream/'org.kde.kdenlive.json'
    if hashlib.sha256(manifest.read_bytes()).hexdigest() != lock['manifest_sha256'].lower():
        raise SystemExit('Unexpected upstream manifest hash')
    source = json.loads(manifest.read_text(encoding='utf-8'))['modules'][-1]['sources'][0]
    archive = upstream/Path(source['url']).name
    if not archive.exists():
        temporary = archive.with_suffix(archive.suffix + '.part')
        try:
            urllib.request.urlretrieve(source['url'], temporary)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != lock['archive_sha256']:
                raise SystemExit('Downloaded source hash mismatch')
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != lock['archive_sha256']:
        raise SystemExit('Local source archive hash mismatch')
    target = upstream/('kdenlive-' + lock['kdenlive'])
    if target.exists():
        print('Pristine source directory already present; not overwritten:', target)
        return
    with tempfile.TemporaryDirectory(dir=upstream, prefix='extract-') as folder:
        with tarfile.open(archive) as bundle:
            bundle.extractall(folder, filter='data')
        (Path(folder)/target.name).rename(target)
    print('Verified and extracted:', target)


if __name__ == '__main__':
    main()
