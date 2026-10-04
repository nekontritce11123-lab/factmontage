// SPDX-License-Identifier: MIT
// Qt shaping is performed ONCE. Runtime remains the existing native STXT renderer.
#pragma once
#include <QColor>
#include <QDataStream>
#include <QFont>
#include <QFile>
#include <QSize>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRawFont>
#include <QSaveFile>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>
#include <QtEndian>
#include <array>
#include <algorithm>
#include <string>
#include <cmath>
#include <memory>
#include <map>
#include <stdexcept>
#include <vector>

namespace SunimoTextQt {
inline double number(const QJsonObject &s, const char *key, double fallback, double low, double high)
{
    const auto value = s.value(QString::fromLatin1(key));
    const double result = value.isUndefined() ? fallback : value.toDouble(std::nan(""));
    if (!std::isfinite(result) || result < low || result > high)
        throw std::runtime_error(std::string("Invalid text setting: ") + key);
    return result;
}
inline QJsonObject rasterSettings(QJsonObject settings)
{
    for (const char *key : {"duration", "inDuration", "outDuration", "lag", "inPreset", "outPreset",
                            "order", "lifeAmount", "lifeSpeed", "lifeSource", "styleId"})
        settings.remove(QString::fromLatin1(key));
    return settings;
}
inline QByteArray compile(const QJsonObject &settings, QSize reference)
{
    if (reference.width() < 64 || reference.height() < 64 || reference.width() > 4096 || reference.height() > 4096)
        throw std::runtime_error("Native text editor supports project dimensions from 64 to 4096 pixels");
    const QString text = settings.value(QStringLiteral("text")).toString().normalized(QString::NormalizationForm_C);
    if (text.trimmed().isEmpty() || text.size() > 8192) throw std::runtime_error("Text is empty or exceeds 8192 UTF-16 units");
    QFont font(settings.value(QStringLiteral("font")).toString(QStringLiteral("sans-serif")));
    const int fontSize = int(number(settings, "size", 64, 12, 320));
    font.setPixelSize(fontSize);
    font.setBold(settings.value(QStringLiteral("bold")).toBool(true));
    if (settings.value(QStringLiteral("italic")).toBool()) font.setItalic(true);
    const double letterSpacing = number(settings, "letterSpacing", 0, -10, 40);
    const double wordSpacing = number(settings, "wordSpacing", 0, 0, 80);
    if (letterSpacing) font.setLetterSpacing(QFont::AbsoluteSpacing, letterSpacing);
    if (wordSpacing) font.setWordSpacing(wordSpacing);
    const int alignment = int(number(settings, "alignment", 1, 0, 2));
    const double lineSpacing = number(settings, "lineSpacing", 108, 80, 200) / 100;
    const int opacity = int(number(settings, "opacity", 100, 0, 100));
    const QColor color(settings.value(QStringLiteral("color")).toString(QStringLiteral("#fff9ef")));
    if (!color.isValid()) throw std::runtime_error("Invalid text color");
    const int requestedOutline = int(number(settings, "outlineWidth", 0, 0, 20));
    const int outline = settings.value(QStringLiteral("outlineEnabled")).toBool() ? requestedOutline : 0;
    const QColor outlineColor(settings.value(QStringLiteral("outlineColor")).toString(QStringLiteral("#ff000000")));
    if (!outlineColor.isValid()) throw std::runtime_error("Invalid outline color");
    const int shadow = int(number(settings, "shadow", 0, 0, 20));
    const QColor shadowColor(settings.value(QStringLiteral("shadowColor")).toString(QStringLiteral("#80000000")));
    if (!shadowColor.isValid()) throw std::runtime_error("Invalid shadow color");
    const bool background = settings.value(QStringLiteral("backgroundEnabled")).toBool();
    const QColor backgroundColor(settings.value(QStringLiteral("backgroundColor")).toString(QStringLiteral("#66000000")));
    if (!backgroundColor.isValid()) throw std::runtime_error("Invalid background color");
    const int padding = std::max(8, fontSize / 3) + outline + shadow + (background ? 10 : 0);
    const int width = int(reference.width() * number(settings, "width", 84, 10, 96) / 100);
    const int lineWidth = width - 2 * padding;
    if (lineWidth < 8) throw std::runtime_error("Text area too narrow for the selected font");
    qreal y = padding;
    int lines = 0;
    QRectF textBounds;
    std::vector<std::unique_ptr<QTextLayout>> layouts;
    std::vector<std::unique_ptr<QTextLayout>> outlines;
    for (const QString &paragraph : text.split(QLatin1Char('\n'))) {
        const QString source = paragraph.isEmpty() ? QStringLiteral(" ") : paragraph;
        auto layout = std::make_unique<QTextLayout>(source, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setAlignment(alignment == 0 ? Qt::AlignLeft : alignment == 2 ? Qt::AlignRight : Qt::AlignHCenter);
        layout->setTextOption(option);
        layout->beginLayout();
        while (true) {
            auto line = layout->createLine();
            if (!line.isValid()) break;
            if (++lines > 64) { layout->endLayout(); throw std::runtime_error("Too many lines"); }
            line.setLineWidth(lineWidth);
            line.setPosition(QPointF(padding, y));
            const auto bounds = line.naturalTextRect();
            if (!bounds.isEmpty()) textBounds = textBounds.isNull() ? bounds : textBounds.united(bounds);
            y += std::ceil(line.height() * lineSpacing);
        }
        layout->endLayout();
        if (outline) {
            auto outlined = std::make_unique<QTextLayout>(source, font);
            QTextCharFormat format;
            format.setForeground(color);
            format.setTextOutline(QPen(outlineColor, outline * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            outlined->setFormats({{0, int(source.size()), format}});
            outlined->setTextOption(option);
            outlined->beginLayout();
            for (int i = 0; i < layout->lineCount(); ++i) {
                auto line = outlined->createLine();
                if (!line.isValid()) throw std::runtime_error("Cannot outline shaped text");
                line.setLineWidth(lineWidth);
                line.setPosition(layout->lineAt(i).position());
            }
            outlined->endLayout();
            outlines.push_back(std::move(outlined));
        }
        layouts.push_back(std::move(layout));
    }
    const int height = int(std::ceil(y)) + padding;
    if (height > reference.height() || uint64_t(width) * height > 16 * 1024 * 1024)
        throw std::runtime_error("Text does not fit: reduce font size or number of lines");
    QImage image(width, height, QImage::Format_RGBA8888);
    if (image.isNull()) throw std::runtime_error("Cannot allocate text image");
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::TextAntialiasing);
        if (background && !textBounds.isEmpty()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(backgroundColor);
            painter.drawRoundedRect(textBounds.adjusted(-10, -7, 10, 7), 7, 7);
        }
        if (shadow) {
            painter.save();
            painter.translate(shadow, shadow);
            painter.setPen(shadowColor);
            for (const auto &layout : layouts) layout->draw(&painter, QPointF());
            painter.restore();
        }
        painter.setPen(color);
        for (const auto &layout : outlines) layout->draw(&painter, QPointF());
        for (const auto &layout : layouts) layout->draw(&painter, QPointF());
    }
    if (opacity < 100) for (int row = 0; row < height; ++row) {
        auto *pixels = image.scanLine(row);
        for (int x = 0; x < width; ++x) pixels[x * 4 + 3] = uchar((unsigned(pixels[x * 4 + 3]) * opacity + 50) / 100);
    }
    const int group = int(number(settings, "group", 3, 0, 3));
    struct EncodedSprite { QImage pixels; QRect bounds; quint32 word = 0, line = 0, flags = 0; };
    std::vector<EncodedSprite> sprites;
    if (group == 3) {
        sprites.push_back({image, image.rect()});
    } else {
        auto fade = [opacity](QImage &sprite) {
            if (opacity == 100) return;
            for (int row = 0; row < sprite.height(); ++row) {
                auto *pixels = sprite.scanLine(row);
                for (int x = 0; x < sprite.width(); ++x)
                    pixels[x * 4 + 3] = uchar((unsigned(pixels[x * 4 + 3]) * opacity + 50) / 100);
            }
        };
        if (background && !textBounds.isEmpty()) {
            const QRect rect = textBounds.adjusted(-12, -9, 12, 9).toAlignedRect().intersected(image.rect());
            QImage box(rect.size(), QImage::Format_RGBA8888);
            box.fill(Qt::transparent);
            QPainter painter(&box);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.translate(-rect.topLeft());
            painter.setPen(Qt::NoPen);
            painter.setBrush(backgroundColor);
            painter.drawRoundedRect(textBounds.adjusted(-10, -7, 10, 7), 7, 7);
            painter.end();
            fade(box);
            sprites.push_back({box, rect, 0, 0, 1});
        }
        struct Glyph { QRawFont font; quint32 index; QPointF position; };
        struct Cluster { std::vector<Glyph> glyphs; quint32 word = 0, line = 0; };
        quint32 wordId = 0, lineId = 0;
        for (const auto &layout : layouts) {
            const QString source = layout->text();
            std::vector<quint32> words(size_t(source.size()));
            bool afterSpace = true;
            for (int i = 0; i < source.size(); ++i) {
                if (!source.at(i).isSpace() && afterSpace) ++wordId;
                words[size_t(i)] = wordId;
                afterSpace = source.at(i).isSpace();
            }
            for (int lineNo = 0; lineNo < layout->lineCount(); ++lineNo, ++lineId) {
                const auto line = layout->lineAt(lineNo);
                std::map<int, Cluster> clusters;
                for (const auto &run : line.glyphRuns(line.textStart(), line.textLength(), QTextLayout::RetrieveAll)) {
                    const auto indexes = run.glyphIndexes();
                    const auto positions = run.positions();
                    const auto characters = run.stringIndexes();
                    if (characters.size() != indexes.size() || positions.size() != indexes.size())
                        throw std::runtime_error("Cannot extract shaped text clusters");
                    for (qsizetype i = 0; i < indexes.size(); ++i) {
                        const int character = int(characters.at(i));
                        if (character < 0 || character >= source.size() || source.at(character).isSpace()) continue;
                        int start = character;
                        while (start > 0 && !layout->isValidCursorPosition(start)) --start;
                        auto &cluster = clusters[start];
                        cluster.word = words[size_t(character)];
                        cluster.line = lineId;
                        cluster.glyphs.push_back({run.rawFont(), indexes.at(i), positions.at(i)});
                    }
                }
                for (const auto &[start, cluster] : clusters) {
                    QPainterPath path;
                    QRectF bounds;
                    for (const auto &glyph : cluster.glyphs) {
                        QTransform transform;
                        transform.translate(glyph.position.x(), glyph.position.y());
                        const QPainterPath shape = transform.map(glyph.font.pathForGlyph(glyph.index));
                        path.addPath(shape);
                        const QRectF glyphBounds = glyph.font.boundingRect(glyph.index).translated(glyph.position);
                        bounds = bounds.isNull() ? glyphBounds : bounds.united(glyphBounds);
                    }
                    if (bounds.isEmpty()) continue;
                    const int margin = outline + shadow + 2;
                    const QRect rect = bounds.adjusted(-margin, -margin, margin, margin).toAlignedRect().intersected(image.rect());
                    if (rect.isEmpty()) continue;
                    QImage sprite(rect.size(), QImage::Format_RGBA8888);
                    sprite.fill(Qt::transparent);
                    QPainter painter(&sprite);
                    painter.setRenderHint(QPainter::Antialiasing);
                    painter.setRenderHint(QPainter::TextAntialiasing);
                    painter.translate(-rect.topLeft());
                    auto drawGlyphs = [&](const QColor &ink) {
                        painter.setPen(ink);
                        for (const auto &glyph : cluster.glyphs) {
                            QGlyphRun run;
                            run.setRawFont(glyph.font);
                            run.setGlyphIndexes({glyph.index});
                            run.setPositions({glyph.position});
                            painter.drawGlyphRun(QPointF(), run);
                        }
                    };
                    if (shadow) { painter.save(); painter.translate(shadow, shadow); drawGlyphs(shadowColor); painter.restore(); }
                    if (outline && !path.isEmpty()) {
                        painter.setBrush(Qt::NoBrush);
                        painter.setPen(QPen(outlineColor, outline * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                        painter.drawPath(path);
                    }
                    drawGlyphs(color);
                    painter.end();
                    fade(sprite);
                    sprites.push_back({sprite, rect, cluster.word, cluster.line, 0});
                    if (sprites.size() > 512) throw std::runtime_error("Text needs more than 512 shaped clusters");
                }
            }
        }
    }
    if (sprites.empty()) throw std::runtime_error("Text has no visible shaped clusters");
    uint64_t pixelCount = 0;
    for (const auto &sprite : sprites) pixelCount += uint64_t(sprite.pixels.width()) * sprite.pixels.height();
    if (pixelCount > 16 * 1024 * 1024) throw std::runtime_error("Text clusters exceed the 16 million pixel limit");
    QJsonObject metadata = settings;
    metadata.insert(QStringLiteral("text"), text);
    metadata.insert(QStringLiteral("native_editor_version"), 1);
    metadata.insert(QStringLiteral("layout"), group == 3 ? QStringLiteral("qt-shaped-block") : QStringLiteral("qt-shaped-clusters"));
    const auto json = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    const float duration = float(number(settings,"duration",4,.1,120));
    const std::array<float,32> config{{duration,
        float(number(settings,"inDuration",.7,0,6)), float(number(settings,"outDuration",.7,0,6)),
        .8f,float(number(settings,"lag",0,0,.9)),24191.f,float(number(settings,"inPreset",2,0,100)),float(number(settings,"outPreset",-1,-1,100)),
        float(group),float(number(settings,"order",0,0,5)),9.f,float(number(settings,"lifeAmount",.35,0,1)),float(number(settings,"lifeSpeed",1,.2,2)),
        1.f,0.f,.3f,0.f,0.f,float(number(settings,"lifeSource",0,0,100)),0.f,0.f,1.f,0.f,
        float(number(settings,"fpsNum",25,1,120000)),float(number(settings,"fpsDen",1,1,1001)),0.f,0.f,0.f,float(fontSize),0.f,0.f,0.f}};
    QByteArray result;
    QDataStream out(&result, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setFloatingPointPrecision(QDataStream::SinglePrecision);
    out.writeRawData("SUNMTX1\0",8);
    out << quint32(2) << quint32(json.size()) << quint32(sprites.size()) << quint32(reference.width()) << quint32(reference.height()) << quint32(32);
    for (float value : config) out << value;
    out.writeRawData(json.constData(),json.size());
    const float centerX = reference.width() * number(settings,"x",50,0,100) / 100;
    const float centerY = reference.height() * number(settings,"y",50,0,100) / 100;
    for (const auto &sprite : sprites) {
        const int sw = sprite.pixels.width(), sh = sprite.pixels.height();
        const float x = centerX - width / 2.f + sprite.bounds.x();
        const float y = centerY - height / 2.f + sprite.bounds.y();
        out << x << y << float(sw) << float(sh) << x + sw / 2.f << y + sh / 2.f;
        out << quint32(sw) << quint32(sh) << sprite.word << sprite.line << sprite.flags << quint32(sw * sh * 4);
        for (int row = 0; row < sh; ++row)
            out.writeRawData(reinterpret_cast<const char *>(sprite.pixels.constScanLine(row)), sw * 4);
    }
    if (out.status()!=QDataStream::Ok) throw std::runtime_error("Cannot encode STXT scene");
    return result;
}
// Reuses the shaped sprites when only timing or motion changed.
inline QByteArray retime(const QByteArray &scene, const QJsonObject &settings)
{
    if (scene.size() < 160 || scene.left(8) != QByteArray("SUNMTX1\0", 8)
        || qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(scene.constData() + 8)) != 2)
        throw std::runtime_error("Invalid cached STXT scene");
    const auto oldLength = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(scene.constData() + 12));
    if (oldLength > 2 * 1024 * 1024 || oldLength > quint32(scene.size() - 160))
        throw std::runtime_error("Invalid cached STXT metadata");
    QJsonObject oldMetadata = QJsonDocument::fromJson(scene.mid(160, oldLength)).object();
    if (oldMetadata.isEmpty()) throw std::runtime_error("Invalid cached STXT metadata");
    const QString layout = oldMetadata.take(QStringLiteral("layout")).toString();
    oldMetadata.remove(QStringLiteral("native_editor_version"));
    QJsonObject current = settings;
    current.insert(QStringLiteral("text"), settings.value(QStringLiteral("text")).toString().normalized(QString::NormalizationForm_C));
    if (rasterSettings(oldMetadata) != rasterSettings(current))
        throw std::runtime_error("Cached text raster no longer matches settings");
    current.insert(QStringLiteral("native_editor_version"), 1);
    current.insert(QStringLiteral("layout"), layout);
    const QByteArray json = QJsonDocument(current).toJson(QJsonDocument::Compact);
    QByteArray result = scene.left(160);
    qToLittleEndian<quint32>(quint32(json.size()), reinterpret_cast<uchar *>(result.data() + 12));
    QDataStream out(&result, QIODevice::ReadWrite);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setFloatingPointPrecision(QDataStream::SinglePrecision);
    const auto patch = [&](int index, const char *key, double fallback, double low, double high) {
        out.device()->seek(32 + index * 4);
        out << float(number(settings, key, fallback, low, high));
    };
    patch(0, "duration", 4, .1, 120);
    patch(1, "inDuration", .7, 0, 6);
    patch(2, "outDuration", .7, 0, 6);
    patch(4, "lag", 0, 0, .9);
    patch(6, "inPreset", 2, 0, 100);
    patch(7, "outPreset", -1, -1, 100);
    patch(9, "order", 0, 0, 5);
    patch(11, "lifeAmount", .35, 0, 1);
    patch(12, "lifeSpeed", 1, .2, 2);
    patch(18, "lifeSource", 0, 0, 100);
    if (out.status() != QDataStream::Ok) throw std::runtime_error("Cannot update STXT timing");
    result.append(json);
    result.append(scene.constData() + 160 + oldLength, scene.size() - 160 - oldLength);
    return result;
}
inline bool save(const QString &path, const QByteArray &scene, QString *error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(scene)!=scene.size() || !file.commit()) {
        if (error) *error=file.errorString();
        return false;
    }
    return true;
}
inline QJsonObject metadata(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto header=file.read(160);
    if(header.size()!=160 || header.left(8)!=QByteArray("SUNMTX1\0",8)) return {};
    const auto length=qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header.constData()+12));
    if(length>2*1024*1024) return {};
    const auto bytes=file.read(length);
    if(bytes.size()!=length) return {};
    return QJsonDocument::fromJson(bytes).object();
}
}
