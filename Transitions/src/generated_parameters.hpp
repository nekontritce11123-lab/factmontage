#pragma once
#include <array>
#include <string_view>

namespace sunimo::parameters {
inline constexpr int progress = 0;
inline constexpr int style = 1;
inline constexpr int strength = 2;
inline constexpr int direction = 3;
inline constexpr int softness = 4;
inline constexpr int center = 5;
inline constexpr int quality = 6;
inline constexpr int parameterCount = 7;
inline constexpr int styleCount = 15;
inline constexpr int directionCount = 4;
inline constexpr double defaultStrength = .45;
inline constexpr double defaultSoftness = .35;
inline constexpr double defaultCenter = .5;
inline constexpr std::array<double, styleCount> styleCodes{{
    0, 0.0909090909091, 0.181818181818, 0.272727272727, 0.363636363636, 0.454545454545, 0.545454545455, 0.636363636364, 0.727272727273, 0.818181818182, 0.909090909091, 1, 0.0454545454545, 0.136363636364, 0.227272727273
}};
inline constexpr std::array<std::string_view, styleCount> styleNames{{
    "Двойное отдаление",
    "Проезд вперёд",
    "Мягкий взмах",
    "Плавный проезд",
    "Через расфокус",
    "Диагональное раскрытие",
    "Лёгкий поворот",
    "Живой наплыв",
    "Круговое раскрытие",
    "Мягкое разделение",
    "Наклон и зум",
    "Мягкая шторка",
    "Световой засвет",
    "Раскрытие по яркости",
    "Цифровой сдвиг"
}};
} // namespace sunimo::parameters
