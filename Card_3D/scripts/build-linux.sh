#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test "$(uname -s)" = Linux
test "$(uname -m)" = x86_64
python3 scripts/generate_ui.py --check
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --clean-first --parallel 2
mkdir -p bin/linux-x86_64
cp build/linux/card3d.so bin/linux-x86_64/card3d.so
printf '\nBuilt bin/linux-x86_64/card3d.so\n'
