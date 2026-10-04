# Сборка FactMontage

[English](BUILD.md) · [На главную](../README_RU.md)

Собирай в Linux x86-64 на файловой системе Linux, обычным пользователем. WSL2 подходит для разработки. Результаты сборки, кеши, загруженные исходники и модели хранятся вне Git. Сначала собирается контрольный редактор, затем модифицированный.

## Закреплённые входные данные

- Kdenlive 26.08.0 и манифест Flatpak: [Studio/upstream/sources.json](../Studio/upstream/sources.json).
- KDE Platform/SDK 6.10 и расширение LLVM 21 версии 25.08, с коммитами из этого файла.
- Дополнительные зависимости и модели: [Background/SOURCES.json](../Background/SOURCES.json), [Subtitles/SOURCES.json](../Subtitles/SOURCES.json).
- Тестовые зависимости Python: [test-requirements.txt](../Studio/scripts/test-requirements.txt).

Источники зависимостей открыты. Доступ к прежнему приватному репозиторию не требуется.

## Явная подготовка окружения

Установи средствами дистрибутива Git, Python 3, CMake, Ninja, компилятор C/C++, pkg-config, Flatpak и flatpak-builder. Подключи Flathub и установи runtime, SDK и LLVM в установку пользователя сборки:

```sh
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.kde.Platform//6.10 org.kde.Sdk//6.10 org.freedesktop.Sdk.Extension.llvm21//25.08
```

Для каждого из трёх ref выполни `flatpak update --user --commit=<коммит из sources.json> <ref>`. Сборка отказывает при другой ревизии SDK/runtime. Не обходи эту проверку.

Склонируй репозиторий на файловую систему Linux и явно загрузи официальный архив редактора:

```sh
git clone https://github.com/nekontritce11123-lab/factmontage.git
cd factmontage
python3 -B Studio/scripts/dev.py bootstrap --timeout 300
```

До открытия репозитория эта подготовительная копия требует доступа владельца. Перед стабильной публикацией ограничение должно быть снято.

Модель фона можно взять из материалов соответствующего выпуска либо экспортировать через [export_humanseg_onnx.py](../Background/scripts/export_humanseg_onnx.py). Нужны открытые исходники PaddleSeg `3c4db66de1d9d59d0628ed87590b6308a2f4aa2a`, PaddlePaddle 2.6.2 и Paddle2ONNX 1.3.1 в совместимом отдельном окружении Python. Ссылка и SHA256 архива весов указаны в `Background/SOURCES.json`; хеш извлечённого `.pdparams` проверяет экспортёр. Сам он ничего не загружает.

```sh
python Background/scripts/export_humanseg_onnx.py --paddleseg /path/to/PaddleSeg --weights /path/to/model.pdparams --output /path/outside/git/pp_humansegv2_lite_portrait_static.onnx
```

SHA256 ONNX: `2047b95bea344af15575f4279d6fac44bb07a42e7d537cf8d3265f15fe95cc82`.

## Проверки исходников

FAST работает без сети. Подготовка зависимостей и bootstrap выполняются отдельно.

```sh
python3 -B Studio/scripts/dev.py test all --jobs 2 --timeout 180
```

Для контрактов без компиляции добавь `--contracts-only`. Это не проверяет готовый редактор, GUI или экспорт. Команды интеграции и SMOKE описаны в [TESTING.md](TESTING.md).

## Сборка и упаковка

Выбери один постоянный каталог сборки на файловой системе Linux и укажи модель:

```sh
export STUDIO_BUILD_ROOT="$HOME/factmontage-build"
export STUDIO_BUILD_JOBS=2
export STUDIO_HUMANSEG_ONNX=/path/outside/git/pp_humansegv2_lite_portrait_static.onnx
export CCACHE_DIR="$STUDIO_BUILD_ROOT/ccache"
export CCACHE_MAXSIZE=2G
bash Studio/scripts/build.sh baseline
bash Studio/scripts/build.sh studio
bash Studio/scripts/check-sdk.sh
```

Контрольная сборка переиспользуется для тех же входных данных. Проверки SDK включают ABI, санитайзеры, движки, MLT, редактор и установку. В `STUDIO_PREVIOUS_BUNDLE` укажи предыдущий проверенный пакет для изолированной проверки установки, обновления и отката. Для первого публичного выпуска его предоставляет сопровождающий.

После автоматических проверок используй только **свободный** номер кандидата:

```sh
bash Studio/scripts/package.sh 1.2-rc14
```

Упаковка отказывает при занятом номере и изменении проверенных входных данных. В `$STUDIO_BUILD_ROOT/dist/<версия>` сохраняются пакет, архивы исходников, версии, протоколы и контрольные суммы. Физическая приёмка Steam Deck и интерактивные монтажные сценарии проверяются отдельно. Подготовка кандидата не разрешает стабильный выпуск.

В сохранённом WSL-окружении сопровождающего тяжёлые этапы запускаются через действующий ограничитель: 120 GiB для VHD, резерв 32 GiB на диске, два задания и кеш компилятора 2 GiB.
