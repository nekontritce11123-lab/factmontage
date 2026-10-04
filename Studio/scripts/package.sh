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
python3 "$root/Studio/scripts/verification.py" verify tests "$work"
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
cp "$root/Background/SOURCES.json" "$stage/BACKGROUND_SOURCES.json"
cp "$root/Subtitles/SOURCES.json" "$stage/SUBTITLE_SOURCES.json"
cp "$root/LICENSE" "$root/THIRD_PARTY_NOTICES.md" "$stage/"
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
cat > "$stage/DECK_CHECKLIST_RU.md" <<'EOF'
# Приёмка кандидата на Steam Deck OLED

Работайте с копией проекта SDR Rec.709, 1920×1080, 60 кадров/с. Предпросмотр 1:2 или 1:4 допустим; экспорт остаётся 1080p60.

1. Установите пакет по `INSTALL_RU.txt` и запустите FactMontage.
2. Откройте клипы с внутреннего диска и microSD; один путь сделайте с пробелами и кириллицей.
3. Проверьте три вида карточек и скругление 0%, 4%, 10% и 20%. На прозрачном PNG сравните «С фоном» и «Только обводка», затем тень 0%, 35% и 100%.
4. Создайте карточку из видео: появление и готовые позиции должны работать. Перетащите карточку в мониторе проекта и проверьте синхронные ползунки X/Y и отмену одним шагом.
5. Добавьте камеру перед карточкой, откройте кадрирование в мониторе и проверьте цель и проезд.
6. В «Камера → Трекинг лица» поставьте точку на лице в стоп-кадре внутри панели, задайте приближение и нажмите «Применить трекинг». Проверьте отмену анализа, перемотку, потерю трека и экспорт.
7. Для всей последовательности включите «Зум по кругу», проверьте цикл 2, 6 и 12 секунд.
8. Сохраните свой шаблон, примените его к одному клипу, затем к нескольким и отмените одной командой.
9. Разрежьте клип с камерой: кадр сразу после разреза не должен прыгать. Отмените и повторите разрезание.
10. Сохраните, закройте, снова откройте проект и экспортируйте короткий фрагмент.
11. В «Эффектах» проверьте поиск, категории, избранное, применение к нескольким клипам и полную отмену.
12. В «Переходах» выберите два соседних клипа V1, проверьте все 15 видов, первый/последний кадры, длительность, перемотку, удаление и Ctrl+Z/Ctrl+Shift+Z. Для каждого вида включите звук, проверьте уровень по умолчанию, выключение, смену стиля, Save/Reopen и экспорт.
13. Повторите с двумя связанными AV-клипами и с видео без звука; нехватка исходных кадров и чужой переход должны блокировать действие без частичных изменений.
14. Сохраните и снова откройте проект с переходом; экспорт должен совпасть с монитором. Проверьте прозрачность, обрезку, перемещение пары и удаление одного клипа.
15. На мониторе Full HD проверьте масштаб 100%, 125%, 150% и ширину панели 300, 440 и 800 px; отдельно проверьте встроенный экран 1280×800.
16. Сравните один и два слоя при качестве предпросмотра 1:1, 1:2 и 1:4 после прогрева.
17. Закройте панель и убедитесь, что воспроизведение не становится тяжелее из-за превью.
18. Удалите студию по `INSTALL_RU.txt`; официальный Kdenlive и проекты должны остаться.
19. В «Фон → Однотонный» выберите пипеткой зелёный и синий фон на фото и видео; проверьте прозрачность и Ctrl+Z.
20. В «Фон → Человек» проверьте размытие, прозрачность и заливку, отмените анализ и убедитесь, что старая маска осталась рабочей.
21. Сохраните, разрежьте и обрежьте клип с маской, повторно откройте и экспортируйте; Reverse и скорость не 100% должны блокироваться понятным сообщением.

22. В «Цвете» проверьте восемь стилей и ручную SDR-коррекцию, прозрачность, сохранение и экспорт.
23. В «Звуке» проверьте голос, общую громкость, роли голоса/музыки и сокращение пауз вместе с субтитрами и маркерами. Проверьте отмену, повторную обработку и восстановление WAV.
24. Проверьте совместные цепочки всех разделов, старый проект, отсутствующие ресурсы и нехватку места на отдельном тестовом носителе.
25. Поработайте 30 минут. Запишите время кадра, пропуски, память, время анализа и экспорта для одного и двух слоёв; сравните с контрольной сборкой на этом же Deck. Цель — плавные 60 кадров/с при выбранном качестве предпросмотра.
26. Проверьте обновление с предыдущего кандидата и откат к его сохранённому Flatpak, затем удаление студии. Официальный org.kde.kdenlive остаётся отдельным приложением.
27. В «Эффекты → Глазок» проверьте круг, овал и прямоугольное окно, тёмный край, размытие края, центр и сохранение проекта.
28. В «Эффекты → Старая камера» проверьте VHS, Hi8, Super 8, CRT и веб-камеру; перемотка и повторный экспорт одного кадра должны быть воспроизводимыми.
29. В «Субтитрах» выберите клип с русской речью, примените каждый шаблон, отмените анализ и проверьте правку текста на дорожке после сохранения/повторного открытия.
30. В разделе «Текст» добавьте надпись над видео, измените её, обрежьте и разрежьте клип; проверьте Undo/Redo, Save/Reopen и экспорт. Сохраните проект в папку с пробелами и кириллицей через «Сохранить как», уберите исходную папку из пути доступа и убедитесь, что надпись видна после открытия копии.

Верните: версии SteamOS и runtime, результат каждого шага, показатели скорости и
снимки всех разделов. До подтверждения этой приёмки кандидат экспериментальный.
EOF
(cd "$stage" && find . -type f ! -name SHA256SUMS.txt -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS.txt)
mkdir "$out" # Atomic reservation: never write into an existing candidate.
cp -R "$stage"/. "$out/"
echo "Candidate prepared in $out"
