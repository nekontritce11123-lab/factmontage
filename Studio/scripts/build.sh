#!/usr/bin/env bash
# Build on WSL's native Linux filesystem, never directly under /mnt/c or /mnt/e.
set -euo pipefail
studio_root="$(cd "$(dirname "$0")/.." && pwd)"
mode="${1:-baseline}"
case "$mode" in baseline|studio) ;; *) echo 'Usage: build.sh baseline|studio' >&2; exit 2;; esac
if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 || "$EUID" == 0 ]]; then
  echo 'Use a non-root Linux x86-64 build user.' >&2
  exit 1
fi
python3 - "$studio_root/upstream/sources.json" <<'PY'
import hashlib, json, pathlib, subprocess, sys
lock=json.load(open(sys.argv[1], encoding='utf-8-sig'))
source=pathlib.Path(sys.argv[1]).with_name('org.kde.kdenlive.json')
if hashlib.sha256(source.read_bytes()).hexdigest()!=lock['manifest_sha256'].lower():
    raise SystemExit('Upstream manifest differs from sources.json')
for name,branch,key in [('org.kde.Sdk','6.10','kde_sdk_commit'),('org.kde.Platform','6.10','kde_runtime_commit'),('org.freedesktop.Sdk.Extension.llvm21','25.08','llvm_sdk_commit')]:
    actual=subprocess.check_output(['flatpak','info','--show-commit',name+'//'+branch],text=True).strip()
    if actual!=lock[key]: raise SystemExit(name+': installed revision differs from sources.json; review SDK update before rebuilding')
PY
work="${STUDIO_BUILD_ROOT:-$HOME/video-studio}"
mkdir -p "$work"
case "$(realpath "$work")" in /mnt/*) echo 'STUDIO_BUILD_ROOT must be on the Linux filesystem' >&2; exit 1;; esac
if [[ "$mode" == baseline ]]; then
  python3 "$studio_root/scripts/prepare.py" --baseline --output "$work/upstream"
  manifest=upstream/baseline.json
else
  test -f "$work/baseline/files/bin/kdenlive" || { echo 'Build the unchanged baseline first.' >&2; exit 1; }
  (cd "$work" && sha256sum --check baseline.manifest.sha256) || { echo 'A successful baseline for this manifest is required.' >&2; exit 1; }
  python3 "$studio_root/scripts/prepare.py" --baseline --output "$work/baseline-input-current"
  cmp "$work/upstream/baseline.json" "$work/baseline-input-current/baseline.json" || { echo 'Baseline inputs changed; rebuild baseline first.' >&2; exit 1; }
  python3 "$studio_root/scripts/prepare.py" --output "$work/studio-input"
  manifest=studio-input/local.VideoStudio.Kdenlive.json
  python3 "$studio_root/scripts/verification.py" begin build "$work"
fi
cd "$work"
# Target directories are dedicated disposable build outputs, not projects.
flatpak-builder --disable-updates --disable-rofiles-fuse --ccache --force-clean --keep-build-dirs --jobs="${STUDIO_BUILD_JOBS:-2}" "$mode" "$manifest" 2>&1 | tee "$mode-build.log"
if [[ "$mode" == baseline ]]; then
  sha256sum "$manifest" > baseline.manifest.sha256
else
  model_tests="$(find "$work/.flatpak-builder/build" -type f -name studio-model-tests.xml -printf '%T@ %p\n' | sort -nr | sed -n '1s/^[^ ]* //p')"
  test -s "$model_tests" || { echo 'Studio model test report was not produced by the current build.' >&2; exit 1; }
  test -d "$(dirname "$model_tests")/studio-ui" || { echo 'Studio UI evidence was not produced by the current build.' >&2; exit 1; }
  evidence_stage="$(mktemp -d "$work/studio-test-results.XXXXXX")"
  cp "$model_tests" "$evidence_stage/model-tests.xml"
  cp -R "$(dirname "$model_tests")/studio-ui" "$evidence_stage/"
  rm -rf "$work/studio-test-results"
  mv "$evidence_stage" "$work/studio-test-results"
  flatpak-builder --run "$mode" "$manifest" sh -ec '
    QT_QPA_PLATFORM=offscreen kdenlive --version
    melt -query filter=studio.camera | grep -q "identifier: studio.camera"
    melt -query filter=frei0r.card3d | grep -q "identifier: frei0r.card3d"
    melt color:red out=180 -attach studio.camera mode=5 live=0 zoom=140 cycle_period=6 -consumer null real_time=-1
    melt color:red out=30 -attach frei0r.card3d 0=0 1=.25 2=0 3=.16666666666666666 4=.68 5=.34 6=0 7=1 8=.2 9=1 10=.35 11=1 12=.2 13=.8 -consumer null real_time=-1
    test -r /app/share/kdenlive/studio/studio.json
    test -r /app/share/kdenlive/studio/studio_camera.json
    test -x /app/bin/studio-head-tracker
    melt qimage:/app/share/kdenlive/studio/camera_mode_0.png out=30 -consumer avformat:/tmp/studio-head-test.mkv vcodec=ffv1 an=1 real_time=-1
    studio-head-tracker /tmp/studio-head-test.mkv /tmp/studio-head-test.scam 0 1 .35 .25 .3 .5
    grep -q '^SUNIMO_CAMERA_TRACK_V1$' /tmp/studio-head-test.scam
  '
  python3 "$studio_root/scripts/verification.py" record build "$work"
fi
echo "Built $work/$mode; GUI/Deck acceptance and release packaging are separate."
