#!/usr/bin/env bash
# Verify the assembled Flatpak tree without requiring a graphical session.
set -euo pipefail
work="${STUDIO_BUILD_ROOT:-$HOME/video-studio}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
stage="${1:-studio}"
[[ "$stage" == studio || "$stage" == sdk-check ]] || { echo 'Expected studio or sdk-check build stage.' >&2; exit 2; }
manifest="$work/studio-input/local.VideoStudio.Kdenlive.json"
test -x "$work/$stage/files/bin/kdenlive" || { echo "Build $stage first." >&2; exit 1; }
test -f "$work/$stage/files/extensions/Plugins/.keep" || { echo 'Flatpak plugin extension mount point is missing.' >&2; exit 1; }
cd "$work"
flatpak-builder --run "$stage" "$manifest" sh -ec '
  export QT_QPA_PLATFORM=offscreen
  melt -query filters >/tmp/studio-filters.txt 2>/tmp/studio-errors.txt || true
  test ! -s /tmp/studio-errors.txt || { cat /tmp/studio-errors.txt >&2; exit 1; }
  grep -Fq frei0r.card3d /tmp/studio-filters.txt
  grep -Fq frei0r.c0rners /tmp/studio-filters.txt
  grep -Fq studio.camera /tmp/studio-filters.txt
  grep -Fq studio.background /tmp/studio-filters.txt
  grep -Fq studio.color /tmp/studio-filters.txt
  grep -Fq frei0r.studiofx /tmp/studio-filters.txt
  grep -Fq frei0r.studio_lens /tmp/studio-filters.txt
  grep -Fq frei0r.studio_vintage /tmp/studio-filters.txt
  grep -Fq frei0r.sunimo_text_studio /tmp/studio-filters.txt
  melt -query transitions >/tmp/studio-transitions.txt
  grep -Fq frei0r.sunimo_transition /tmp/studio-transitions.txt
  test -x /app/libexec/studio-audio
  test -f /app/share/studio-audio/parameters.json
  test -x /app/libexec/studio-subtitle
  test -x /app/bin/whisper-cli
  test -s /app/share/studio-subtitles/parameters.json
  test -s /app/share/studio-subtitles/ggml-small-q5_1.bin
  test -s /app/share/studio-subtitles/ggml-silero-v5.1.2.bin
  test "$(sha256sum /app/share/studio-subtitles/ggml-silero-v5.1.2.bin | cut -d" " -f1)" = 29940d98d42b91fbd05ce489f3ecf7c72f0a42f027e4875919a28fb4c04ea2cf
  test -s /app/share/licenses/studio-subtitles/SILERO_VAD_LICENSE.txt
  test -x /app/bin/studio-background-analyzer
  test -f /app/share/studio-background/models/pp_humansegv2_lite_portrait_static.onnx
  test "$(sha256sum /app/share/studio-background/models/pp_humansegv2_lite_portrait_static.onnx | cut -d" " -f1)" = 2047b95bea344af15575f4279d6fac44bb07a42e7d537cf8d3265f15fe95cc82
  test -s /app/share/licenses/studio-background/PADDLESEG_LICENSE.txt
  test -f /app/share/kdenlive/studio/studio_background.json
  test -f /app/share/kdenlive/effects/studio_background.xml
  test -f /app/share/kdenlive/effects/studio_color.xml
  test -f /app/share/kdenlive/effects/sunimo_text_studio.xml
  test -f /app/share/kdenlive/effects/frei0r_c0rners.xml
  test -s /app/lib/frei0r-1/c0rners.so
  test -s /app/share/studio-text/catalog.json
  test "$(grep -c '"'"'    "id":'"'"' /app/share/studio-text/catalog.json)" -eq 100
  grep -Fq '"'"'"version": "0.6.0"'"'"' /app/share/studio-subtitles/parameters.json
  test "$(find /app/share/kdenlive/studio/previews -name 'background_*.png' | wc -l)" -eq 6
  test "$(find /app/share/kdenlive/studio/previews -name 'background_*.gif' | wc -l)" -eq 6
  echo "Installed resources: OK"
  /app/bin/studio-background-analyzer --capabilities | grep -Fq "\"max_threads\":2"
  /app/libexec/studio-audio --help | grep -Fq "voice|music|mix|pauses"
  /app/libexec/studio-subtitle --help | grep -Fq "русских субтитров"
  /app/bin/whisper-cli --help >/tmp/studio-whisper-help.txt 2>&1
  grep -Fq -- "--vad-model" /tmp/studio-whisper-help.txt
  echo "Native helpers: OK"
  ffmpeg -hide_banner -filters >/tmp/studio-ffmpeg-filters.txt
  for filter in highpass afftdn deesser acompressor loudnorm ebur128 silencedetect aresample aformat alimiter; do
    grep -Fq "$filter" /tmp/studio-ffmpeg-filters.txt
  done
  echo "FFmpeg filters: OK"
  melt -query filter=frei0r.card3d >/tmp/card3d-metadata.txt
  grep -Fq "title: POSITION_MODE" /tmp/card3d-metadata.txt
  grep -Fq "title: X" /tmp/card3d-metadata.txt
  grep -Fq "title: Y" /tmp/card3d-metadata.txt
  echo "Card metadata: OK"
  melt color:red out=30 -attach frei0r.card3d 0=0 1=.25 2=0 3=.16666666666666666 4=.68 5=.34 6=0 7=1 8=.2 9=1 10=.35 11=1 12=.2 13=.8 -consumer null real_time=-1
  melt color:red out=30 -attach frei0r.studiofx 0=.07874015748031496 1=.46 2=.47 3=.5 4=1 5=.5 6=.5 7=.65 8=1 9=.137 10=0 threads=1 -consumer null real_time=-1
  melt color:red out=30 -attach frei0r.studio_lens 0=.7 1=0 2=.9 3=.5 4=.5 5=.08 6=.4 -consumer null real_time=-1
  melt color:red out=30 -attach frei0r.studio_vintage 0=.55 1=.25 2=.2 3=.1 4=.2 5=.2 6=.1 -consumer null real_time=-1
  melt color:red out=30 -attach studio.color studio_input_validated=1 studio_color_algorithm=sdr-primary-v1 exposure=.2 saturation=105 -consumer null real_time=-1
  transition_dir=$(mktemp -d)
  trap "rm -rf -- \"$transition_dir\"" EXIT
  melt color:red out=29 color:blue out=29 -mix 15 -mixer frei0r.sunimo_transition "0=0=0;14=1" 1=0 2=.45 3=0 4=.35 "5=.5 .5" 6=0 -consumer "avformat:$transition_dir/transition.mkv" vcodec=ffv1 an=1 real_time=-1
  ffmpeg -v error -i "$transition_dir/transition.mkv" -vf scale=1:1 -f rawvideo -pix_fmt rgb24 "$transition_dir/frames.rgb"
  test "$(wc -c <"$transition_dir/frames.rgb")" -eq 135
  set -- $(dd if="$transition_dir/frames.rgb" bs=3 skip=22 count=1 status=none | od -An -tu1)
  test "$1" -gt 30 && test "$3" -gt 30
  echo "MLT renders: OK"
  printf "MLT filters: "
  wc -l </tmp/studio-filters.txt
'
flatpak-builder --run "$stage" "$manifest" env FREI0R_PATH=/app/lib/frei0r-1 MLT_REPOSITORY=/app/lib/mlt-7 \
  QT_QPA_PLATFORM=offscreen python3 "$root/Studio/tests/full_smoke.py"
flatpak-builder --run "$stage" "$manifest" env QT_QPA_PLATFORM=offscreen \
  python3 "$root/Studio/tests/preview_geometry.py"
