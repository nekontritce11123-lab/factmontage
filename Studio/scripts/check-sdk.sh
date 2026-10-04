#!/usr/bin/env bash
# Automatic checks only; never launches an interactive editor.
set -euo pipefail
if [[ "${1:-}" != --inside ]]; then
  root="$(cd "$(dirname "$0")/../.." && pwd)"
  work="${STUDIO_BUILD_ROOT:-$HOME/video-studio}"
  python3 "$root/Studio/scripts/verification.py" verify build "$work"
  validation="$(mktemp -d "$work/validation.XXXXXX")"
  tar -xf "$work/studio-input/studio-changes-source.tar.gz" -C "$validation"
  cp "$root/Studio/upstream/kdenlive-26.08.0.tar.xz" "$validation/Studio/upstream/"
  tar -xf "$validation/Studio/upstream/kdenlive-26.08.0.tar.xz" -C "$validation/Studio/upstream"
  flatpak run --share=network --command=python3 --filesystem="$work" org.kde.Sdk//6.10 -m pip download \
    --only-binary=:all: --no-deps --require-hashes -r "$validation/Studio/scripts/test-requirements.txt" --dest "$work/test-wheels"
  cd "$work"
  # Build only development dependencies; test the exact finished app binary.
  flatpak-builder --disable-updates --disable-rofiles-fuse --ccache --force-clean --keep-build-dirs --stop-at=kdenlive --jobs=2 \
    sdk-deps studio-input/local.VideoStudio.Kdenlive.json
  stage="$(mktemp -d "$work/sdk-validation.XXXXXX")"
  case "$stage" in "$work"/sdk-validation.*) ;; *) echo 'Invalid validation directory.' >&2; exit 1;; esac
  trap 'rm -rf -- "$stage"' EXIT
  cp -al "$work/studio/." "$stage/"
  for directory in include lib/pkgconfig lib/cmake lib64/pkgconfig lib64/cmake share/pkgconfig share/eigen3/cmake; do
    if [[ -d "$work/sdk-deps/files/$directory" && ! -e "$stage/files/$directory" ]]; then
      mkdir -p "$(dirname "$stage/files/$directory")"
      ln -s "$work/sdk-deps/files/$directory" "$stage/files/$directory"
    fi
  done
  test "$(sha256sum "$work/studio/files/bin/kdenlive" | cut -d' ' -f1)" = \
       "$(sha256sum "$stage/files/bin/kdenlive" | cut -d' ' -f1)"
  flatpak-builder --run "$stage" studio-input/local.VideoStudio.Kdenlive.json \
    bash "$validation/Studio/scripts/check-sdk.sh" --inside "$validation" "$work" 2>&1 | tee "$validation/sdk-tests.log"
  bash "$validation/Studio/scripts/check-built.sh"
  flatpak-builder --run baseline upstream/baseline.json \
    python3 "$validation/Studio/scripts/benchmark.py" --baseline --measure "$validation/benchmark-studio/measure" --output "$validation/benchmark-baseline.json"
  bash "$validation/Studio/scripts/check-installed.sh" "$validation" "${STUDIO_PREVIOUS_BUNDLE:-$root/releases/1.0-rc3/VideoStudio-Kdenlive-1.0-rc3.flatpak}" \
    2>&1 | tee "$validation/installed-tests.log"
  mkdir -p "$work/test-results"
  cp "$validation/sdk-tests.log" "$validation/installed-tests.log" "$validation/audio-integration.json" "$validation/benchmark-baseline.json" "$validation/benchmark-studio.json" "$work/test-results/"
  cp "$validation/Card_3D/docs/environment.json" "$work/test-results/environment.json"
  cp -R "$work/studio-test-results/". "$work/test-results/"
  python3 "$root/Studio/scripts/verification.py" record tests "$work"
  echo "Automatic checks passed. Evidence: $validation. Deck acceptance remains open."
  exit
fi
validation="$2"
work="$3"
export QT_QPA_PLATFORM=offscreen PYTHONDONTWRITEBYTECODE=1 PYTHONUTF8=1
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
export CPLUS_INCLUDE_PATH=/app/include
export PKG_CONFIG_PATH=/app/lib/pkgconfig:/app/lib64/pkgconfig
export LD_LIBRARY_PATH=/app/lib:/app/lib64
python3 -m pip install --no-index --find-links="$work/test-wheels" --require-hashes \
  --target "$validation/python" -r "$validation/Studio/scripts/test-requirements.txt"
export PYTHONPATH="$validation/python"
modules=(Card_3D Camera Effects Transitions Color Audio Subtitles Background Text)
archives=(card3d studio-camera studiofx studio-transitions studio-color studio-audio studio-subtitles studio-background studio-text)
for i in "${!modules[@]}"; do
  mkdir "$validation/${modules[$i]}"
  tar -xf "$work/studio-input/${archives[$i]}-source.tar.gz" -C "$validation/${modules[$i]}"
done
python3 "$validation/Studio/scripts/check-all.py"
bash "$validation/Card_3D/scripts/check-linux.sh"
bash "$validation/Camera/scripts/check-linux.sh"
# Flatpak's /var/tmp can be FUSE; remove_all may report ENOTEMPTY there.
export TMPDIR=/tmp
for module in Effects Transitions Color Audio Subtitles Background Text; do
  opts=()
  case "$module" in
    Effects) opts=(-DFREI0R_INCLUDE_DIR=/app/include);;
    Transitions) opts=(-DTRANSITIONS_SYSTEM_FREI0R=ON);;
    Color) opts=(-DSTUDIO_COLOR_MLT=ON);;
    Background) opts=(-DSBG_WITH_MLT=ON);;
    Subtitles) opts=(-DBUILD_TESTING=ON);;
    Text) opts=(-DSTXT_BUILD_QT_COMPILER=ON);;
  esac
  cmake -S "$validation/$module" -B "$validation/$module/build/check" -G Ninja -DCMAKE_BUILD_TYPE=Debug "${opts[@]}"
  cmake --build "$validation/$module/build/check" --parallel 2
  ctest --test-dir "$validation/$module/build/check" --output-on-failure --no-tests=error
  cmake -S "$validation/$module" -B "$validation/$module/build/sanitize" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
    -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' "${opts[@]}"
  cmake --build "$validation/$module/build/sanitize" --parallel 2
  if [[ "$module" == Text || "$module" == Transitions ]]; then
    # Python loads the ASan-instrumented frei0r library through ctypes.
    asan_runtime="$(c++ -print-file-name=libasan.so)"
    test -f "$asan_runtime" || { echo 'ASan runtime is missing from SDK.' >&2; exit 1; }
    LD_PRELOAD="$asan_runtime${LD_PRELOAD:+:$LD_PRELOAD}" \
      ctest --test-dir "$validation/$module/build/sanitize" --output-on-failure --no-tests=error
  else
    ctest --test-dir "$validation/$module/build/sanitize" --output-on-failure --no-tests=error
  fi
done
python3 "$validation/Audio/tests/integration.py" --worker /app/libexec/studio-audio --report "$validation/audio-integration.json"
python3 "$validation/Studio/tests/background_analyzer_check.py"
python3 "$validation/Studio/scripts/benchmark.py" --output "$validation/benchmark-studio.json"
test -s "$work/studio-test-results/model-tests.xml"
