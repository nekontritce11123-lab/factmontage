// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "bin/projectclip.h"
#include "doc/kdenlivedoc.h"
#include "kdenlivesettings.h"
#include "monitor/monitormanager.h"
#include <QSet>

namespace StudioOptimization {
inline bool defaults(KdenliveDoc *document)
{
    if (!document || document->getDocumentProperty(QStringLiteral("sunimo:optimizationDefaultsVersion")).toInt() >= 1) return false;
    for (const auto &[name, value] : QMap<QString, QString>{
             {QStringLiteral("enableproxy"), QStringLiteral("1")},
             {QStringLiteral("generateproxy"), QStringLiteral("1")},
             {QStringLiteral("generateimageproxy"), QStringLiteral("0")},
             {QStringLiteral("proxyminsize"), QStringLiteral("1000")},
             {QStringLiteral("proxyresize"), QStringLiteral("640")}}.asKeyValueRange()) document->setDocumentProperty(name, value);
    if (document->getDocumentProperty(QStringLiteral("proxyparams")).isEmpty()) {
        document->setDocumentProperty(QStringLiteral("proxyparams"), QStringLiteral("-vf scale=%width:-2 -fps_mode passthrough -c:v libx264 -g 1 -bf 0 -crf 20 -preset veryfast -c:a aac -ab 128k"));
        document->setDocumentProperty(QStringLiteral("proxyextension"), QStringLiteral("mov"));
    }
    if (KdenliveSettings::proxyalphaparams().isEmpty()) {
        KdenliveSettings::setProxyalphaparams(QStringLiteral("-vf scale=%width:-2 -fps_mode passthrough -c:v ffv1 -pix_fmt bgra -c:a pcm_s16le"));
        KdenliveSettings::setProxyalphaextension(QStringLiteral("mkv"));
    }
    // Composition stays at the project profile; only monitor/preview output shrinks.
    document->setDocumentProperty(QStringLiteral("previewparameters"), QStringLiteral("vcodec=ffv1 pix_fmt=bgra an=1 scale=0.25 threads=2 real_time=-1"));
    document->setDocumentProperty(QStringLiteral("previewextension"), QStringLiteral("mkv"));
    KdenliveSettings::setPreviewScaling(4);
    KdenliveSettings::setAutopreview(true);
    if (pCore->monitorManager()) Q_EMIT pCore->monitorManager()->updatePreviewScaling();
    document->setDocumentProperty(QStringLiteral("sunimo:optimizationDefaultsVersion"), QStringLiteral("1"));
    return true;
}

inline bool proxyEligible(const std::shared_ptr<ProjectClip> &clip)
{
    if (!clip || (clip->clipType() != ClipType::AV && clip->clipType() != ClipType::Video)) return false;
    if (clip->getProducerIntProperty(QStringLiteral("meta.media.width")) <= 1000
        || clip->getProducerIntProperty(QStringLiteral("meta.media.variable_frame_rate")) != 0
        || clip->getProducerIntProperty(QStringLiteral("meta.media.frame_rate_num")) <= 0
        || clip->getProducerIntProperty(QStringLiteral("meta.media.frame_rate_den")) <= 0
        || clip->getProducerProperty(QStringLiteral("meta.media.progressive")) != QLatin1String("1")
        || clip->getProducerIntProperty(QStringLiteral("_wait_for_transcode")) != 0) return false;
    const QSet<QString> formats{QStringLiteral("yuv420p"), QStringLiteral("yuv422p"), QStringLiteral("yuv444p"),
        QStringLiteral("yuvj420p"), QStringLiteral("yuvj422p"), QStringLiteral("yuvj444p"),
        QStringLiteral("yuva420p"), QStringLiteral("yuva422p"), QStringLiteral("yuva444p"),
        QStringLiteral("rgb24"), QStringLiteral("bgr24"), QStringLiteral("rgba"), QStringLiteral("bgra"),
        QStringLiteral("argb"), QStringLiteral("abgr"), QStringLiteral("nv12"), QStringLiteral("nv21"),
        QStringLiteral("yuyv422"), QStringLiteral("uyvy422")};
    if (!formats.contains(clip->videoCodecProperty(QStringLiteral("pix_fmt")))) return false;
    const QSet<QString> codecs{QStringLiteral("h264"), QStringLiteral("hevc"), QStringLiteral("ffv1"),
        QStringLiteral("mjpeg"), QStringLiteral("mpeg4"), QStringLiteral("prores"), QStringLiteral("vp8"),
        QStringLiteral("vp9"), QStringLiteral("libvpx"), QStringLiteral("libvpx-vp9"), QStringLiteral("dnxhd"), QStringLiteral("rawvideo")};
    if (!codecs.contains(clip->videoCodecProperty(QStringLiteral("name")))) return false;
    const QString transfer = clip->getProducerProperty(QStringLiteral("meta.media.color_trc"));
    return transfer == QLatin1String("bt709") || transfer == QLatin1String("smpte170m")
        || transfer == QLatin1String("iec61966-2-1") || transfer == QLatin1String("1")
        || transfer == QLatin1String("6") || transfer == QLatin1String("13");
}
}
