#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Stop at the first failed or unavailable check. Never package an old library.
bash scripts/check-linux.sh
python3 tests/make_assets.py
python3 tests/render_showcase.py
python3 tests/benchmark.py
python3 - <<'PY'
from pathlib import Path
import hashlib
import tarfile
root=Path.cwd()
files=sorted(p for p in root.rglob('*') if p.is_file() and not any(x in p.relative_to(root).parts for x in ('build','__pycache__')) and p.name!='SHA256SUMS.txt')
manifest=root/'SHA256SUMS.txt'
manifest.write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(root).as_posix()+'\n' for p in files),encoding='utf-8')
archive=root/'build/Card_3D_linux_x86_64.tar.gz'
with tarfile.open(archive,'w:gz') as out:
    for p in files+[manifest]:out.add(p,arcname='Card_3D/'+p.relative_to(root).as_posix())
print(archive)
PY
