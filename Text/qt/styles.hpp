// SPDX-License-Identifier: MIT
#pragma once
#include <QJsonObject>
#include <array>

namespace SunimoTextQt {
struct Style {
    int id;
    const char *family;
    const char *name;
    int entrance, life, group, order;
    double lag, amount, speed, inDuration, outDuration;
};

// Curated combinations of the existing, stable 1..100 entrance and life recipes.
inline constexpr std::array<Style, 18> styles{{
    {1, "Спокойные", "Мягкое проявление", 1, 1, 3, 0, 0, .40, .8, .8, .8},
    {2, "Спокойные", "Парение", 2, 2, 0, 0, .20, 1, 1, .8, .8},
    {3, "Спокойные", "Спокойная подпись", 2, 91, 3, 0, 0, .55, .7, .9, .9},
    {4, "Упругие", "Пружина", 21, 21, 0, 0, .35, 1, 1.1, .7, .7},
    {5, "Упругие", "Желе", 27, 27, 0, 0, .35, 1, 1.2, .7, .7},
    {6, "Упругие", "Пульс", 22, 22, 1, 0, .25, 1, 1.2, .6, .6},
    {7, "Волны", "Волна", 72, 72, 0, 0, .30, 1, 1.2, .8, .8},
    {8, "Волны", "Змейка", 47, 47, 0, 0, .32, 1, 1.2, .9, .9},
    {9, "Волны", "Танец слов", 77, 77, 1, 0, .28, 1, 1.1, .8, .8},
    {10, "Печатные", "Машинка", 61, 61, 0, 0, .65, .45, 1.2, 1.2, .6},
    {11, "Печатные", "Мягкая печать", 62, 62, 0, 0, .58, .55, 1, 1.2, .7},
    {12, "Печатные", "Набор словами", 93, 93, 1, 0, .55, .50, 1, 1.1, .7},
    {13, "Цифровые", "Декодирование", 63, 63, 0, 4, .42, .80, 1.3, 1, .7},
    {14, "Цифровые", "Глитч", 64, 64, 0, 0, .30, .90, 1.3, .8, .7},
    {15, "Цифровые", "Сканирование", 19, 19, 1, 0, .28, .80, 1, .9, .8},
    {16, "Кинематографичные", "Фокус", 52, 52, 1, 0, .24, .70, .8, .9, .9},
    {17, "Кинематографичные", "Наезд", 53, 53, 3, 0, 0, .65, .7, 1, 1},
    {18, "Кинематографичные", "Мягкое эхо", 54, 54, 1, 0, .25, .70, .9, .9, .9},
}};

inline QJsonObject applyStyle(QJsonObject settings, const Style &style)
{
    settings.insert(QStringLiteral("styleId"), style.id);
    settings.insert(QStringLiteral("inPreset"), style.entrance);
    settings.insert(QStringLiteral("outPreset"), -1);
    settings.insert(QStringLiteral("lifeSource"), style.life);
    settings.insert(QStringLiteral("lifeAmount"), style.amount);
    settings.insert(QStringLiteral("lifeSpeed"), style.speed);
    settings.insert(QStringLiteral("group"), style.group);
    settings.insert(QStringLiteral("order"), style.order);
    settings.insert(QStringLiteral("lag"), style.lag);
    settings.insert(QStringLiteral("inDuration"), style.inDuration);
    settings.insert(QStringLiteral("outDuration"), style.outDuration);
    return settings;
}
}
