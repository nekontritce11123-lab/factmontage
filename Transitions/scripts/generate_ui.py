#!/usr/bin/env python3
"""Generate the single Kdenlive transition contract from one table."""
from pathlib import Path
import argparse
import json
import xml.etree.ElementTree as E

ROOT = Path(__file__).resolve().parents[1]
STYLES = [
    ("Двойное отдаление", True, False, False),
    ("Проезд вперёд", True, False, False),
    ("Мягкий взмах", False, True, False),
    ("Плавный проезд", False, True, True),
    ("Через расфокус", False, False, True),
    ("Диагональное раскрытие", False, True, True),
    ("Лёгкий поворот", True, False, False),
    ("Живой наплыв", True, True, False),
    ("Круговое раскрытие", True, False, True),
    ("Мягкое разделение", False, True, True),
    ("Наклон и зум", True, False, False),
    ("Мягкая шторка", False, True, True),
    ("Световой засвет", True, False, True),
    ("Раскрытие по яркости", False, False, True),
    ("Цифровой сдвиг", True, True, False),
]
DIRECTIONS = ["Влево", "Вправо", "Вверх", "Вниз"]
STYLE_CODES = [index / 11 for index in range(12)] + [1 / 22, 3 / 22, 5 / 22]


def number(value):
    return format(value, ".12g")


def normalized(count):
    return ";".join(number(index / (count - 1)) for index in range(count))


def xml_text():
    root = E.Element("transition", tag="frei0r.sunimo_transition", id="studio_transition",
                     type="videotransition", LC_NUMERIC="C")
    E.SubElement(root, "name").text = "FactMontage: Переходы"
    E.SubElement(root, "description").text = "Пятнадцать переходов для двух соседних видеоклипов."
    E.SubElement(root, "author").text = "SUNIMO Motion contributors"

    progress = E.SubElement(root, "parameter", name="0", type="animated", default="0=0;%out=1",
                            value="0=0;%out=1", min="0", max="1", factor="1", decimals="3")
    E.SubElement(progress, "name").text = "Ход перехода"
    style = E.SubElement(root, "parameter", name="1", type="list", default="0", value="0",
                         paramlist=";".join(number(code) for code in STYLE_CODES))
    E.SubElement(style, "name").text = "Стиль"
    E.SubElement(style, "paramlistdisplay").text = ";".join(item[0] for item in STYLES)
    index, label, default = 2, "Сила", .45
    parameter = E.SubElement(root, "parameter", name=str(index), type="constant",
                             default=number(default), value=number(default), min="0", max="100",
                             factor="100", decimals="0")
    E.SubElement(parameter, "name").text = label
    direction = E.SubElement(root, "parameter", name="3", type="list", default="0", value="0",
                             paramlist=normalized(len(DIRECTIONS)))
    E.SubElement(direction, "name").text = "Направление"
    E.SubElement(direction, "paramlistdisplay").text = ";".join(DIRECTIONS)
    for index, label, default in ((4, "Мягкость", .35),):
        parameter = E.SubElement(root, "parameter", name=str(index), type="constant",
                                 default=number(default), value=number(default), min="0", max="100",
                                 factor="100", decimals="0")
        E.SubElement(parameter, "name").text = label
    E.SubElement(root, "parameter", name="5", type="fixed", default="0.5 0.5", value="0.5 0.5")
    E.SubElement(root, "parameter", name="6", type="fixed", default="0", value="0")
    E.SubElement(root, "parameter", name="studio:audio_dip", type="fixed", default="0", value="0")
    E.SubElement(root, "parameter", name="studio:sfx_enabled", type="fixed", default="0", value="0")
    E.SubElement(root, "parameter", name="studio:sfx_level", type="fixed", default="0.5", value="0.5")
    E.SubElement(root, "parameter", name="studio:sfx_source", type="fixed", default="", value="")
    E.SubElement(root, "parameter", name="studio:sfx_id", type="fixed", default="", value="")
    E.indent(root, space="  ")
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + E.tostring(root, encoding="unicode") + "\n"


def panel_text():
    styles = []
    for index, (name, strength, direction, softness) in enumerate(STYLES):
        styles.append(dict(value=index, code=STYLE_CODES[index], name=name,
                           preview=f"transition_{index:02d}", animated=True,
                           strength=strength, direction=direction, softness=softness))
    sounds = [dict(value="", name="Автоматически — звук этого перехода")]
    for index, (name, *_flags) in enumerate(STYLES):
        sounds.append(dict(value=f"transition_{index:02d}.wav", name=name))
        if index == 2:
            sounds.append(dict(value="transition_02_right.wav", name="Мягкий взмах — обратное направление"))
    controls = [
        dict(key="style", parameter="1", label="Готовый переход", kind="choice", lo=0, hi=14,
             value=0, normalized=True, options=styles),
        dict(key="duration", label="Длительность", kind="number", lo=.08, hi=3, step=.05,
             value=.65, suffix=" с", decimals=2),
        dict(key="direction", parameter="3", label="Направление", kind="choice", lo=0, hi=3,
             value=0, normalized=True, options=[dict(value=i, name=name) for i, name in enumerate(DIRECTIONS)],
             visible_styles=[2, 3, 5, 7, 9, 11]),
        dict(key="strength", parameter="2", label="Сила", kind="number", lo=0, hi=100, value=45,
             suffix="%", decimals=0, normalized=True, visible_styles=[0, 1, 6, 7, 8, 10]),
        dict(key="softness", parameter="4", label="Мягкость", kind="number", lo=0, hi=100, value=35,
             suffix="%", decimals=0, normalized=True, visible_styles=[3, 4, 5, 8, 9, 11]),
        dict(key="audio_dip", parameter="studio:audio_dip", label="Приглушение звука на стыке", kind="number",
             lo=0, hi=100, value=25, suffix="%", decimals=0, normalized=True,
             tip="0% — обычное плавное смешивание; 100% — дополнительно до −12 дБ в середине стыка."),
        dict(key="sfx_enabled", parameter="studio:sfx_enabled", label="Звук перехода", kind="bool", lo=0, hi=1, value=0,
             tip="Добавить звук к переходу."),
        dict(key="sfx_source", parameter="studio:sfx_source", label="Выбрать звук", kind="sound", options=sounds,
             tip="Любой встроенный звук или собственный аудиофайл."),
        dict(key="sfx_level", parameter="studio:sfx_level", label="Громкость звука", kind="number",
             lo=0, hi=100, value=50, suffix="%", decimals=0, normalized=True),
    ]
    groups = [dict(key="ready", label="Готовые", keys=["style"], collapsed=False),
              dict(key="settings", label="Настройка перехода",
                   keys=["duration", "direction", "strength", "softness", "audio_dip", "sfx_enabled", "sfx_source", "sfx_level"], collapsed=True)]
    return json.dumps(dict(version=2, effect="studio_transition", asset="studio_transition", service="frei0r.sunimo_transition",
                           default_duration=.65, groups=groups, controls=controls),
                      ensure_ascii=False, indent=2) + "\n"


def previews_text():
    return json.dumps(dict(version=1, width=144, height=81,
                           items=[dict(style=i, code=STYLE_CODES[i], name=item[0], png=f"transition_{i:02d}.png",
                                       gif=f"transition_{i:02d}.gif") for i, item in enumerate(STYLES)]),
                      ensure_ascii=False, indent=2) + "\n"


def header_text():
    names = ",\n    ".join(json.dumps(item[0], ensure_ascii=False) for item in STYLES)
    return f'''#pragma once
#include <array>
#include <string_view>

namespace sunimo::parameters {{
inline constexpr int progress = 0;
inline constexpr int style = 1;
inline constexpr int strength = 2;
inline constexpr int direction = 3;
inline constexpr int softness = 4;
inline constexpr int center = 5;
inline constexpr int quality = 6;
inline constexpr int parameterCount = 7;
inline constexpr int styleCount = {len(STYLES)};
inline constexpr int directionCount = {len(DIRECTIONS)};
inline constexpr double defaultStrength = .45;
inline constexpr double defaultSoftness = .35;
inline constexpr double defaultCenter = .5;
inline constexpr std::array<double, styleCount> styleCodes{{{{
    {", ".join(number(code) for code in STYLE_CODES)}
}}}};
inline constexpr std::array<std::string_view, styleCount> styleNames{{{{
    {names}
}}}};
}} // namespace sunimo::parameters
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    outputs = {
        ROOT / "src/generated_parameters.hpp": header_text(),
        ROOT / "kdenlive/studio_transition.xml": xml_text(),
        ROOT / "kdenlive/studio_transition.json": panel_text(),
        ROOT / "kdenlive/transition_previews.json": previews_text(),
    }
    if args.check:
        stale = [str(path) for path, data in outputs.items()
                 if not path.exists() or path.read_text(encoding="utf-8") != data]
        if stale:
            raise SystemExit("Generated files are stale: " + ", ".join(stale))
        return
    for path, data in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(data, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
