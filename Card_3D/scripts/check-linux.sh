#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash scripts/build-linux.sh
mkdir -p build/linux docs
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -shared -fPIC -fvisibility=hidden -DCARD3D_TEST_API src/card_studio.cpp -o build/linux/card3d_test.so
g++ -std=c++17 -O2 -Wall -Wextra tests/abi_host.cpp -ldl -o build/linux/abi_host
build/linux/abi_host "$PWD/bin/linux-x86_64/card3d.so" 2>&1 | tee docs/abi_log.txt
python3 tests/test_public.py 2>&1 | tee docs/public_test_log.txt
python3 tests/test_studio_schema.py 2>&1 | tee docs/studio_schema_log.txt
python3 tests/test_engine.py 2>&1 | tee docs/engine_test_log.txt
python3 tests/test_geometry.py 2>&1 | tee docs/geometry_test_log.txt
python3 tests/test_installer.py 2>&1 | tee docs/installer_test_log.txt
g++ -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined tests/sanitizer_smoke.cpp -o build/linux/sanitizer_smoke
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 build/linux/sanitizer_smoke 2>&1 | tee docs/sanitizer_log.txt
python3 tests/test_mlt.py 2>&1 | tee docs/mlt_test_log.txt
python3 scripts/record_environment.py
printf '\nLinux checks passed. Flatpak GUI and Steam Deck acceptance are still required.\n'
