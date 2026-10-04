#!/usr/bin/env python3
"""Compatibility entrypoint: offline module checks, explicit --sdk release checks."""
import argparse
from pathlib import Path
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sdk', action='store_true', help='Explicit full pinned build/check; may download dependencies')
    parser.add_argument('--contracts-only', action='store_true')
    args = parser.parse_args()
    command = [sys.executable, '-B', str(ROOT/'Studio/scripts/dev.py'), 'test', 'all']
    if args.contracts_only:
        command.append('--contracts-only')
    subprocess.run(command, cwd=ROOT, check=True)
    if args.sdk:
        if sys.platform != 'linux':
            raise SystemExit('--sdk requires Linux/WSL and the pinned Flatpak SDK.')
        for mode in ('baseline', 'studio'):
            subprocess.run(['bash', str(ROOT/'Studio/scripts/build.sh'), mode], cwd=ROOT, check=True)
        subprocess.run(['bash', str(ROOT/'Studio/scripts/check-sdk.sh')], cwd=ROOT, check=True)
if __name__ == '__main__':
    main()
