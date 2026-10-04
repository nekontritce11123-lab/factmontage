// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QAtomicInt>
#include <QElapsedTimer>
#include <QProcess>
#include <QDebug>

namespace StudioWorker {
// Only used by native TaskManager workers. Cancellation never touches a worker's
// QProcess from the GUI thread; the deadline starts when its process is launched.
inline bool wait(QProcess &process, QAtomicInt &canceled, int timeout = 1800000)
{
    QElapsedTimer deadline;
    deadline.start();
    while (process.state() != QProcess::NotRunning) {
        if (canceled.loadAcquire() || deadline.elapsed() >= timeout) {
            if (!canceled.loadAcquire()) qWarning() << "Background process exceeded its deadline:" << process.program();
            canceled.storeRelease(1);
            process.kill();
            process.waitForFinished(1000);
            return false;
        }
        process.waitForFinished(100);
    }
    return !canceled.loadAcquire() && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
}
