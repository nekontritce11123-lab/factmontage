# Проверки разработки и выпуска

## Повседневные команды

Требуются Python 3.10+, CMake, Ninja, C++ compiler и зависимости выбранного ядра. FAST не устанавливает их скрыто. На Debian/Ubuntu подготовка окружения явно показана в workflow linux-fast.yml; другие дистрибутивы используют свои пакеты.

```bash
./dev list
./dev test audio
./dev test background
./dev test transitions
./dev test text
./dev test changed
./dev test changed --base origin/main
./dev test studio
./dev test all
./dev build text
```

`changed` без --base проверяет незакоммиченные/staged/untracked изменения. Чистая рабочая копия даёт NO CHANGES, не доказательство корректности предыдущего коммита. `--base` дополнительно проверяет committed range. Audio затрагивает Subtitles; `.beads/` и документация не запускают сборку движков. Правка overlay требует отдельной проверки Kdenlive и не запускает девять независимых сборок модулей: `test changed` сообщает `HOST INTEGRATION NOT RUN` с кодом 3, затем явно запускается `integration`. Для Git-проверок в WSL используйте рабочую копию на Linux-диске: на `/mnt/e` обход файлов Git превысил установленный лимит.

`build` компилирует только независимый native модуль; не запускает тесты, не собирает всю панель/Kdenlive и не устанавливает приложение. `test` запускает контракт, CMake/Ninja в постоянном `.build/dev/<module>` и CTest. `--contracts-only` исключает native build, это не native PASS. `--dry-run` ничего не исполняет и не создаёт отчёт.

При запуске из WSL над `/mnt/...` задавайте `--build-root` на Linux-диске: CMake может не создать служебные файлы в `.build/dev` на смонтированном Windows-диске. Для общего `test all` нужны также объявленные Python-зависимости и ZLIB; используйте подготовленный SDK с уже проверенными test wheels, без скрытой установки в FAST.

Параллелизм по умолчанию 2, лимит команды 180 s. Меняются явно через --jobs/--timeout. Журналы и JSON: `.build/reports/`; каждый сбой содержит исходный stderr, код возврата и SHA. Таймаут убивает process group. Нет повторных попыток до случайного зелёного статуса.

## Инкрементальная интеграция Kdenlive

Один раз подготовьте в закреплённом SDK baseline, Studio и `sdk-check` с заголовками; тяжёлая сборка остаётся отдельной явной операцией. Затем работайте с сохранённым build-каталогом на Linux-диске:

```bash
./dev integration --host-build "$HOME/video-studio/.flatpak-builder/build/kdenlive/_flatpak_build" --build-only
./dev integration --host-build "$HOME/video-studio/.flatpak-builder/build/kdenlive/_flatpak_build" --case 'Studio rejects malformed pause cuts before editing'
./dev integration --host-build "$HOME/video-studio/.flatpak-builder/build/kdenlive/_flatpak_build"
```

`integration` сверяет закреплённые исходники, контрольную сборку, SDK/runtime, флаги CMake и сохранённый бинарник; копирует в build-каталог только отличающиеся файлы интеграции. При неизменных входах приложение переиспользуется, Ninja не компилирует код. `--build-only` не объявляет тесты пройденными; `--case` помогает проверить один сценарий, а запуск без него выполняет весь `studioregressiontest`. Отчёты и журналы — `$HOME/video-studio/dev-host/reports/`. Ошибка теста остаётся FAIL, даже если сборка прошла. При отсутствии исходника, заголовков или закреплённого SDK команда завершается ошибкой без скрытой полной сборки. `./dev bootstrap` явно загружает закреплённый исходник; `./dev release 1.1-rc1` (только если номер свободен) запускает тяжёлую build/SDK/package цепочку для кандидата.

`bash Studio/scripts/check-built.sh sdk-check` проверяет уже подготовленный SDK без новой компиляции. Общий короткий проект соединяет девять движков: реальные кадры перехода, звук, видимую надпись и видимые субтитры. Для композиции субтитров используется фиксированный ответ распознавания. Дополнительно настоящий Whisper с обеими локальными моделями проверяется на чистом тоне: исходные временные метки, интервалы VAD и отказ worker создавать субтитры без речи. Это ограниченные контрольные входы, не полная приёмка качества ASR. Это сборочный smoke, не проверка установленного кандидата. Пользовательский Flatpak экспортируется без `lib/debug`; Debug-ref остаётся отдельно в локальном репозитории сборки.

В релизной цепочке Kdenlive собирается один раз. Проверка SDK готовит только зависимости с заголовками до модуля Kdenlive и запускает тесты на копии уже собранного бинарника; установка и пакетирование используют исходное собранное дерево.

## Отдельные проверяемые Qt/MLT границы

```bash
cmake -S Text -B .build/qt-text -G Ninja -DSTXT_BUILD_QT_COMPILER=ON
cmake --build .build/qt-text --parallel 2
ctest --test-dir .build/qt-text -R text-qt-compiler --output-on-failure

cmake -S Studio/tests/qt -B .build/qt-resources -G Ninja
cmake --build .build/qt-resources --parallel 2
ctest --test-dir .build/qt-resources --output-on-failure

cmake -S Background -B .build/mlt-background -G Ninja -DSBG_WITH_MLT=ON -DSBG_WITH_ONNX=OFF
cmake --build .build/mlt-background --target sbg-mlt-callback-test --parallel 2
ctest --test-dir .build/mlt-background -R mlt-background-callback --output-on-failure
```

Text Qt tests используют offscreen. Нужны Qt6 Core/Gui и официальный MLT framework для соответствующих целей. Эти проверки запускает отдельный workflow linux-boundaries.yml, когда меняются их входы. Он дополнительно проверяет, что возврат прежнего ошибочного callback-кода ломает регрессионный тест, а не остаётся зелёным.

Это реальные библиотеки Qt/MLT, но НЕ весь закреплённый Kdenlive/SDK. Компиляция Qt → STXT не доказывает готовность текстовой панели.

## Критерии продуктовой приёмки

Audio: два применения на одинаковые источники → одна managed track, один result, один managed mute на источник; то же после Save/Reopen. Пользовательские volume не затронуты, неудача откатывается.

Background: известная маска и цветной нижний слой; foreground виден, фон открывает нижний слой. Затем настоящий анализатор, trim/seek/reduced preview/save/reopen. Диагностический кадр означает ошибку удаления фона, а не успешный результат. Оригинальный фон не раскрывать в экспорт.

Transitions: узнаваемые A/B; кадры до, несколько внутри, после. Ожидание зависит от стиля. Проверять controller/mix/сохранение/рендер, не только return true.

Text: кириллица, переносы, shaping, STXT compatibility, deterministic seek; редактирование текста, Undo, Save/Reopen, Export. Descriptor/библиотека/CLI отдельно не завершают продуктовую функцию.

Панель: 1280×800 при масштабе 100%, 125% и 150%; доступ к разделам, основной кнопке и всем настройкам при прокрутке. При 150% используйте штатную компактную раскладку: в меню «Вид» отключите «Монитор клипа», «Библиотека», «Редактор речи» и после выбора клипа штатную панель эффектов/композиций (Effect/Composition Stack), сохранив монитор проекта и панель FactMontage. Стандартная раскладка самого Kdenlive с двумя мониторами может требовать окно больше экрана. Масштаб и шрифт ради успешной проверки не уменьшать; раскладку и фактические размеры записывать в отчёте. Отдельно проверить панель шириной 300 и 440 физических пикселей.

SMOKE — один маленький проект с основными сценариями. RELEASE — только конкретный RC, обновление окружения или релизной цепочки: pinned SDK/baseline/ASan/UBSan/ABI/Flatpak/install/update/rollback, GUI и Linux/Deck acceptance. Исторические PASS не переносить на другую сборку.
