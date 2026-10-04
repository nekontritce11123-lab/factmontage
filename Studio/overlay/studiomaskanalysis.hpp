// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QDomDocument>
#include <QImageReader>

namespace StudioBackground {
// A recognized image format is NOT proof of a still frame: GIF/WebP can move,
// and an ordinary PNG can move through upstream animated effects. Be conservative.
inline bool canUseSingleFrameMask(const QString &source, const QByteArray &analysisXml)
{
    QImageReader reader(source);
    if (!reader.canRead() || reader.supportsAnimation() || reader.imageCount() != 1) return false;
    QDomDocument document;
    if (!document.setContent(analysisXml)) return false;
    return document.elementsByTagName(QStringLiteral("filter")).isEmpty();
}
}
