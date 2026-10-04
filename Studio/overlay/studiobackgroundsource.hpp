// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "xml/xml.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTime>
#include <QUrl>

namespace StudioBackground {
inline bool entryRange(const QDomElement &filter, int fpsNum, int fpsDen, qlonglong &first, qlonglong &duration)
{
    const auto entry = filter.parentNode().toElement();
    if (entry.tagName() != QLatin1String("entry") || fpsNum < 1 || fpsDen < 1) return false;
    const auto frame = [fpsNum, fpsDen](const QString &value, qlonglong &result) {
        bool numeric = false;
        result = value.toLongLong(&numeric);
        if (numeric) return true;
        const QTime time = QTime::fromString(value, QStringLiteral("hh:mm:ss.zzz"));
        if (!time.isValid()) return false;
        result = qRound64(double(QTime(0, 0).msecsTo(time)) * fpsNum / (1000.0 * fpsDen));
        return true;
    };
    qlonglong last = -1;
    if (!frame(entry.attribute(QStringLiteral("in")), first)
        || !frame(entry.attribute(QStringLiteral("out")), last) || last < first) return false;
    duration = last - first + 1;
    return true;
}

inline QString sourcePath(const QDomElement &filter, const QString &projectRoot)
{
    QDomElement source;
    for (auto owner = filter.parentNode().toElement(); !owner.isNull(); owner = owner.parentNode().toElement()) {
        if (owner.tagName() == QLatin1String("entry")) {
            const QString id = owner.attribute(QStringLiteral("producer"));
            if (id.isEmpty()) return {};
            const auto root = filter.ownerDocument().documentElement();
            for (auto node = root.firstChildElement(); !node.isNull(); node = node.nextSiblingElement()) {
                if (node.attribute(QStringLiteral("id")) != id
                    || (node.tagName() != QLatin1String("chain") && node.tagName() != QLatin1String("producer"))) continue;
                if (!source.isNull()) return {}; // Ambiguous producer must not select a different clip.
                source = node;
            }
            break;
        }
        if (owner.tagName() == QLatin1String("chain") || owner.tagName() == QLatin1String("producer")) {
            source = owner;
            break;
        }
    }
    if (source.isNull()) return {};
    QString resource = Xml::getXmlProperty(source, QStringLiteral("kdenlive:originalurl"));
    if (resource.isEmpty()) resource = Xml::getXmlProperty(source, QStringLiteral("resource"));
    if (resource.isEmpty()) return {};
    const QUrl url(resource);
    QString path = url.isLocalFile() ? url.toLocalFile() : resource;
    if (QDir::isRelativePath(path)) path = QDir(projectRoot).absoluteFilePath(path);
    return path;
}

inline QString maskPath(const QString &asset, const QString &projectDataFolder, const QString &documentRoot, const QUrl &projectUrl = {})
{
    if (asset.isEmpty() || QFileInfo(asset).fileName() != asset) return {};
    const QString savedRoot = projectUrl.isLocalFile() && !projectUrl.toLocalFile().isEmpty()
        ? QFileInfo(projectUrl.toLocalFile()).absolutePath() : QString();
    QStringList candidates;
    if (!projectDataFolder.isEmpty())
        candidates << QDir(projectDataFolder).absoluteFilePath(QStringLiteral("studio-background/") + asset);
    if (!documentRoot.isEmpty())
        candidates << QDir(documentRoot).absoluteFilePath(QStringLiteral("others/") + asset);
    if (!savedRoot.isEmpty())
        candidates << QDir(savedRoot).absoluteFilePath(QStringLiteral("others/") + asset);
    if (!documentRoot.isEmpty()) {
        // Save As also copies here when legacy projects have no storagefolder.
        candidates << QDir(documentRoot).absoluteFilePath(QStringLiteral("studio-background/") + asset);
    }
    // Live Save As updates the URL but retains the native documentRoot.
    if (!savedRoot.isEmpty())
        candidates << QDir(savedRoot).absoluteFilePath(QStringLiteral("studio-background/") + asset);
    for (const QString &path : candidates)
        if (QFileInfo(path).isFile()) return path;
    return {};
}

inline void restoreMaskPaths(QDomDocument &document, const QString &projectDataFolder, const QString &documentRoot)
{
    const auto profile = document.documentElement().firstChildElement(QStringLiteral("profile"));
    const int fpsNum = profile.attribute(QStringLiteral("frame_rate_num")).toInt();
    const int fpsDen = profile.attribute(QStringLiteral("frame_rate_den")).toInt();
    const auto filters = document.elementsByTagName(QStringLiteral("filter"));
    for (int i = 0; i < filters.count(); ++i) {
        auto filter = filters.at(i).toElement();
        if (Xml::getXmlProperty(filter, QStringLiteral("mlt_service")) != QLatin1String("studio.background")
            && Xml::getXmlProperty(filter, QStringLiteral("kdenlive_id")) != QLatin1String("studio_background")) continue;
        const QString asset = Xml::getXmlProperty(filter, QStringLiteral("mask_asset"));
        const QString path = maskPath(asset, projectDataFolder, documentRoot);
        Xml::setXmlProperty(filter, QStringLiteral("_sbg_mask_path"), path);
        qlonglong first = 0, duration = 0;
        if (entryRange(filter, fpsNum, fpsDen, first, duration))
            Xml::setXmlProperty(filter, QStringLiteral("_sbg_clip_in"), QString::number(first));
    }
}

struct MaskRequest {
    QString path, sourcePath, sourceSha, recipeSha;
    int quality = 0, fpsNum = 0, fpsDen = 0, width = 0, height = 0;
    qlonglong offset = 0, duration = 0;
    bool staticImage = false;
};

inline quint32 u32(const QByteArray &data, int at)
{
    quint32 value = 0;
    for (int i = 0; i < 4; ++i) value |= quint32(quint8(data[at + i])) << (8 * i);
    return value;
}

inline quint64 u64(const QByteArray &data, int at)
{
    quint64 value = 0;
    for (int i = 0; i < 8; ++i) value |= quint64(quint8(data[at + i])) << (8 * i);
    return value;
}

inline quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xffffffffu;
    for (const char byte : data) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & quint32(-(crc & 1u)));
    }
    return ~crc;
}

inline QString fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) hash.addData(file.read(1024 * 1024));
    return file.error() == QFileDevice::NoError ? QString::fromLatin1(hash.result().toHex()) : QString();
}

inline QString validateMask(const MaskRequest &request)
{
    const QString name = QFileInfo(request.path).fileName();
    QFile file(request.path);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("маска %1 не найдена").arg(name);
    const QByteArray header = file.read(256);
    if (header.size() != 256 || header.left(8) != QByteArray("SBG0001", 8) || u32(header, 8) != 1
        || crc32(header.left(252)) != u32(header, 252))
        return QStringLiteral("маска %1 повреждена").arg(name);
    const quint32 frames = u32(header, 20);
    const quint64 indexAt = u64(header, 48);
    if (frames < 1 || indexAt < 256 || quint64(file.size()) < indexAt || quint64(file.size()) - indexAt != quint64(frames) * 16)
        return QStringLiteral("маска %1 повреждена").arg(name);
    if (!QFileInfo(request.sourcePath).isFile()) return QStringLiteral("исходный файл для маски %1 недоступен").arg(name);
    if (request.quality != 512 && request.quality != 768 && request.quality != 1280)
        return QStringLiteral("неверный размер анализа маски %1").arg(name);
    if (u32(header, 24) != quint32(request.fpsNum) || u32(header, 28) != quint32(request.fpsDen)
        || u32(header, 32) != quint32(request.width) || u32(header, 36) != quint32(request.height)
        || QString::fromLatin1(header.mid(56, 64)) != request.sourceSha
        || QString::fromLatin1(header.mid(120, 64)) != request.recipeSha
        || request.sourceSha != fileSha256(request.sourcePath))
        return QStringLiteral("маска %1 устарела или не соответствует проекту").arg(name);
    if (request.offset < 0 || request.duration < 1
        || (!request.staticImage && (quint64(request.offset) >= frames
                                     || quint64(request.offset) + quint64(request.duration) > frames)))
        return QStringLiteral("клип выходит за рассчитанный диапазон маски %1").arg(name);
    return {};
}
}
