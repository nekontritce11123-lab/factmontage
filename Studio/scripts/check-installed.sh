#!/usr/bin/env bash
# Exercise deployment in a separate user installation; do not touch installed editors.
set -euo pipefail
validation="${1:?validation directory required}"
previous="${2:?previous candidate bundle required}"
work="${STUDIO_BUILD_ROOT:-$HOME/video-studio}"
test -f "$previous" || { echo 'Previous candidate is required to verify upgrade and rollback.' >&2; exit 1; }
app=local.VideoStudio.Kdenlive
official_before="$(flatpak info --show-commit org.kde.kdenlive 2>/dev/null || true)"
studio_before="$(flatpak info --user --show-commit "$app" 2>/dev/null || true)"
flatpak build-export --no-update-summary --exclude=lib/debug "$work/repository" "$work/studio" experimental
flatpak build-bundle "$work/repository" "$validation/install-test.flatpak" "$app" experimental
export FLATPAK_USER_DIR="$validation/flatpak-installation"
flatpak install --user --noninteractive "$previous"
old_commit="$(flatpak info --user --show-commit "$app")"
flatpak install --user --noninteractive --reinstall "$validation/install-test.flatpak"
new_commit="$(flatpak info --user --show-commit "$app")"
test "$old_commit" != "$new_commit"
expected_whisper="$(sha256sum "$work/studio/files/bin/whisper-cli" | cut -d' ' -f1)"
flatpak run --user --env=QT_QPA_PLATFORM=offscreen --env=XDG_CONFIG_HOME=/tmp/studio-installed-config \
  --env=XDG_CACHE_HOME=/tmp/studio-installed-cache --env=XDG_DATA_HOME=/tmp/studio-installed-data \
  --env=STUDIO_EXPECTED_WHISPER_SHA256="$expected_whisper" --command=sh "$app" -ec '
  kdenlive --version
  test "$MLT_REPOSITORY" = /app/lib/mlt-7
  test "$MLT_DATA" = /app/share/mlt-7
  for item in studio.json studio_camera.json studio_background.json studio_transition.json studiofx.json studio_lens.json studio_vintage.json studio_color.json; do
    test -s "/app/share/kdenlive/studio/$item"
  done
  test -s /app/share/studio-audio/parameters.json
  test -x /app/libexec/studio-subtitle
  test -x /app/bin/whisper-cli
  test "$(sha256sum /app/bin/whisper-cli | cut -d" " -f1)" = "$STUDIO_EXPECTED_WHISPER_SHA256"
  test -s /app/share/studio-subtitles/parameters.json
  test -s /app/share/studio-subtitles/ggml-small-q5_1.bin
  test -s /app/share/studio-subtitles/ggml-silero-v5.1.2.bin
  test "$(sha256sum /app/share/studio-subtitles/ggml-silero-v5.1.2.bin | cut -d" " -f1)" = 29940d98d42b91fbd05ce489f3ecf7c72f0a42f027e4875919a28fb4c04ea2cf
  test -s /app/share/licenses/studio-subtitles/SILERO_VAD_LICENSE.txt
  test -s /app/share/studio-background/models/pp_humansegv2_lite_portrait_static.onnx
  test "$(sha256sum /app/share/studio-background/models/pp_humansegv2_lite_portrait_static.onnx | cut -d" " -f1)" = 2047b95bea344af15575f4279d6fac44bb07a42e7d537cf8d3265f15fe95cc82
  test -s /app/share/licenses/studio-background/PADDLESEG_LICENSE.txt
  test -s /app/share/studio-text/catalog.json
  test "$(grep -c "    \"id\":" /app/share/studio-text/catalog.json)" -eq 100
  grep -Fq "\"version\": \"0.6.0\"" /app/share/studio-subtitles/parameters.json
  test ! -e /app/lib/debug
  for filter in frei0r.c0rners frei0r.card3d studio.camera studio.background frei0r.studiofx frei0r.studio_lens frei0r.studio_vintage studio.color frei0r.sunimo_text_studio; do
    melt -query "filter=$filter" 2>&1 | grep -Fq "identifier: $filter"
  done
  melt -query transition=frei0r.sunimo_transition 2>&1 | grep -Fq "identifier: frei0r.sunimo_transition"
  /app/libexec/studio-audio --help >/dev/null
  melt color:red out=29 -attach studio.camera mode=4 zoom=140 -attach studio.color studio_input_validated=1 exposure=.2 \
    -attach frei0r.card3d 0=0 1=.25 2=0 3=.16666666666666666 4=.68 5=.34 6=0 7=1 \
    -consumer avformat:/tmp/studio-installed.mkv vcodec=ffv1 an=1 real_time=-1
  test "$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 /tmp/studio-installed.mkv)" = 30
'
flatpak install --user --noninteractive --reinstall "$previous"
test "$(flatpak info --user --show-commit "$app")" = "$old_commit"
flatpak install --user --noninteractive --reinstall "$validation/install-test.flatpak"
test "$(flatpak info --user --show-commit "$app")" = "$new_commit"
flatpak uninstall --user --noninteractive "$app"
if flatpak info --user "$app" >/dev/null 2>&1; then echo 'Uninstall did not remove the test deployment.' >&2; exit 1; fi
unset FLATPAK_USER_DIR
test "$(flatpak info --show-commit org.kde.kdenlive 2>/dev/null || true)" = "$official_before"
test "$(flatpak info --user --show-commit "$app" 2>/dev/null || true)" = "$studio_before"
printf 'PASS: install, Studio resources, MLT export, upgrade, rollback, uninstall; existing editors unchanged.\nold=%s\nnew=%s\n' "$old_commit" "$new_commit"
