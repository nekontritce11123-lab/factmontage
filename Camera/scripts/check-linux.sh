#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${CAMERA_BUILD_DIR:-$root/build/linux-check}"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTUDIO_CAMERA_HEAD_TRACKER=OFF
cmake --build "$build"
python3 "$root/tests/test_contract.py"
gcc -std=c11 -O2 -Wall -Wextra "$root/tests/test_engine.c" "$root/src/camera.c" -o "$build/test-engine" -lm -pthread
"$build/test-engine"
gcc -std=c11 -O2 -Wall -Wextra "$root/tests/test_track.c" "$root/src/track.c" -o "$build/test-track" -lm
"$build/test-track" "$root/tests/sample.scam"
gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "$root/tests/test_engine.c" "$root/src/camera.c" -o "$build/test-engine-sanitize" -lm -pthread
ASAN_OPTIONS=detect_leaks=1 "$build/test-engine-sanitize"
python3 "$root/tests/test_mlt.py" "$build/libmltstudiocamera.so"
echo 'Camera contract, engine, ASan/UBSan and MLT: PASS'
