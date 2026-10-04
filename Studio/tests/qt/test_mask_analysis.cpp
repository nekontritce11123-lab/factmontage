// SPDX-License-Identifier: GPL-3.0-only
#include "../../overlay/studiomaskanalysis.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <cassert>
#include <iostream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir folder;
    assert(folder.isValid());
    const auto still = folder.filePath(QString::fromUtf8("неподвижный кадр.png"));
    QImage image(4, 4, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    assert(image.save(still));
    const QByteArray plain("<mlt><producer id='a'/></mlt>");
    assert(StudioBackground::canUseSingleFrameMask(still, plain));
    // A valid still with prefix effects may animate. Analyze every timeline frame.
    assert(!StudioBackground::canUseSingleFrameMask(still,
        "<mlt><producer><filter><property name='mlt_service'>affine</property></filter></producer></mlt>"));
    assert(!StudioBackground::canUseSingleFrameMask(still, "invalid XML"));
    assert(!StudioBackground::canUseSingleFrameMask(folder.filePath("missing.png"), plain));
    QFile animation(folder.filePath(QString::fromUtf8("движение.gif")));
    assert(animation.open(QIODevice::WriteOnly));
    // Two visibly different frames, not merely an image with a GIF extension.
    const auto bytes = QByteArray::fromBase64(
        "R0lGODlhBAAEAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQACgAAACwAAAAABAAEAAAICQABCBxIsCCAgAAh+QQBCgABACwAAAAABAAEAIEAAP8AAAAAAAAAAAAICQABCBxIsCCAgAA7");
    assert(animation.write(bytes) == bytes.size());
    animation.close();
    QImageReader reader(animation.fileName());
    assert(reader.canRead() && reader.supportsAnimation() && reader.imageCount() == 2);
    assert(!QImageReader::imageFormat(animation.fileName()).isEmpty()); // old shortcut wrongly accepted this
    assert(!StudioBackground::canUseSingleFrameMask(animation.fileName(), plain));
    std::cout << "Still/animated/filtered/missing/invalid analysis policy PASS\n";
}
