// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QAtomicInt>
#include <QElapsedTimer>
#include <QProcess>
#include <QSet>
#include <cmath>

namespace StudioOptimization {
// Run inside ProxyTask's worker/lane. Demux timestamps without retaining the whole file's packet list.
inline bool constantFrameRate(const QString &probe, const QString &source, int videoStream, double fps, const QAtomicInt &canceled, qint64 *frames = nullptr)
{
    if (probe.isEmpty() || videoStream < 0 || !std::isfinite(fps) || fps <= 0) return false;
    QProcess metadata;
    metadata.start(probe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QString::number(videoStream),
        QStringLiteral("-show_entries"), QStringLiteral("stream=time_base,avg_frame_rate"), QStringLiteral("-of"), QStringLiteral("default=nw=1"), source});
    QElapsedTimer metadataDeadline;
    metadataDeadline.start();
    while (metadata.state() != QProcess::NotRunning) {
        if (canceled.loadAcquire() || metadataDeadline.elapsed() >= 5000) {
            metadata.kill(); metadata.waitForFinished(1000); return false;
        }
        metadata.waitForFinished(100);
    }
    if (metadata.exitStatus() != QProcess::NormalExit || metadata.exitCode() != 0 || canceled.loadAcquire()) return false;
    double tick = 0, declaredRate = 0;
    for (const auto &line : metadata.readAllStandardOutput().split('\n')) {
        const auto fields = line.trimmed().split('=');
        if (fields.size() != 2) continue;
        const auto fraction = fields[1].split('/');
        if (fraction.size() != 2) continue;
        bool numeratorOk, denominatorOk;
        const double numerator = fraction[0].toDouble(&numeratorOk), denominator = fraction[1].toDouble(&denominatorOk);
        if (!numeratorOk || !denominatorOk || !std::isfinite(numerator) || !std::isfinite(denominator) || numerator <= 0 || denominator <= 0) return false;
        if (fields[0] == "time_base") tick = numerator / denominator;
        if (fields[0] == "avg_frame_rate") declaredRate = numerator / denominator;
    }
    if (tick <= 0 || std::abs(declaredRate - fps) > fps * .000001) return false;
    // Each stored PTS is quantized to the container time base (MKV usually 1 ms).
    // Two timestamps can differ from the ideal grid by at most one whole tick.
    const double tolerance = std::max(.001, tick * fps + .00001);
    if (tolerance >= .25) return false; // Too coarse to prove frame correspondence.
    QProcess process;
    process.start(probe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-threads"), QStringLiteral("2"), QStringLiteral("-select_streams"), QString::number(videoStream),
        QStringLiteral("-show_packets"), QStringLiteral("-show_entries"), QStringLiteral("packet=pts_time"),
        QStringLiteral("-of"), QStringLiteral("csv=p=0"), source});
    QElapsedTimer deadline;
    deadline.start();
    QByteArray pending;
    QSet<qint64> reorder;
    qint64 next = 0;
    double origin = 0;
    bool valid = true;
    const auto read = [&] {
        pending += process.readAllStandardOutput();
        process.readAllStandardError();
        int end;
        while ((end = pending.indexOf('\n')) >= 0) {
            const QByteArray line = pending.left(end).trimmed();
            pending.remove(0, end + 1);
            if (line.isEmpty()) continue;
            bool ok;
            const double pts = line.split(',').front().toDouble(&ok);
            if (!ok || !std::isfinite(pts)) { valid = false; return; }
            if (next == 0 && reorder.isEmpty()) origin = pts;
            const double position = (pts - origin) * fps;
            if (!std::isfinite(position) || position < -tolerance || position > next + 256 + tolerance) { valid = false; return; }
            const qint64 frame = qRound64(position);
            if (std::abs(position - frame) > tolerance || frame < next || frame - next > 256 || reorder.contains(frame)) { valid = false; return; }
            reorder.insert(frame);
            while (reorder.remove(next)) ++next;
        }
        if (pending.size() > 65536) valid = false;
    };
    while (process.state() != QProcess::NotRunning) {
        if (canceled.loadAcquire() || deadline.elapsed() > 120000 || !valid) {
            process.kill();
            process.waitForFinished(1000); // Worker only; never called from the panel.
            return false;
        }
        process.waitForReadyRead(100);
        read();
    }
    read();
    if (frames) *frames = next;
    return valid && !canceled.loadAcquire() && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
        && pending.trimmed().isEmpty() && next > 1 && reorder.isEmpty();
}
}
