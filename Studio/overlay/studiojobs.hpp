// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "jobs/taskmanager.h"
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QImage>
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "utils/thumbnailcache.hpp"

namespace StudioJobs {
inline bool currentClip(const std::shared_ptr<ProjectClip> &clip)
{
    return clip && pCore->currentDoc() && !pCore->currentDoc()->closing
        && pCore->projectItemModel()->getClipByBinID(clip->clipId()) == clip;
}

inline std::shared_ptr<QAtomicInt> cancellation(AbstractTask *task)
{
    auto canceled = std::make_shared<QAtomicInt>(0);
    QObject::connect(task, &AbstractTask::jobCanceled, task, [canceled] { *canceled = 1; }, Qt::DirectConnection);
    return canceled;
}

inline void thumbnail(const std::shared_ptr<ProjectClip> &clip, int frame, QObject *owner, std::function<void(QImage)> ready)
{
    const ObjectId id(KdenliveObjectType::NoItem, -2, {});
    pCore->taskManager.discardJobs(id);
    if (ThumbnailCache::get()->hasThumbnail(clip->clipId(), frame)) {
        ready(ThumbnailCache::get()->getThumbnail(clip->clipId(), frame));
        return;
    }
    struct Task : AbstractTask {
        std::shared_ptr<ProjectClip> clip;
        int frame;
        QPointer<QObject> target;
        QPointer<KdenliveDoc> document;
        std::function<void(QImage)> ready;
        Task(const ObjectId &id, std::shared_ptr<ProjectClip> clip, int frame, QObject *owner, std::function<void(QImage)> ready)
            : AbstractTask(id, THUMBJOB, owner), clip(std::move(clip)), frame(frame), target(owner), document(pCore->currentDoc()), ready(std::move(ready)) {}
        void run() override {
            AbstractTaskDone finished(m_owner.itemId, this);
            QMutexLocker lock(&m_runMutex);
            if (m_isCanceled || pCore->taskManager.isBlocked() || !target) return;
            m_running = true;
            const QImage image = clip->fetchPixmap(frame); // Native cache, otherwise a separate thumbnail producer.
            if (m_isCanceled || !target) return;
            QMetaObject::invokeMethod(target, [image, document = document, clip = clip, callback = ready] {
                if (document == pCore->currentDoc() && pCore->projectItemModel()->getClipByBinID(clip->clipId()) == clip) callback(image);
            }, Qt::QueuedConnection);
        }
    };
    pCore->taskManager.startTask(-2, new Task(id, clip, frame, owner, std::move(ready)));
}

inline bool busy(QProcess *process)
{
    return process && (process->property("_sunimoQueued").toBool() || process->state() != QProcess::NotRunning);
}

// Both ends of a raw-frame pipe share one TaskManager slot.
inline void start(QProcess *process, int priority = 20, QProcess *producer = nullptr, bool automatic = false,
                  std::function<bool()> eligible = {})
{
    process->setProperty("_sunimoTimedOut", false);
    struct Completion { QUuid id; int remaining; bool done[2]{false, false}; QPointer<QObject> connections; };
    auto completion = std::make_shared<Completion>();
    completion->remaining = producer ? 2 : 1;
    completion->connections = new QObject(pCore.get());
    auto deadline = new QTimer(completion->connections);
    deadline->setSingleShot(true);
    deadline->setInterval(1800000);
    QObject::connect(process, &QProcess::started, deadline, qOverload<>(&QTimer::start));
    QObject::connect(deadline, &QTimer::timeout, process, [process, completion] {
        if (process->state() == QProcess::NotRunning || process->property("_sunimoJob").toUuid() != completion->id) return;
        process->setProperty("_sunimoTimedOut", true);
        pCore->taskManager.cancelExternal(completion->id);
    });
    QPointer<QProcess> target(process), input(producer);
    QPointer<KdenliveDoc> document(pCore->currentDoc());
    const auto finish = [completion](int index) {
        if (completion->done[index]) return;
        completion->done[index] = true;
        if (--completion->remaining == 0) {
            pCore->taskManager.finishExternal(completion->id);
            if (completion->connections) completion->connections->deleteLater();
        }
    };
    int index = 0;
    for (QProcess *p : {process, producer}) {
        if (!p) continue;
        const int slot = index++;
        QObject::connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), completion->connections, [finish, slot] { finish(slot); });
        QObject::connect(p, &QProcess::errorOccurred, completion->connections, [finish, slot, input](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                finish(slot);
                if (slot == 0 && input && input->state() == QProcess::NotRunning) finish(1);
            }
        });
        QObject::connect(p, &QObject::destroyed, completion->connections, [finish, slot] { finish(slot); });
        QObject::connect(p, &QProcess::started, completion->connections, [p] { AbstractTask::setPreferredPriority(p->processId()); });
    }
    process->setProperty("_sunimoQueued", true);
    completion->id = pCore->taskManager.queueExternal(process,
        [target, document, completion, input, finish, eligible] {
            if (!target) { finish(0); if (input) finish(1); return; }
            target->setProperty("_sunimoQueued", false);
            if (document != pCore->currentDoc() || (eligible && !eligible())) {
                if (input) finish(1);
                Q_EMIT target->finished(-1, QProcess::CrashExit);
                return;
            }
            target->start();
        },
        [target, input, finish] {
            if (input && input->state() != QProcess::NotRunning) input->kill();
            else if (input) finish(1);
            if (!target) { finish(0); return; }
            target->setProperty("_sunimoQueued", false);
            if (target->state() != QProcess::NotRunning) target->kill();
            else QTimer::singleShot(0, target, [target] { if (target) Q_EMIT target->finished(-1, QProcess::CrashExit); });
        }, priority, automatic);
    process->setProperty("_sunimoJob", completion->id);
}

inline void start(QProcess *process, const QString &program, const QStringList &arguments, int priority = 20)
{
    process->setProgram(program);
    process->setArguments(arguments);
    start(process, priority);
}

inline void cancel(QProcess *process)
{
    if (!process) return;
    pCore->taskManager.cancelExternal(process->property("_sunimoJob").toUuid());
    if (process->state() != QProcess::NotRunning) process->kill();
}

// A closing panel never waits for a child process on the GUI thread.
inline void dispose(QProcess *process)
{
    if (!process) return;
    if (process->parent()) process->disconnect(process->parent());
    process->setParent(nullptr);
    if (!busy(process)) { process->deleteLater(); return; }
    QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process, &QObject::deleteLater);
    QObject::connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) process->deleteLater();
    });
    cancel(process);
}
}
