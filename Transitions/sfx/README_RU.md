# Звуки переходов 1.2

Это 16 подготовленных WAV для 15 переходов. Исходные записи и эффекты взяты из каталогов [Freesound](https://freesound.org/), [Kenney](https://kenney.nl/assets) и [OpenGameArt](https://opengameart.org/); каждый из перечисленных ниже источников опубликован под [CC0](https://creativecommons.org/publicdomain/zero/1.0/). Благодарим авторов, хотя атрибуция для CC0 не обязательна.

Звуки различаются по материалу и характеру. Для стилей с движением применена стереопанорама, для разделения сведены два разных записанных взмаха, для наклона и зума — два кинематографичных эффекта. Файлы обрезаны до 0,28–0,62 с, сведены к 48 кГц, 16 бит, стерео и выровнены по RMS 0,05 с мягким ограничением пика до 0,4. При громкости 50% WAV воспроизводится без дополнительного усиления; галочка звука выключена по умолчанию.

В панели отметьте «Звук перехода», затем в «Выбрать звук» оставьте автоматический вариант или выберите любой из 16 встроенных звуков. Кнопка «Добавить свой звук…» открывает аудиофайл и добавляет его в медиатеку проекта; повторный выбор того же файла не создаёт второй записи. Исходный пользовательский файл должен оставаться доступным проекту, как и другие импортированные медиа Kdenlive. Выбор звука сохраняется с переходом и в пользовательском шаблоне; при применении шаблона в другом проекте собственный файл должен быть доступен по сохранённому пути.

| № | Переход | Звук и источник |
|---:|---|---|
| 0 | Двойное отдаление | Нарастающее обратное движение — [Backwards Whoosh, SlavicMagic](https://freesound.org/people/SlavicMagic/sounds/446010/) |
| 1 | Проезд вперёд | Плотный записанный проход — [Boomy Whoosh, Joao_Janz](https://freesound.org/people/Joao_Janz/sounds/485241/) |
| 2 | Мягкий взмах | Взмах бамбуковой палки со стереодвижением; отдельный WAV для противоположного направления — [Whoosh, qubodup](https://freesound.org/people/qubodup/sounds/60013/) |
| 3 | Плавный проезд | Протяжённый проход ветра — [wind whoosh loop, SketchMan3](https://opengameart.org/content/wind-whoosh-loop) |
| 4 | Через расфокус | Мягкое затуманенное движение с коротким акцентом — [Soft Synth Whoosh and Impact, doudar41](https://freesound.org/people/doudar41/sounds/728523/) |
| 5 | Диагональное раскрытие | Быстрый кинематографичный проход со стереодвижением — [Quick Movement C, EpicSoundEffects](https://freesound.org/people/EpicSoundEffects/sounds/475896/) |
| 6 | Лёгкий поворот | Вращающийся механизм — [engineCircular_000 из Sci-fi Sounds, Kenney](https://kenney.nl/assets/sci-fi-sounds) |
| 7 | Живой наплыв | Короткая естественная волна — [Water Waves, transitking](https://opengameart.org/content/water-waves) |
| 8 | Круговое раскрытие | Расширяющееся энергетическое поле — [forceField_000 из Sci-fi Sounds, Kenney](https://kenney.nl/assets/sci-fi-sounds) |
| 9 | Мягкое разделение | Два встречных записанных взмаха, swish-1 и swish-8 — [Swishes Sound Pack, artisticdude](https://opengameart.org/content/swishes-sound-pack) |
| 10 | Наклон и зум | Кинематографичный проход и низкий удар — [Cinematic Woosh, AudioPapkin](https://freesound.org/people/AudioPapkin/sounds/648729/) и [Sonic Punch, EpicSoundEffects](https://freesound.org/people/EpicSoundEffects/sounds/475887/) |
| 11 | Мягкая шторка | Настоящее движение ткани, Cloth_05 — [202 More Sound Effects, OwlishMedia](https://opengameart.org/content/202-more-sound-effects) |
| 12 | Световой засвет | Короткий стеклянный световой акцент — [glass_001 из Interface Sounds, Kenney](https://kenney.nl/assets/interface-sounds) |
| 13 | Раскрытие по яркости | Короткий восходящий мерцающий тон — [Quick Shimmer Wait and Rise, PLukx](https://freesound.org/people/PLukx/sounds/703284/) |
| 14 | Цифровой сдвиг | Фрагмент цифрового сбоя — [Glitch, AmicaSys](https://freesound.org/people/AmicaSys/sounds/332711/) |

Для Freesound взяты публичные качественные MP3-предпросмотры звуков с указанных страниц CC0; архивы Kenney и OpenGameArt скачаны с указанных страниц. Исходные файлы не нужны для обычной сборки и не скачиваются тестами. Чтобы пересобрать WAV, сохраните исходники под именами из `../scripts/import_sfx.py` и запустите скрипт с `--sources`, `--ffmpeg` и `--output`. Скрипт не обращается к сети.
