#!/usr/bin/env bash
# Create an installable unsigned local bundle only after the Studio build checks pass.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="${STUDIO_BUILD_ROOT:-$HOME/video-studio}"
version="${1:?Usage: package.sh NEXT_FREE_VERSION}"
[[ "$version" =~ ^[0-9]+\.[0-9]+(-rc[0-9]+)?$ ]] || { echo 'Invalid release version.' >&2; exit 2; }
dist="${STUDIO_DIST_DIR:-$work/dist}"
out="$dist/$version"
test ! -e "$out" || { echo "Candidate already exists: $out" >&2; exit 1; }
test -x "$work/studio/files/bin/kdenlive" || { echo 'Build Studio first.' >&2; exit 1; }
test -f "$work/studio/metadata.debuginfo" -a -d "$work/studio/files/lib/debug" || { echo 'Separate debug files are required.' >&2; exit 1; }
python3 "$root/Studio/scripts/verification.py" verify tests "$work" --upstream-archive "$root/Studio/upstream/kdenlive-26.08.0.tar.xz"
mkdir -p "$dist"
stage="$(mktemp -d "$dist/.package-$version.XXXXXX")"
trap 'rm -rf -- "$stage"' EXIT
flatpak build-export --no-update-summary --exclude=lib/debug "$work/repository" "$work/studio" experimental
flatpak build-export --no-update-summary --runtime --files=files/lib/debug --metadata=metadata.debuginfo "$work/repository" "$work/studio" experimental
flatpak build-bundle "$work/repository" "$stage/FactMontage-$version.flatpak" local.VideoStudio.Kdenlive experimental
ostree --repo="$work/repository" rev-parse runtime/local.VideoStudio.Kdenlive.Debug/x86_64/experimental > "$stage/DEBUG_REF.txt"
cp "$work/studio-input/local.VideoStudio.Kdenlive.json" "$stage/"
cp "$work/studio-input/card3d-source.tar.gz" "$work/studio-input/studio-camera-source.tar.gz" "$work/studio-input/studiofx-source.tar.gz" "$work/studio-input/studio-transitions-source.tar.gz" "$work/studio-input/studio-color-source.tar.gz" "$work/studio-input/studio-audio-source.tar.gz" "$work/studio-input/studio-subtitles-source.tar.gz" "$work/studio-input/studio-background-source.tar.gz" "$work/studio-input/studio-text-source.tar.gz" "$stage/"
cp "$work/studio-input/studio-changes-source.tar.gz" "$stage/"
cp "$root/Studio/upstream/kdenlive-26.08.0.tar.xz" "$stage/"
cp "$work/studio/files/share/studio-background/models/pp_humansegv2_lite_portrait_static.onnx" "$stage/"
tar -xOf "$work/studio-input/studio-background-source.tar.gz" SOURCES.json > "$stage/BACKGROUND_SOURCES.json"
tar -xOf "$work/studio-input/studio-subtitles-source.tar.gz" SOURCES.json > "$stage/SUBTITLE_SOURCES.json"
tar -xOf "$work/studio-input/studio-changes-source.tar.gz" LICENSE > "$stage/LICENSE"
tar -xOf "$work/studio-input/studio-changes-source.tar.gz" THIRD_PARTY_NOTICES.md > "$stage/THIRD_PARTY_NOTICES.md"
cp "$work/tests-verified.json" "$stage/VERIFICATION.json"
cp -R "$work/test-results" "$stage/TEST_RESULTS"
tar -xOf "$work/studio-input/studio-changes-source.tar.gz" Studio/upstream/sources.json > "$stage/VERSIONS.json"
cat > "$stage/INSTALL_RU.txt" <<EOF
FactMontage $version устанавливается рядом с официальным Kdenlive.
Сначала подключите Flathub и установите runtime:
  flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
  flatpak install --user flathub org.kde.Platform//6.10

Проверьте пакет и установите его:
  sha256sum --ignore-missing --check SHA256SUMS.txt
  flatpak install --user ./FactMontage-$version.flatpak

Запуск:
  flatpak run local.VideoStudio.Kdenlive

Удаление только студии (проекты не удаляются):
  flatpak uninstall --user local.VideoStudio.Kdenlive

До завершения приёмки на Steam Deck работайте с копиями проектов.

Отладочные символы сохранены отдельно в репозитории сборки; обычному пользователю они не нужны.
EOF
cat > "$stage/INSTALL_EN.txt" <<EOF
FactMontage $version is installed alongside official Kdenlive.
Set up Flathub and the runtime:
  flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
  flatpak install --user flathub org.kde.Platform//6.10

Verify the download and install:
  sha256sum --ignore-missing --check SHA256SUMS.txt
  flatpak install --user ./FactMontage-$version.flatpak

Run:
  flatpak run local.VideoStudio.Kdenlive

Update or roll back: close the editor, verify the chosen package, then use:
  flatpak install --user --reinstall ./FactMontage-$version.flatpak
For rollback use the previous verified package filename.

Remove only FactMontage:
  flatpak uninstall --user local.VideoStudio.Kdenlive

The panel is currently in Russian. Use project copies until release acceptance.
EOF
for name in ACCEPTANCE.md ACCEPTANCE_RU.md; do
  tar -xOf "$work/studio-input/studio-changes-source.tar.gz" "Studio/release/$name" > "$stage/$name"
done
cp "$stage/ACCEPTANCE_RU.md" "$stage/DECK_CHECKLIST_RU.md"
(cd "$stage" && find . -type f ! -name SHA256SUMS.txt -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS.txt)
mkdir "$out" # Atomic reservation: never write into an existing candidate.
cp -R "$stage"/. "$out/"
echo "Candidate prepared in $out"
