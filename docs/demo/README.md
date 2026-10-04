# Публичное демо FactMontage

Демо создаёт один настоящий проект Kdenlive с девятью последовательными фрагментами, синей нижней дорожкой, оригинальным тоном и отдельной нативной надписью. Восемь разделов применяются через существующую панель. Файлы проекта и снимки создаются вне исходников.

Материалы собственные: процедурная геометрия, хромакей, синтезированный тон и короткая надпись. Фотографий, частных QA-видео, записанной музыки и загрузок нет. Описание происхождения и права находятся в `SOURCES.json`; генератор сохраняет реальные SHA256 и команды в `generated-sources.json`. Хромакей демонстрирует только удаление однотонного фона. Human segmentation здесь не проверяется.

## Подключение в существующую QA-сборку

Скопировать `Studio/tests/public_demo.hpp` рядом с `studioregressiontest.cpp` в подготовленных исходниках Kdenlive и включить его **в конце** `Studio/tests/studioregressiontest.cpp`:

```cpp
#include "public_demo.hpp"
```

В `Studio/scripts/prepare.py` добавить копирование header рядом с основным тестом и отдельный file source манифеста с `dest: tests`. В `Studio/scripts/host_dev.py` добавить `files['tests/public_demo.hpp'] = root / 'Studio/tests/public_demo.hpp'`. В `Studio/scripts/dev.py` учитывать изменение этого header среди изменений тестового runner. Эти файлы helper не меняет. Дополнительные библиотеки/новый target не нужны: fixture использует зависимости уже существующего `studioregressiontest`, включая KWidgetsAddons и native GUI.

## Подготовка материалов

Нужны Python 3 и ffmpeg с `libx264`, `zoompan` и `lavfi`. Генератор использует стандартную библиотеку Python; pip и сеть не нужны. Каталог результата должен быть пустым. Повторный запуск не перезаписывает проект или доказательства.

```sh
python3 docs/demo/create_demo.py /tmp/factmontage-public-demo --ffmpeg /app/bin/ffmpeg
```

Видео: SDR BT.709, yuv420p, 1920×1080, 60 fps, по 360 кадров; WAV: PCM 48 kHz, шесть секунд. Для обычной host-сборки можно использовать `--ffmpeg ffmpeg`. Команда не собирает и не устанавливает приложение.

## Настоящий тёмный интерфейс

Нужна уже подготовленная QA-сборка FactMontage с актуальными движками, ресурсами панели, `studio-audio`, `ffmpeg`, `melt`, нативным Text и штатной цветовой схемой Breeze Dark. Runner запускать отдельным процессом и только с данным тегом. На Linux нужен X11: настоящий экран или Xvfb размером минимум 1920×1200; Qt xcb и рабочий OpenGL/software Mesa. Offscreen не подходит для снимка окна с нативным монитором.

Пример для уже запущенного QA-окружения с установленными путями MLT/Qt/XDG:

```sh
export FACTMONTAGE_DEMO_ROOT=/tmp/factmontage-public-demo
export QT_QPA_PLATFORM=xcb
export QT_SCALE_FACTOR=1
export STUDIO_PREFIX=/app
export STUDIO_QA_PREFIX=/app
timeout 240s xvfb-run -a -s '-screen 0 1920x1200x24' \
  /path/to/studioregressiontest '[FactMontagePublication]' --reporter compact
```

`/path/to/studioregressiontest` заменить настоящим runner этой сборки. Для Flatpak запускать команду внутри существующего build/runtime окружения и сохранить тот же внешний writable каталог результата. Не устанавливать отдельный редактор и не менять официальный Kdenlive. Если runner требует дополнительные XDG/MLT-переменные, использовать его существующую QA-обвязку. Нужные пакеты Xvfb/Breeze/Qt должны быть подготовлены явно заранее; helper ничего не скачивает.

Fixture активирует Breeze Dark через настоящее меню тем и проверяет тёмную палитру. Через native actions открывает Project Monitor и правую панель FactMontage. Применяет Карточки, Камеру, хромакей, переход «Через расфокус», «Глубокие края», Цвет, обработку громкости и Text. Сохраняет `FactMontage-demo.kdenlive`, реальные managed WAV/STXT и `capture-evidence.json`.

В `screenshots/` должны появиться `workspace-dark.png`, `cards-dark.png`, `camera-dark.png`, `background-dark.png`, `transitions-dark.png`, `effects-dark.png`, `colour-dark.png`, `audio-dark.png`, `text-dark.png`. Снимок захватывается с настоящего X11-окна, включая поверхность монитора; `QWidget::grab` может пропустить native QQuickView. Обработка снимков, вставка видеокадра и синтетический UI не используются.

## Проверка перед публикацией

Открыть каждый снимок и подтвердить: настоящий тёмный интерфейс; монитор с видимым материалом; таймлайн; правая общая панель нужного раздела. У фона должны быть видны оранжевая фигура, белая полоса и синий нижний слой. У перехода должен быть видимый промежуточный кадр двух источников. У звука доказательство применения видно по настоящему результату на аудиодорожке; тон не является примером реставрации человеческого голоса. Снять снимки заново штатным захватом при пустом/чёрном native monitor; никогда не подставлять туда кадр.

Успех fixture подтверждает UI-действия, появление ожидаемых экземпляров в модели, сохранение и позицию монитора. Он не заменяет визуальную проверку изображений, Save/Reopen, Undo/Redo, экспорт, Flatpak install/update/rollback или Steam Deck acceptance. До отдельного запуска helper имеет статус NOT RUN. Для доказательств фиксировать SHA исходников, версию ffmpeg/MLT и SHA256 runner и новых артефактов.
