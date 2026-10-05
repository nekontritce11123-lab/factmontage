#!/usr/bin/env python3
"""Apply the deliberately small integration to pristine Kdenlive 26.08.0."""
from pathlib import Path


def replace(path, old, new, expected=1):
    path = Path(path)
    data = path.read_text(encoding='utf-8')
    if data.count(old) != expected:
        raise RuntimeError(f'{path}: unsupported source or already patched ({data.count(old)} matches)')
    path.write_text(data.replace(old, new), encoding='utf-8', newline='\n')


def rewrite_block(path, start, end, rewrite):
    path = Path(path)
    data = path.read_text(encoding='utf-8')
    if data.count(start) != 1 or data.count(end) != 1:
        raise RuntimeError(f'{path}: unsupported source or already patched block')
    first = data.index(start)
    last = data.index(end, first)
    path.write_text(data[:first] + rewrite(data[first:last]) + data[last:], encoding='utf-8', newline='\n')


def optimize_tasks():
    replace('src/jobs/abstracttask.h', '#include <QAtomicInt>', '#include <QAtomicInt>\n#include <QPointer>')
    replace('src/jobs/abstracttask.h', '    QObject* m_object;', '    QPointer<QObject> m_object;')
    replace('src/jobs/taskmanager.h', '#include <vector>',
            '#include <vector>\n#include <functional>\n#include <QPointer>\n#include <QAtomicInt>\n#include <QEventLoop>\n#include <QTimer>')
    replace('src/jobs/taskmanager.h', '    QThreadPool m_transcodePool;', '''    struct ExternalTask {
        QUuid id;
        QPointer<QObject> owner;
        std::function<void()> start, cancel;
        int priority;
        bool automatic;
        QMetaObject::Connection ownerGone;
    };
    QList<AbstractTask *> m_pendingTasks;
    AbstractTask *m_activeTask = nullptr;
    QList<ExternalTask> m_externalTasks;
    QUuid m_activeExternal;
    bool m_exporting = false;
    void dispatch();
    void cancelTasks(const std::function<bool(AbstractTask *)> &matches, bool softDelete = false);''')
    replace('src/jobs/taskmanager.h', '    bool m_blockUpdates;', '    QAtomicInt m_blockUpdates;')
    replace('src/jobs/taskmanager.h', '    void unBlock();', '''    void unBlock();
    QUuid queueExternal(QObject *owner, std::function<void()> start, std::function<void()> cancel, int priority, bool automatic);
    void finishExternal(const QUuid &id);
    void cancelExternal(const QUuid &id);
    void setExporting(bool exporting);
    bool backgroundIdle() const;
    void waitUntilIdle(const QVector<AbstractTask::JOBTYPE> &exceptions = {});
    bool exporting() const { return m_exporting; }''')
    replace('src/jobs/taskmanager.h', '    void jobCount(int);', '    void jobCount(int);\n    void exportChanged(bool);')
    replace('src/jobs/taskmanager.cpp', '''    int maxThreads = qMin(4, QThread::idealThreadCount() - 1);
    m_taskPool.setMaxThreadCount(qMax(maxThreads, 1));
    m_transcodePool.setMaxThreadCount(KdenliveSettings::proxythreads());''', '    m_taskPool.setMaxThreadCount(1);')
    replace('src/jobs/taskmanager.cpp', '    slotCancelJobs();', '    slotCancelJobs(true);\n    m_taskPool.waitForDone();')
    rewrite_block('src/jobs/taskmanager.cpp', 'void TaskManager::updateConcurrency()', 'bool TaskManager::hasPendingJob(', lambda _: r'''void TaskManager::updateConcurrency()
{
    m_taskPool.setMaxThreadCount(1);
}

void TaskManager::cancelTasks(const std::function<bool(AbstractTask *)> &matches, bool softDelete)
{
    QWriteLocker lock(&m_tasksListLock);
    for (auto it = m_taskList.begin(); it != m_taskList.end();) {
        auto &tasks = it->second;
        for (auto t = tasks.begin(); t != tasks.end();) {
            AbstractTask *task = *t;
            if (!matches(task)) { ++t; continue; }
            if (task == m_activeTask) {
                task->cancelJob(softDelete);
                ++t; // taskDone owns cleanup after the worker actually stops.
            } else {
                task->cancelJob(softDelete);
                m_pendingTasks.removeAll(task);
                t = tasks.erase(t);
                task->deleteLater();
            }
        }
        if (tasks.empty()) it = m_taskList.erase(it);
        else ++it;
    }
    QMetaObject::invokeMethod(this, &TaskManager::dispatch, Qt::QueuedConnection);
}

void TaskManager::discardJobsByType(AbstractTask::JOBTYPE type)
{
    cancelTasks([type](AbstractTask *task) { return task->m_type == type; });
}

void TaskManager::discardJobs(const ObjectId &owner, AbstractTask::JOBTYPE type, bool softDelete, const QVector<AbstractTask::JOBTYPE> exceptions)
{
    cancelTasks([&](AbstractTask *task) {
        return task->m_owner == owner && (type == AbstractTask::NOJOBTYPE || task->m_type == type)
            && !exceptions.contains(task->m_type);
    }, softDelete);
}

void TaskManager::discardJob(const ObjectId &owner, const QUuid &uuid)
{
    cancelTasks([&](AbstractTask *task) { return task->m_owner == owner && task->m_uuid == uuid; });
}

''')
    replace('src/jobs/taskmanager.cpp', '        return m_taskList.find(owner.itemId) != m_taskList.end();', '''        const auto found = m_taskList.find(owner.itemId);
        return found != m_taskList.end() && std::any_of(found->second.begin(), found->second.end(),
            [](AbstractTask *task) { return !task->m_isCanceled; });''')
    rewrite_block('src/jobs/taskmanager.cpp', 'void TaskManager::taskDone(', 'int TaskManager::getJobProgressForClip(', lambda _: r'''void TaskManager::taskDone(int cid, AbstractTask *task)
{
    // Called by the worker, including while a document is closing.
    {
        QWriteLocker lock(&m_tasksListLock);
        auto it = m_taskList.find(cid);
        if (it != m_taskList.end()) {
            auto &tasks = it->second;
            tasks.erase(std::remove(tasks.begin(), tasks.end(), task), tasks.end());
            if (tasks.empty()) m_taskList.erase(it);
        }
        if (m_activeTask == task) m_activeTask = nullptr;
    }
    task->deleteLater();
    QMetaObject::invokeMethod(this, &TaskManager::dispatch, Qt::QueuedConnection);
}

void TaskManager::slotCancelJobs(bool leaveBlocked, const QVector<AbstractTask::JOBTYPE> exceptions)
{
    if (m_blockUpdates) return; // A nested callback must not unblock its caller.
    m_blockUpdates = true;
    cancelTasks([&](AbstractTask *task) { return !exceptions.contains(task->m_type); });
    const auto external = m_externalTasks;
    for (const auto &task : external) cancelExternal(task.id);
    waitUntilIdle(exceptions);
    if (!leaveBlocked) unBlock();
}

void TaskManager::unBlock()
{
    m_blockUpdates = false;
    QMetaObject::invokeMethod(this, &TaskManager::dispatch, Qt::QueuedConnection);
}

void TaskManager::startTask(int ownerId, AbstractTask *task)
{
    QWriteLocker lock(&m_tasksListLock);
    if (m_blockUpdates) { task->deleteLater(); return; }
    if (task->m_type != AbstractTask::LOADJOB && task->m_type != AbstractTask::THUMBJOB
        && task->m_type != AbstractTask::AUDIOTHUMBJOB && task->m_type != AbstractTask::CACHEJOB
        && !task->property("_sunimoAutomaticProxy").toBool()) task->m_priority = 20;
    if (task->m_type == AbstractTask::THUMBJOB && task->m_owner.type == KdenliveObjectType::NoItem) task->m_priority = 20;
    m_taskList[ownerId].push_back(task);
    m_pendingTasks.append(task);
    QMetaObject::invokeMethod(this, &TaskManager::dispatch, Qt::QueuedConnection);
}

QUuid TaskManager::queueExternal(QObject *owner, std::function<void()> start, std::function<void()> cancel, int priority, bool automatic)
{
    const QUuid id = QUuid::createUuid();
    m_externalTasks.append({id, owner, std::move(start), std::move(cancel), priority, automatic, {}});
    m_externalTasks.last().ownerGone = connect(owner, &QObject::destroyed, this, [this, id] { cancelExternal(id); });
    if (m_blockUpdates || (automatic && m_exporting)) QMetaObject::invokeMethod(this, [this, id] { cancelExternal(id); }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(this, &TaskManager::dispatch, Qt::QueuedConnection);
    return id;
}

void TaskManager::finishExternal(const QUuid &id)
{
    for (int i = 0; i < m_externalTasks.size(); ++i) if (m_externalTasks[i].id == id) {
        disconnect(m_externalTasks[i].ownerGone);
        m_externalTasks.removeAt(i);
        if (m_activeExternal == id) m_activeExternal = {};
        QMetaObject::invokeMethod(this, &TaskManager::dispatch, Qt::QueuedConnection);
        return;
    }
}

void TaskManager::cancelExternal(const QUuid &id)
{
    for (int i = 0; i < m_externalTasks.size(); ++i) if (m_externalTasks[i].id == id) {
        const auto stop = m_externalTasks[i].cancel;
        if (m_activeExternal != id) { disconnect(m_externalTasks[i].ownerGone); m_externalTasks.removeAt(i); }
        stop();
        QMetaObject::invokeMethod(this, &TaskManager::dispatch, Qt::QueuedConnection);
        return;
    }
}

bool TaskManager::backgroundIdle() const
{
    QReadLocker lock(&m_tasksListLock);
    return m_activeTask == nullptr && m_activeExternal.isNull() && m_taskPool.activeThreadCount() == 0;
}

void TaskManager::waitUntilIdle(const QVector<AbstractTask::JOBTYPE> &exceptions)
{
    // Preserve the native cancellation fence before profile changes or file moves.
    // Individual discard/cancel operations remain asynchronous.
    const auto done = [this, &exceptions] {
        if (backgroundIdle()) return true;
        QReadLocker lock(&m_tasksListLock);
        return m_activeExternal.isNull() && m_activeTask && exceptions.contains(m_activeTask->m_type);
    };
    if (done()) return;
    QEventLoop loop;
    QTimer timer;
    connect(&timer, &QTimer::timeout, &loop, [&] { if (done()) loop.quit(); });
    timer.start(10);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
}

void TaskManager::setExporting(bool exporting)
{
    if (m_exporting == exporting) return;
    m_exporting = exporting;
    if (exporting) {
        const auto tasks = m_externalTasks;
        for (const auto &task : tasks) if (task.automatic) cancelExternal(task.id);
    }
    Q_EMIT exportChanged(exporting);
    QMetaObject::invokeMethod(this, &TaskManager::dispatch, Qt::QueuedConnection);
}

void TaskManager::dispatch()
{
    QWriteLocker lock(&m_tasksListLock);
    int count = m_externalTasks.size();
    for (const auto &entry : m_taskList) count += entry.second.size();
    Q_EMIT jobCount(count);
    if (m_blockUpdates || m_exporting || m_activeTask || !m_activeExternal.isNull()) return;
    AbstractTask *native = nullptr;
    for (auto *task : std::as_const(m_pendingTasks))
        if (!native || task->m_priority > native->m_priority) native = task;
    int external = -1;
    for (int i = 0; i < m_externalTasks.size(); ++i)
        if (m_externalTasks[i].owner && (external < 0 || m_externalTasks[i].priority > m_externalTasks[external].priority)) external = i;
    if (external >= 0 && (!native || m_externalTasks[external].priority > native->m_priority)) {
        const auto task = m_externalTasks[external];
        m_activeExternal = task.id;
        lock.unlock();
        task.start();
    } else if (native) {
        m_pendingTasks.removeAll(native);
        m_activeTask = native;
        m_taskPool.start(native);
    }
}

''')


def optimize_document():
    # The test-only save helper skips the native Save As cache initialization.
    replace('src/project/projectmanager.cpp', 'bool ProjectManager::testSaveFileAs(const QString &outputFileName)\n{',
            'bool ProjectManager::testSaveFileAs(const QString &outputFileName)\n{\n    m_project->initCacheDirs();')
    # The pinned FFmpeg removed -vsync; its replacement only selects frame timing.
    replace('src/kdenlivesettings.kcfg', 'filter_hw_device,i,vsync,ab,qp', 'filter_hw_device,i,vsync,fps_mode,ab,qp')
    replace('src/project/projectmanager.cpp', 'bool ProjectManager::closeCurrentDocument(bool saveChanges, bool quit)\n{',
            'bool ProjectManager::closeCurrentDocument(bool saveChanges, bool quit)\n{\n    if (m_project && pCore->taskManager.isBlocked()) return false;')
    replace('src/project/projectmanager.cpp', '''                // Wait until project finished loading to close app
                return false;''', '''                // Wait until project finished loading to close app.
                // Cancellation has finished; the deferred close must be allowed to retry.
                pCore->taskManager.unBlock();
                return false;''')
    replace('src/project/projectmanager.cpp', '        pCore->taskManager.slotCancelJobs(true);',
            '        pCore->taskManager.slotCancelJobs(true);\n        pCore->taskManager.waitUntilIdle();')
    replace('src/project/projectmanager.cpp', '#include "projectmanager.h"', '#include "projectmanager.h"\n#include <QScopeGuard>')
    replace('src/mainwindow.cpp', '                        pCore->projectManager()->moveProjectData(oldDir.absoluteFilePath(documentId), newDir.absolutePath());',
            '''                        pCore->projectManager()->moveProjectData(oldDir.absoluteFilePath(documentId), newDir.absolutePath());
                        project = pCore->currentDoc();
                        if (!project) { delete w; return; }''')
    rewrite_block('src/project/projectmanager.cpp', 'void ProjectManager::moveProjectData(', 'void ProjectManager::slotMoveProgress(', lambda _: '''void ProjectManager::moveProjectData(const QString &src, const QString &dest)
{
    if (pCore->taskManager.isBlocked()) return;
    pCore->taskManager.slotCancelJobs(true);
    const auto unblock = qScopeGuard([] { pCore->taskManager.unBlock(); });
    QPointer<KdenliveDoc> document(m_project);
    bool ok;
    const QList<QUrl> proxyUrls = m_project->getProjectData(&ok);
    if (!ok) {
        KMessageBox::error(pCore->window(), i18n("Error moving project folder, cannot access cache folder"));
        return;
    }
    // Finish both moves before either caller changes the profile or reloads the project.
    const auto move = [this](const QList<QUrl> &sources, const QString &destination) {
        KIO::CopyJob *job = KIO::move(sources, QUrl::fromLocalFile(destination));
        job->setAutoDelete(false);
        if (job->uiDelegate()) KJobWidgets::setWindow(job, pCore->window());
        connect(job, &KJob::percentChanged, this, &ProjectManager::slotMoveProgress);
        job->exec();
        return job;
    };
    if (!proxyUrls.isEmpty()) {
        const QString proxyDir = dest + QStringLiteral("/proxy/");
        if (!QDir().mkpath(proxyDir)) {
            KMessageBox::error(pCore->window(), i18n("Cannot create folder %1", proxyDir));
            return;
        }
        std::unique_ptr<KIO::CopyJob> job(move(proxyUrls, proxyDir));
        if (job->error()) {
            KMessageBox::error(pCore->window(), i18n("Error moving project folder: %1", job->errorText()));
            return;
        }
    }
    std::unique_ptr<KIO::CopyJob> job(move({QUrl::fromLocalFile(src)}, dest));
    if (document != m_project || !document) return;
    pCore->taskManager.unBlock();
    slotMoveFinished(job.get());
}

''')
    replace('src/doc/kdenlivedoc.cpp', '#include "assets/studio/studiobackgroundsource.hpp"',
            '#include "assets/studio/studiobackgroundsource.hpp"\n#include "assets/studio/studiooptimization.hpp"')
    replace('src/doc/kdenlivedoc.cpp', '    // Load metadata\n', '    StudioOptimization::defaults(this);\n\n    // Load metadata\n')
    replace('src/doc/kdenlivedoc.cpp', '    QString proxyparams = m_documentProperties.value(QStringLiteral("proxyparams"))',
            '    StudioOptimization::defaults(this);\n\n    QString proxyparams = m_documentProperties.value(QStringLiteral("proxyparams"))')
    replace('src/doc/kdenlivedoc.cpp', '    initializeProperties(true, tracks, 2);',
            '    initializeProperties(true, tracks, 2);\n    StudioOptimization::defaults(this);')
    replace('src/bin/projectclip.cpp', '#include "projectclip.h"', '#include "projectclip.h"\n#include "assets/studio/studiooptimization.hpp"')
    # Thumbnail-only loads never replace the producer or emit enableUndo(true).
    replace('src/bin/projectclip.cpp', '''            if (pCore->window()) {
                // Disable undo / redo while a clip is loading, else we could attempt operations on a clip not completely loaded''',
            '''            if (!refreshOnly && pCore->window()) {
                // Disable undo / redo while a clip is loading, else we could attempt operations on a clip not completely loaded''')
    # Preserve explicit manual proxy creation/rebuild; guard all automatic media kinds.
    replace('src/bin/projectclip.cpp', '    bool generateProxy = false;\n',
            '    if (!rebuildProxy && !StudioOptimization::proxyEligible(std::static_pointer_cast<ProjectClip>(shared_from_this()))) return;\n    bool generateProxy = false;\n')
    replace('src/bin/projectclip.cpp', '''                if (!hasAlpha()) {
                    clipToProxy = std::static_pointer_cast<ProjectClip>(shared_from_this());
                } else {
                    qDebug() << ":::::: VIDEO WITH ALPHA; SKIP PROXY GENERATION....";
                }''', '                clipToProxy = std::static_pointer_cast<ProjectClip>(shared_from_this());')
    replace('src/bin/projectclip.cpp', '    if (generateProxy) {\n',
            '    if (generateProxy) {\n        setProducerProperty(QStringLiteral("sunimo:automaticProxy"), rebuildProxy ? 0 : 1);\n')
    replace('src/project/projectmanager.cpp', '    pCore->bin()->setDocument(doc);', '''    pCore->bin()->setDocument(doc);
    QTimer::singleShot(0, doc, [doc] {
        if (doc != pCore->currentDoc()) return;
        for (const auto &id : pCore->projectItemModel()->getAllClipIds()) {
            const auto clip = pCore->projectItemModel()->getClipByBinID(id);
            if (clip && clip->getProducerProperty(QStringLiteral("kdenlive:proxy")).isEmpty()) clip->checkProxy();
        }
    });''')
    replace('src/jobs/proxytask.cpp', '            parameters << dest;\n',
            '            parameters << QStringLiteral("-threads") << QStringLiteral("2") << dest;\n')
    replace('src/jobs/proxytask.cpp', '        QProcess jobProcess;\n        QObject::connect(&jobProcess, &QProcess::readyReadStandardError, this, &ProxyTask::processLogInfo);',
            '''        parameters = QStringList{QStringLiteral("-threads"), QStringLiteral("2"), QStringLiteral("-filter_threads"), QStringLiteral("1"),
                            QStringLiteral("-filter_complex_threads"), QStringLiteral("1")} + parameters;
        QProcess jobProcess;
        QObject::connect(&jobProcess, &QProcess::readyReadStandardError, this, &ProxyTask::processLogInfo);''')


def optimize_worker_processes():
    for name in ('melt', 'filter', 'transcode', 'stabilize', 'speed', 'cut', 'scenesplit', 'mask', 'customjob'):
        path = Path(f'src/jobs/{name}task.cpp')
        replace(path, '#include "core.h"', '#include "core.h"\n#include "assets/studio/studioworker.hpp"')
        data = path.read_text(encoding='utf-8')
        process = 'm_scriptJob' if name == 'mask' else 'm_jobProcess'
        count = data.count(f'{process}->waitForFinished(-1);')
        if not count:
            raise RuntimeError(f'{path}: missing native process wait')
        data = data.replace(f'{process}->waitForFinished(-1);', f'StudioWorker::wait(*{process}, m_isCanceled);')
        # Polling occurs in the process's owning worker, including cancellation.
        data = '\n'.join(line for line in data.split('\n') if not ('jobCanceled' in line and '&QProcess::kill' in line))
        path.write_text(data, encoding='utf-8', newline='\n')


def optimize_job_commits():
    # Retain the original clip, not an id that can be reused by the next document.
    for name in ('proxytask', 'cliploadtask'):
        replace(f'src/jobs/{name}.h', '#pragma once', '#pragma once\n#include <memory>\nclass ProjectClip;')
        replace(f'src/jobs/{name}.h', 'private:\n', '''private:
    std::shared_ptr<ProjectClip> m_studioClip;
    std::shared_ptr<QAtomicInt> m_studioCanceled;
''')
        replace(f'src/jobs/{name}.cpp', f'#include "{name}.h"',
                f'#include "{name}.h"\n#include "assets/studio/studiojobs.hpp"')
    replace('src/jobs/proxytask.h', '    bool m_isFfmpegJob;', '    bool m_isFfmpegJob;\n    bool m_studioAutomatic = false;')
    replace('src/jobs/proxytask.cpp', '    m_description = i18n("Creating proxy");', '''    m_description = i18n("Creating proxy");
    m_studioClip = pCore->projectItemModel()->getClipByBinID(QString::number(owner.itemId));
    m_studioCanceled = StudioJobs::cancellation(this);''')
    replace('src/jobs/cliploadtask.cpp', '    m_description = m_thumbOnly ? i18n("Video thumbs") : i18n("Loading clip");', '''    m_description = m_thumbOnly ? i18n("Video thumbs") : i18n("Loading clip");
    m_studioClip = pCore->projectItemModel()->getClipByBinID(QString::number(owner.itemId));
    m_studioCanceled = StudioJobs::cancellation(this);
    connect(this, &AbstractTask::jobCanceled, this, [this] {
        if (m_softDelete || m_thumbOnly || !m_studioClip) return;
        QMetaObject::invokeMethod(m_studioClip.get(), [clip = m_studioClip] {
            if (!pCore->taskManager.isBlocked() && StudioJobs::currentClip(clip) && !clip->statusReady())
                clip->setClipStatus(FileStatus::StatusMissing);
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection);''')
    for name, count in (('proxytask', 1), ('cliploadtask', 5)):
        replace(f'src/jobs/{name}.cpp', 'auto binClip = pCore->projectItemModel()->getClipByBinID(QString::number(m_owner.itemId));',
                'auto binClip = m_studioClip;', expected=count)
    replace('src/jobs/proxytask.cpp', '    task->m_isForce = force;', '''    task->m_isForce = force;
    if (task->m_studioClip) {
        task->m_studioAutomatic = task->m_studioClip->getProducerIntProperty(QStringLiteral("sunimo:automaticProxy")) == 1;
        task->m_studioClip->resetProducerProperty(QStringLiteral("sunimo:automaticProxy"));
    }
    task->setProperty("_sunimoAutomaticProxy", task->m_studioAutomatic);''')
    replace('src/jobs/proxytask.cpp', '#include "assets/studio/studiojobs.hpp"',
            '#include "assets/studio/studiojobs.hpp"\n#include "assets/studio/studiocfr.hpp"\n#include "assets/studio/studioworker.hpp"')
    replace('src/jobs/proxytask.cpp', '    QString source = binClip->getProducerProperty(QStringLiteral("kdenlive:originalurl"));\n', '')
    replace('src/jobs/proxytask.cpp', '    QFileInfo fInfo(dest);', '''    QString source = binClip->getProducerProperty(QStringLiteral("kdenlive:originalurl"));
    const QString probe = QFileInfo(KdenliveSettings::ffmpegpath()).dir().absoluteFilePath(QStringLiteral("ffprobe"));
    const int stream = binClip->getProducerIntProperty(QStringLiteral("video_index"));
    const double numerator = binClip->getProducerIntProperty(QStringLiteral("meta.media.frame_rate_num"));
    const double denominator = binClip->getProducerIntProperty(QStringLiteral("meta.media.frame_rate_den"));
    const double fps = denominator > 0 ? numerator / denominator : 0;
    qint64 sourceFrames = 0;
    if (m_studioAutomatic && !StudioOptimization::constantFrameRate(probe, source, stream, fps, m_isCanceled, &sourceFrames)) {
        QMetaObject::invokeMethod(binClip.get(), [clip = binClip, canceled = m_studioCanceled] {
            if (canceled->loadAcquire() || !StudioJobs::currentClip(clip)) return;
            clip->setProperties({{QStringLiteral("kdenlive:proxy"), QStringLiteral("-")}}, true);
            pCore->displayBinMessage(i18n("Automatic proxy skipped: constant frame timing could not be verified. The original remains available."), int(KMessageWidget::Information));
        }, Qt::QueuedConnection);
        m_progress = 100;
        return;
    }
    QFileInfo fInfo(dest);
    bool cachedProxy = fInfo.exists() && fInfo.size() > 0;
    if (cachedProxy && m_studioAutomatic) {
        qint64 proxyFrames = 0;
        cachedProxy = StudioOptimization::constantFrameRate(probe, dest, stream, fps, m_isCanceled, &proxyFrames) && proxyFrames == sourceFrames;
    }''')
    replace('src/jobs/proxytask.cpp', 'fInfo.exists() && fInfo.size() > 0) {', 'cachedProxy) {')
    replace('src/jobs/proxytask.cpp', '        jobProcess.waitForFinished(-1);', '        StudioWorker::wait(jobProcess, m_isCanceled);', expected=2)
    proxy_source = Path('src/jobs/proxytask.cpp')
    proxy_source.write_text('\n'.join(line for line in proxy_source.read_text(encoding='utf-8').split('\n')
                                    if not ('jobCanceled' in line and '&QProcess::kill' in line)), encoding='utf-8', newline='\n')
    replace('src/jobs/proxytask.cpp', '    // remove temporary playlist if it exists', '''    if (m_studioAutomatic && result && !m_isCanceled) {
        qint64 proxyFrames = 0;
        result = StudioOptimization::constantFrameRate(probe, dest, stream, fps, m_isCanceled, &proxyFrames) && proxyFrames == sourceFrames;
        if (!result) m_logDetails += QStringLiteral("Proxy frame timing/count does not match the original.");
    }
    // remove temporary playlist if it exists''')
    # Queued commits carry their cancellation flag independently of the finished worker.
    for connection in ('QueuedConnection', 'BlockingQueuedConnection'):
        replace('src/jobs/proxytask.cpp', f'QMetaObject::invokeMethod(binClip.get(), "updateProxyProducer", Qt::{connection}, Q_ARG(QString, dest));', '''QMetaObject::invokeMethod(binClip.get(), [clip = binClip, canceled = m_studioCanceled, dest] {
            if (!canceled->loadAcquire() && !pCore->taskManager.isBlocked() && StudioJobs::currentClip(clip)
                && clip->getProducerProperty(QStringLiteral("kdenlive:proxy")) == dest) clip->updateProxyProducer(dest);
        }, Qt::QueuedConnection);''')
    replace('src/jobs/proxytask.cpp', 'QMetaObject::invokeMethod(binClip.get(), "setProperties", Qt::BlockingQueuedConnection, Q_ARG(stringMap, proxyValue), Q_ARG(bool, true));', '''QMetaObject::invokeMethod(binClip.get(), [clip = binClip, canceled = m_studioCanceled, proxyValue] {
                    if (!canceled->loadAcquire() && !pCore->taskManager.isBlocked() && StudioJobs::currentClip(clip)) clip->setProperties(proxyValue, true);
                }, Qt::QueuedConnection);''', expected=2)
    replace('src/jobs/cliploadtask.cpp', '''            QMetaObject::invokeMethod(binClip.get(), "setProducer", Qt::BlockingQueuedConnection, Q_ARG(std::shared_ptr<Mlt::Producer>, std::move(producer)),
                                      Q_ARG(bool, true));''', '''            QPointer<ClipLoadTask> task(this);
            QMetaObject::invokeMethod(binClip.get(), [clip = binClip, canceled = m_studioCanceled, producer = std::move(producer), task] {
                if (canceled->loadAcquire() || pCore->taskManager.isBlocked() || !StudioJobs::currentClip(clip)) return;
                clip->setProducer(producer, true);
                if (task) Q_EMIT task->taskDone();
            }, Qt::QueuedConnection);''')
    replace('src/jobs/cliploadtask.cpp', '            Q_EMIT taskDone();\n            return;', '            return;')
    replace('src/jobs/cliploadtask.cpp', '''                pCore->bin()->shouldCheckProfile = false;
                QMetaObject::invokeMethod(pCore->bin(), "slotCheckProfile", Qt::QueuedConnection, Q_ARG(QString, QString::number(m_owner.itemId)));''', '''                QMetaObject::invokeMethod(pCore->bin(), [clip = binClip, canceled = m_studioCanceled] {
                    if (canceled->loadAcquire() || pCore->taskManager.isBlocked() || !StudioJobs::currentClip(clip)) return;
                    pCore->bin()->shouldCheckProfile = false;
                    pCore->bin()->slotCheckProfile(clip->clipId());
                }, Qt::QueuedConnection);''')
    path = Path('src/jobs/cliploadtask.cpp')
    original_abort = path.read_text(encoding='utf-8').split('void ClipLoadTask::abort()', 1)[1]
    replace(path, 'void ClipLoadTask::abort()' + original_abort, '''void ClipLoadTask::abort()
{
    m_progress = 100;
    if (m_softDelete || m_thumbOnly || m_isCanceled || pCore->taskManager.isBlocked() || !m_studioClip) return;
    QMetaObject::invokeMethod(m_studioClip.get(), [clip = m_studioClip, canceled = m_studioCanceled] {
        if (canceled->loadAcquire() || pCore->taskManager.isBlocked() || !StudioJobs::currentClip(clip)) return;
        clip->setInvalid();
        if (!clip->isReloading) pCore->projectItemModel()->requestBinClipDeletionById(clip->clipId());
        else clip->setClipStatus(FileStatus::StatusMissing);
    }, Qt::QueuedConnection);
}
''')


def optimize_preview_renderer():
    # avformat's s=/width/height mutate its MLT profile. Effects must retain the
    # source profile so pixel coordinates scale with the requested frame size.
    replace('renderer/kdenlive_render.cpp',
            '            QScopedPointer<Mlt::Consumer> cons(\n                new Mlt::Consumer(profile,',
            '            Mlt::Profile outputProfile(mlt_profile_clone(profile.get_profile()));\n'
            '            QScopedPointer<Mlt::Consumer> cons(\n                new Mlt::Consumer(outputProfile,')


def fix_preview_track_lifecycle():
    rewrite_block('src/timeline2/view/previewmanager.cpp',
                  'void PreviewManager::disconnectTrack()', 'void PreviewManager::disable()', lambda _: '''void PreviewManager::disconnectTrack()
{
    // User track insertion/deletion shifts the cached preview index.
    for (int index = m_tractor->count() - 1; index >= 0; --index) {
        std::unique_ptr<Mlt::Producer> track(m_tractor->track(index));
        const char *id = track ? track->get("kdenlive:playlistid") : nullptr;
        if (id && (strcmp(id, "timeline_preview") == 0 || strcmp(id, "timeline_overlay") == 0)) {
            m_tractor->remove_track(index);
        }
    }
    m_previewTrackIndex = -1;
}

''')


def optimize_preview():
    optimize_preview_renderer()
    fix_preview_track_lifecycle()
    replace('src/monitor/videowidget.cpp', 'int VideoWidget::setProducer(const QString &file)\n{',
            'int VideoWidget::setProducer(const QString &file)\n{\n    // Join frame readers before replacing their producer.\n    stop();')
    replace('src/monitor/videowidget.cpp', '    pause();\n    m_producer.reset();',
            '    // Pausing keeps read-ahead alive; join it before reconnecting.\n    stop();\n    m_producer.reset();')
    replace('src/monitor/monitor.h', '    void seekPosition(int pos);', '    void seekPosition(int pos);\n    void playbackStateChanged(bool playing);')
    replace('src/monitor/monitor.cpp', '    m_playAction->setActive(play);',
            '    m_playAction->setActive(play);\n    Q_EMIT playbackStateChanged(play);', expected=2)
    replace('src/monitor/monitor.cpp', '    if (!play && KdenliveSettings::rewindOnStop()) {',
            '    Q_EMIT playbackStateChanged(play);\n    if (!play && KdenliveSettings::rewindOnStop()) {')
    replace('src/timeline2/view/previewmanager.h', '#pragma once', '#pragma once\n#include <QPoint>')
    replace('src/timeline2/view/timelinecontroller.h', '    int m_duration;', '    int m_duration;\n    QTimer m_studioIdle;')
    replace('src/timeline2/view/timelinecontroller.h', '#pragma once', '#pragma once\n#include <QTimer>')
    replace('src/timeline2/view/timelinecontroller.h', '    void stopPreviewRender();',
            '    void stopPreviewRender();\n    void stopAutomaticPreview();')
    replace('src/timeline2/view/timelinecontroller.cpp', '    m_disablePreview = pCore->currentDoc()->getAction(QStringLiteral("disable_preview"));', '''    m_studioIdle.setSingleShot(true);
    m_studioIdle.setInterval(3000);
    connect(this, &TimelineController::seeked, this, [this] { stopAutomaticPreview(); m_studioIdle.start(); });
    if (pCore->window()) connect(pCore->monitorManager()->projectMonitor(), &Monitor::playbackStateChanged, this, [this](bool playing) {
        stopAutomaticPreview();
        if (!playing) m_studioIdle.start();
    });
    connect(&m_studioIdle, &QTimer::timeout, this, [this] {
        if (!m_ready || !m_model || pCore->currentTimelineId() != m_model->uuid()
            || !pCore->currentDoc() || pCore->currentDoc()->closing || pCore->taskManager.isBlocked()
            || (m_disablePreview && m_disablePreview->isChecked())
            || !KdenliveSettings::autopreview() || pCore->taskManager.exporting()) return;
        if (KdenliveSettings::draginprogress() || (pCore->window() && pCore->monitorManager()->projectMonitor()->isPlaying())) { m_studioIdle.start(); return; }
        initializePreview();
        if (m_model->hasTimelinePreview()) {
            m_usePreview = true;
            m_model->previewManager()->requestAutomaticPreview(m_model->tractor()->position(), m_model->duration());
        }
    });
    connect(&pCore->taskManager, &TaskManager::exportChanged, this, [this](bool exporting) {
        if (exporting) m_studioIdle.stop(); else m_studioIdle.start();
    });
    m_disablePreview = pCore->currentDoc()->getAction(QStringLiteral("disable_preview"));''')
    replace('src/timeline2/view/timelinecontroller.cpp', '    m_model = model;', '''    m_model = model;
    connect(m_model.get(), &TimelineModel::invalidateZone, &m_studioIdle, qOverload<>(&QTimer::start));
    m_studioIdle.start();''')
    replace('src/timeline2/view/timelinecontroller.cpp', '    m_ready = false;\n    m_root = nullptr;', '    m_studioIdle.stop();\n    m_ready = false;\n    m_root = nullptr;')
    replace('src/timeline2/view/timelinecontroller.cpp', 'void TimelineController::initializePreview()\n{', '''void TimelineController::stopAutomaticPreview()
{
    m_studioIdle.stop();
    if (m_model && m_model->hasTimelinePreview()) m_model->previewManager()->stopAutomaticPreview();
}

void TimelineController::initializePreview()
{''')
    replace('src/timeline2/view/timelinetabs.cpp', 'void TimelineTabs::disconnectTimeline(TimelineWidget *timeline)\n{',
            'void TimelineTabs::disconnectTimeline(TimelineWidget *timeline)\n{\n    timeline->controller()->stopAutomaticPreview();')
    replace('src/timeline2/view/previewmanager.h', '    QProcess m_previewProcess;', '''    QProcess *m_previewProcess;
    bool m_studioAutomatic = true;
    QPoint m_studioWindow;
    quint64 m_studioRevision = 0, m_studioRenderRevision = 0;
    QVariantList m_studioRenderingChunks;''')
    replace('src/timeline2/view/previewmanager.h', '    void startPreviewRender();', '    void startPreviewRender(bool automatic = false);\n    void requestAutomaticPreview(int cursor, int duration);\n    void stopAutomaticPreview();')
    replace('src/timeline2/view/previewmanager.cpp', '#include "previewmanager.h"', '#include "previewmanager.h"\n#include "assets/studio/studiojobs.hpp"\n#include "monitor/monitor.h"\n#include "monitor/monitormanager.h"\n#include <QScopeGuard>')
    path = Path('src/timeline2/view/previewmanager.cpp')
    data = path.read_text(encoding='utf-8').replace('&m_previewProcess', 'm_previewProcess').replace('m_previewProcess.', 'm_previewProcess->')
    path.write_text(data, encoding='utf-8', newline='\n')
    replace(path, '    m_previewGatherTimer.setSingleShot(true);', '    m_previewProcess = new QProcess(this);\n    m_previewGatherTimer.setSingleShot(true);')
    replace(path, '    connect(m_previewProcess, &QProcess::readyReadStandardError, this, &PreviewManager::receivedStderr);', '''    connect(m_previewProcess, &QProcess::readyReadStandardError, this, &PreviewManager::receivedStderr);
    connect(m_previewProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        m_errorLog = m_previewProcess->errorString();
        processEnded(-1, QProcess::CrashExit);
    });''')
    replace(path, '    if (doc->getDocumentProperty(QStringLiteral("resizepreview")).toInt() != 0) {', '''    if (m_consumerParams.removeAll(QStringLiteral("scale=0.25")) > 0) {
        m_consumerParams << QStringLiteral("s=%1x%2").arg(pCore->getProjectProfile().width() / 4).arg(pCore->getProjectProfile().height() / 4);
    } else if (doc->getDocumentProperty(QStringLiteral("resizepreview")).toInt() != 0) {''')
    replace(path, '    delete m_overlayTrack;\n    delete m_previewTrack;',
            '    StudioJobs::dispose(m_previewProcess);\n    delete m_overlayTrack;\n    delete m_previewTrack;')
    replace(path, '    connect(&m_previewTimer, &QTimer::timeout, this, &PreviewManager::startPreviewRender);',
            '    connect(&m_previewTimer, &QTimer::timeout, this, [this] { startPreviewRender(m_studioAutomatic); });')
    rewrite_block(path, 'void PreviewManager::abortRendering()', 'void PreviewManager::receivedStderr()', lambda _: r'''void PreviewManager::abortRendering()
{
    ++m_studioRevision;
    m_warnOnCrash = false;
    StudioJobs::cancel(m_previewProcess);
    Q_EMIT previewRender(-1, QString(), 1000);
}

bool PreviewManager::hasDefinedRange() const
{
    return !m_renderedChunks.isEmpty() || !m_dirtyChunks.isEmpty();
}

void PreviewManager::requestAutomaticPreview(int cursor, int duration)
{
    if (duration <= 0 || pCore->taskManager.exporting()) return;
    if (!m_studioAutomatic && (StudioJobs::busy(m_previewProcess) || m_previewTimer.isActive())) return;
    const int radius = qRound(5 * pCore->getCurrentFps());
    m_studioWindow = QPoint(qMax(0, cursor - radius), qMin(duration - 1, cursor + radius));
    m_studioAutomatic = true;
    pCore->currentDoc()->getTimeline(m_uuid)->buildPreviewTrack();
    addPreviewRange(m_studioWindow, true);
    m_previewTimer.stop();
    startPreviewRender(true);
}

void PreviewManager::stopAutomaticPreview()
{
    if (!m_studioAutomatic) return;
    m_previewTimer.stop();
    if (StudioJobs::busy(m_previewProcess)) abortRendering();
}

void PreviewManager::startPreviewRender(bool automatic)
{
    QMutexLocker lock(&m_previewMutex);
    if (!pCore->currentDoc() || pCore->currentDoc()->closing || pCore->taskManager.isBlocked() || pCore->taskManager.exporting()) return;
    if (automatic && pCore->currentTimelineId() != m_uuid) { stopAutomaticPreview(); return; }
    if (automatic && !m_studioAutomatic && (StudioJobs::busy(m_previewProcess) || m_previewTimer.isActive())) return;
    m_studioAutomatic = automatic;
    if (automatic && (KdenliveSettings::draginprogress() || (pCore->window() && pCore->monitorManager()->projectMonitor()->isPlaying()))) { m_previewTimer.start(); return; }
    if (StudioJobs::busy(m_previewProcess)) {
        abortRendering();
        m_previewTimer.start();
        return;
    }
    if (m_dirtyChunks.isEmpty()) return;
    m_waitingThumbs.clear();
    m_errorLog.clear();
    const QString scene = m_cacheDir.absoluteFilePath(QStringLiteral("preview.mlt"));
    disconnectTrack();
    const auto reconnect = qScopeGuard([this] { reconnectTrack(); });
    const QString playlist = pCore->projectItemModel()->sceneList(m_cacheDir.absolutePath(), QString(), pCore->currentDoc()->getTimeline(m_uuid)->tractor(), -1).first;
    QDomDocument document;
    if (!document.setContent(playlist)) return;
    if (!KdenliveSettings::proxypreview() && pCore->currentDoc()->useProxy()) KdenliveDoc::useOriginals(document);
    if (!Xml::docContentToFile(document, scene)) return;
    m_studioRenderRevision = m_studioRevision;
    m_previewTimer.stop();
    doPreviewRender(scene);
}

''')
    replace(path, '    QStringList resultList = QString::fromLocal8Bit(m_previewProcess->readAllStandardError()).split',
            '    const QByteArray bytes = m_previewProcess->readAllStandardError();\n    if (m_studioRenderRevision != m_studioRevision) return;\n    QStringList resultList = QString::fromLocal8Bit(bytes).split')
    replace(path, '''    const QStringList dirtyChunks = getCompressedList(m_dirtyChunks);
    m_chunksToRender = m_dirtyChunks.count();''', '''    m_studioRenderingChunks = m_dirtyChunks;
    if (m_studioAutomatic) {
        const int size = KdenliveSettings::timelinechunks();
        m_studioRenderingChunks.removeIf([this, size](const QVariant &frame) {
            return frame.toInt() + size <= m_studioWindow.x() || frame.toInt() > m_studioWindow.y();
        });
    }
    if (m_studioRenderingChunks.isEmpty()) return;
    const QStringList dirtyChunks = getCompressedList(m_studioRenderingChunks);
    m_chunksToRender = m_studioRenderingChunks.count();''')
    replace(path, '''    m_previewProcess->start(KdenliveSettings::kdenliverendererpath(), args);
    if (m_previewProcess->waitForStarted()) {
        qDebug() << " -  - -STARTING PREVIEW JOBS . . . STARTED: " << args;
    }''', '''    m_previewProcess->setProgram(KdenliveSettings::kdenliverendererpath());
    m_previewProcess->setArguments(args);
    const bool automatic = m_studioAutomatic;
    StudioJobs::start(m_previewProcess, automatic ? 0 : 15, nullptr, automatic, [this, automatic] {
        if (!automatic) return true;
        if (pCore->currentTimelineId() == m_uuid && !KdenliveSettings::draginprogress()
            && (!pCore->window() || !pCore->monitorManager()->projectMonitor()->isPlaying())) return true;
        ++m_studioRevision;
        m_warnOnCrash = false;
        m_previewTimer.start(3000);
        return false;
    });''')
    replace(path, '    const QString sceneList = m_cacheDir.absoluteFilePath(QStringLiteral("preview.mlt"));\n    QFile::remove(sceneList);',
            '''    const QString sceneList = m_cacheDir.absoluteFilePath(QStringLiteral("preview.mlt"));
    QFile::remove(sceneList);
    if (m_studioRenderRevision != m_studioRevision) {
        for (const auto &chunk : m_studioRenderingChunks)
            if (!m_renderedChunks.contains(chunk)) m_cacheDir.remove(QStringLiteral("%1.%2").arg(chunk.toInt()).arg(m_extension));
    }''')
    replace(path, '    if (pCore->window() && (status == QProcess::QProcess::CrashExit || exitCode != 0)) {',
            '    if (pCore->window() && m_warnOnCrash && (status == QProcess::QProcess::CrashExit || exitCode != 0)) {')
    replace(path, '    invalidatePreviews();\n    if (KdenliveSettings::autopreview()) {',
            '    if (StudioJobs::busy(m_previewProcess)) { m_previewGatherTimer.start(); return; }\n    invalidatePreviews();\n    if (KdenliveSettings::autopreview()) {')
    replace(path, 'void PreviewManager::invalidatePreview(int startFrame, int endFrame)\n{',
            'void PreviewManager::invalidatePreview(int startFrame, int endFrame)\n{\n    if (StudioJobs::busy(m_previewProcess)) abortRendering();')
    replace(path, '    Q_EMIT abortPreview();\n    m_previewProcess->waitForFinished();', '    abortRendering();\n    m_warnOnCrash = true;')
    replace(path, '    return workingPreview >= 0 || m_previewProcess->state() != QProcess::NotRunning;',
            '    return workingPreview >= 0 || StudioJobs::busy(m_previewProcess);')


def optimize_export():
    replace('src/dialogs/renderwidget.cpp', 'void RenderWidget::startRendering(RenderJobItem *item)\n{', '''void RenderWidget::startRendering(RenderJobItem *item)
{
    if (!pCore->currentDoc() || pCore->currentDoc()->closing || pCore->taskManager.isBlocked()) return;
    item->setStatus(STARTINGJOB);
    pCore->taskManager.setExporting(true);
    if (!pCore->taskManager.backgroundIdle()) {
        const QString destination = item->data(1, Qt::DisplayRole).toString();
        QTimer::singleShot(100, this, [this, destination] {
            for (auto *candidate : m_view.running_jobs->findItems(destination, Qt::MatchExactly, 1)) {
                auto *pending = static_cast<RenderJobItem *>(candidate);
                if (pending->status() == STARTINGJOB) { startRendering(pending); return; }
            }
            pCore->taskManager.setExporting(runningJobsCount() > 0);
        });
        return;
    }''')
    rewrite_block('src/dialogs/renderwidget.cpp', 'void RenderWidget::setRenderStatus(', 'void RenderWidget::slotAbortCurrentJob()',
                  lambda block: block.replace('    if (!item) {\n        return;\n    }', '    if (!item) {\n        pCore->taskManager.setExporting(runningJobsCount() > 0);\n        return;\n    }').replace(
                      '    if (item) {\n        if (QFile::exists(dest + ".log")) {',
                      '    pCore->taskManager.setExporting(runningJobsCount() > 0);\n    if (item) {\n        if (QFile::exists(dest + ".log")) {'))
    replace('src/dialogs/renderwidget.cpp', '    if (!proc.startDetached()) {\n        item->setStatus(FAILEDJOB);',
            '    if (!proc.startDetached()) {\n        item->setStatus(FAILEDJOB);\n        pCore->taskManager.setExporting(runningJobsCount() > 0);')
    replace('src/dialogs/renderwidget.cpp', '            delete current;\n            slotCheckJob();',
            '            delete current;\n            pCore->taskManager.setExporting(runningJobsCount() > 0);\n            slotCheckJob();')


def main():
    # Runtime-only fields are intentionally absent from editable XML parameters.
    # Copy them when Kdenlive creates/loads child services, not only on selection.
    replace('src/effects/effectstack/model/effectitemmodel.cpp',
            '            m_childEffects.insert(childId, effect);',
            '''            if (m_assetId == QLatin1String("studio_background")) {
                for (const char *name : {"_sbg_mask_path", "_sbg_clip_in"})
                    effect->filter().set(name, m_asset->get(name));
            }
            m_childEffects.insert(childId, effect);''', expected=2)
    replace('src/effects/effectstack/model/effectstackmodel.cpp',
            '#include "effectstackmodel.hpp"',
            '#include "effectstackmodel.hpp"\n#include "assets/studio/studiohelpers.hpp"')
    # Clip timing belongs to the document model, including headless render/tests.
    replace('src/core.cpp',
            'int Core::getItemIn(const ObjectId &id)\n{\n    if (!m_guiConstructed) {\n        qWarning() << "GUI not build";',
            'int Core::getItemIn(const ObjectId &id)\n{\n    if (!currentDoc()) {')
    # Master effects need the final sequence duration before requires_in_out is applied.
    replace('src/timeline2/model/builders/meltBuilder.cpp',
            '    // Import master track effects\n'
            '    std::shared_ptr<Mlt::Service> serv = std::make_shared<Mlt::Service>(tractor.get_service());\n'
            '    timeline->importMasterEffects(serv);\n\n', '', expected=2)
    replace('src/timeline2/model/builders/meltBuilder.cpp',
            '    timeline->_resetView();',
            '    timeline->importMasterEffects(std::make_shared<Mlt::Service>(tractor.get_service()));\n'
            '    timeline->_resetView();', expected=2)
    replace('src/timeline2/model/builders/meltBuilder.cpp',
            '            // Pass track properties\n',
            '            // Pass track properties\n'
            '            if (audioTrack && playlist.get_int("kdenlive:studio_audio_track") == 1) {\n'
            '                timeline->setTrackProperty(tid, QStringLiteral("kdenlive:studio_audio_track"), QStringLiteral("1"));\n'
            '            }\n'
            '            if (audioTrack && playlist.get_int("kdenlive:studio_transition_sfx_track") == 1) {\n'
            '                timeline->setTrackProperty(tid, QStringLiteral("kdenlive:studio_transition_sfx_track"), QStringLiteral("1"));\n'
            '            }\n')
    replace('src/timeline2/model/trackmodel.cpp',
            '    if (name == QLatin1String("kdenlive:audio_track") || name == QLatin1String("hide")) {',
            '    if (name == QLatin1String("kdenlive:audio_track") || name == QLatin1String("hide")\n'
            '        || name == QLatin1String("kdenlive:studio_audio_track")\n'
            '        || name == QLatin1String("kdenlive:studio_transition_sfx_track")) {')
    replace('src/effects/effectstack/model/effectstackmodel.cpp',
            '        if (filter.property_exists("av.file")) {',
            '''        if (filter.property_exists("0") && QString::fromUtf8(filter.get("mlt_service")) == QLatin1String("frei0r.sunimo_text_studio")) {
            url = filter.get("0");
        } else if (filter.property_exists("track_path") && QString::fromUtf8(filter.get("mlt_service")) == QLatin1String("studio.camera")) {
            url = filter.get("track_path");
        } else if (filter.property_exists("av.file")) {''')
    replace('src/doc/kdenlivedoc.cpp', '#include "effects/effectsrepository.hpp"',
            '#include "effects/effectsrepository.hpp"\n#include "assets/studio/studiobackgroundsource.hpp"')
    replace('src/doc/kdenlivedoc.cpp',
            '    doc->initCacheDirs();\n\n    if (doc->m_document.documentElement()',
            '    doc->initCacheDirs();\n'
            '    StudioBackground::restoreMaskPaths(doc->m_document, doc->projectDataFolder(), doc->documentRoot());\n\n'
            '    if (doc->m_document.documentElement()')
    replace('src/project/projectmanager.cpp', '#include <QCryptographicHash>',
            '#include <QCryptographicHash>\n#include <QDir>\n#include <QDomDocument>\n#include <QFile>\n#include <QFileInfo>\n#include <QUrl>')
    replace('src/project/projectmanager.cpp', 'ProjectManager::ProjectManager(QObject *parent)', r'''namespace {
bool copyStudioAssetFile(const QString &source, const QString &destination)
{
    if (QFileInfo(source).absoluteFilePath() == QFileInfo(destination).absoluteFilePath()) return true;
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) return false;
    if (QFileInfo::exists(destination)) {
        QFile existing(destination);
        QCryptographicHash left(QCryptographicHash::Sha256), right(QCryptographicHash::Sha256);
        return existing.open(QIODevice::ReadOnly) && left.addData(&input) && right.addData(&existing)
            && left.result() == right.result();
    }
    QSaveFile copy(destination);
    bool ok = QDir().mkpath(QFileInfo(destination).absolutePath()) && copy.open(QIODevice::WriteOnly);
    while (ok && !input.atEnd()) {
        const QByteArray block = input.read(1024 * 1024);
        ok = input.error() == QFileDevice::NoError && copy.write(block) == block.size();
    }
    return ok && copy.commit();
}

bool copyStudioEffectAssets(QString &scene, KdenliveDoc *project, const QString &output, QMap<QString, QString> &paths, QString &error)
{
    QDomDocument document;
    if (!document.setContent(scene)) { error = QStringLiteral("описание проекта повреждено"); return false; }
    const QDir serializationRoot(document.documentElement().attribute(QStringLiteral("root")));
    bool changed = false;
    const auto filters = document.elementsByTagName(QStringLiteral("filter"));
    for (int i = 0; i < filters.count(); ++i) {
        auto filter = filters.at(i).toElement();
        const QString service = Xml::getXmlProperty(filter, QStringLiteral("mlt_service"));
        const bool text = service == QLatin1String("frei0r.sunimo_text_studio");
        if (!text && (service != QLatin1String("studio.camera")
            || Xml::getXmlProperty(filter, QStringLiteral("tracking"), QStringLiteral("0")).toInt() != 1)) continue;
        const QString parameter = text ? QStringLiteral("0") : QStringLiteral("track_path");
        const QDir target(QDir(text ? QFileInfo(output).absolutePath() : project->projectDataFolder(QFileInfo(output).absolutePath()))
                              .absoluteFilePath(text ? QStringLiteral("studio-text") : QStringLiteral("studio-tracks")));
        const QString stored = Xml::getXmlProperty(filter, parameter);
        const QUrl url(stored);
        QString source = url.isLocalFile() ? url.toLocalFile() : stored;
        if (QDir::isRelativePath(source)) source = serializationRoot.absoluteFilePath(source);
        const QString name = QFileInfo(source).fileName();
        if (stored.isEmpty() || name.isEmpty() || QFileInfo(name).suffix().compare(text ? QLatin1String("stxt") : QLatin1String("scam"), Qt::CaseInsensitive) != 0
            || !QFileInfo(source).isFile()) {
            error = (text ? QStringLiteral("сцена надписи %1 недоступна") : QStringLiteral("файл трекинга камеры %1 недоступен")).arg(stored);
            return false;
        }
        const QString destination = target.absoluteFilePath(name);
        if (!copyStudioAssetFile(source, destination)) {
            error = (text ? QStringLiteral("не удалось перенести сцену надписи %1: файл недоступен или конфликтует")
                          : QStringLiteral("не удалось перенести трек камеры %1: файл недоступен или конфликтует")).arg(name);
            return false;
        }
        Xml::setXmlProperty(filter, parameter, destination);
        paths.insert(stored, destination);
        paths.insert(source, destination);
        changed = true;
    }
    const QDir audioRoot(QDir(QFileInfo(output).absolutePath()).absoluteFilePath(QStringLiteral("studio-audio")));
    const auto copyAudio = [&](const QString &stored) {
        const QUrl url(stored);
        QString source = url.isLocalFile() ? url.toLocalFile() : stored;
        if (QDir::isRelativePath(source)) source = serializationRoot.absoluteFilePath(source);
        const QFileInfo info(source);
        if (stored.isEmpty() || !info.isFile()
            || (info.fileName() != QLatin1String("result.wav") && info.fileName() != QLatin1String("report.json"))) {
            error = QStringLiteral("файл обработанного звука %1 недоступен").arg(stored);
            return QString();
        }
        const QString destination = audioRoot.absoluteFilePath(info.dir().dirName() + QLatin1Char('/') + info.fileName());
        if (!copyStudioAssetFile(source, destination)) {
            error = QStringLiteral("не удалось перенести обработанный звук %1: файл недоступен или конфликтует").arg(stored);
            return QString();
        }
        paths.insert(stored, destination);
        paths.insert(source, destination);
        return destination;
    };
    // Undo can still need an older WAV after the visible result has been replaced.
    const auto history = project->property("_studioAudioAssetPaths").toMap();
    for (auto it = history.cbegin(); it != history.cend(); ++it) {
        const QString destination = copyAudio(it.value().toString());
        if (destination.isEmpty()) return false;
        paths.insert(it.key(), destination);
    }
    for (const QString &tag : {QStringLiteral("chain"), QStringLiteral("producer")}) {
        const auto producers = document.elementsByTagName(tag);
        for (int i = 0; i < producers.count(); ++i) {
            auto producer = producers.at(i).toElement();
            if (Xml::getXmlProperty(producer, QStringLiteral("studio:audio:role")) != QLatin1String("result")) continue;
            for (const QString &parameter : {QStringLiteral("resource"), QStringLiteral("studio:audio:report")}) {
                const QString destination = copyAudio(Xml::getXmlProperty(producer, parameter));
                if (destination.isEmpty()) return false;
                Xml::setXmlProperty(producer, parameter, destination);
            }
            changed = true;
        }
    }
    if (changed) scene = document.toString();
    return true;
}
}

ProjectManager::ProjectManager(QObject *parent)''')
    replace('src/project/dialogs/archivewidget.cpp',
            '        // properties for vidstab files\n        propertyProcessUrl(e, QStringLiteral("filename"), root);',
            '''        if (Xml::getXmlProperty(e, QStringLiteral("mlt_service")) == QLatin1String("studio.camera"))
            propertyProcessUrl(e, QStringLiteral("track_path"), root);
        if (Xml::getXmlProperty(e, QStringLiteral("mlt_service")) == QLatin1String("frei0r.sunimo_text_studio"))
            propertyProcessUrl(e, QStringLiteral("0"), root);
        // properties for vidstab files
        propertyProcessUrl(e, QStringLiteral("filename"), root);''')
    replace('src/project/dialogs/archivewidget.cpp',
            '    const auto timelineBinId = pCore->bin()->getUsedClipIds();',
            '''    const auto bin = pCore->bin();
    const auto timelineBinId = bin ? bin->getUsedClipIds() : QList<int>{};''')
    replace('src/bin/projectitemmodel.cpp', '#include <QIcon>', '#include <QDomDocument>\n#include <QIcon>')
    replace('src/bin/projectitemmodel.cpp',
            'const std::pair<QString, QString> ProjectItemModel::sceneList(',
            '''namespace {
QString studioEffectAbsolutePaths(QString playlist, const QString &root)
{
    if (!playlist.contains(QLatin1String("frei0r.sunimo_text_studio"))
        && !playlist.contains(QLatin1String("studio.camera"))) return playlist;
    QDomDocument document;
    if (!document.setContent(playlist)) return {};
    // MLT has already made paths relative to the XML consumer's root.
    const QString xmlRoot = document.documentElement().attribute(QStringLiteral("root"), root);
    if (xmlRoot.isEmpty()) return playlist;
    const QDir serializationRoot(xmlRoot);
    const auto filters = document.elementsByTagName(QStringLiteral("filter"));
    for (int i = 0; i < filters.count(); ++i) {
        const auto filter = filters.at(i).toElement();
        const QString service = Xml::getXmlProperty(filter, QStringLiteral("mlt_service"));
        const QString parameter = service == QLatin1String("frei0r.sunimo_text_studio") ? QStringLiteral("0")
            : service == QLatin1String("studio.camera") ? QStringLiteral("track_path") : QString();
        if (parameter.isEmpty()) continue;
        const QString path = Xml::getXmlProperty(filter, parameter);
        if (!path.isEmpty() && !QUrl(path).isLocalFile() && QDir::isRelativePath(path))
            Xml::setXmlProperty(filter, parameter, serializationRoot.absoluteFilePath(path));
    }
    return document.toString();
}
}

const std::pair<QString, QString> ProjectItemModel::sceneList(''')
    replace('src/bin/projectitemmodel.cpp',
            '        return {playlist, QString()};',
            '        return {studioEffectAbsolutePaths(playlist, root), QString()};')
    replace('src/bin/projectitemmodel.cpp',
            '    double targetAspectRatio = 16.0 / 9.0; // default to horizontal (16:9)',
            '''    // Aspect-ratio rendering reads this file instead of the returned XML string.
    if (!tempFile.seek(0)) return {};
    const QByteArray studioPlaylist = studioEffectAbsolutePaths(QString::fromUtf8(tempFile.readAll()), root).toUtf8();
    if (studioPlaylist.isEmpty() || !tempFile.resize(0) || !tempFile.seek(0)
        || tempFile.write(studioPlaylist) != studioPlaylist.size() || !tempFile.flush()) return {};

    double targetAspectRatio = 16.0 / 9.0; // default to horizontal (16:9)''')
    replace('src/bin/projectitemmodel.cpp',
            '    return {playlist, tempFile.fileName()};',
            '    return {studioEffectAbsolutePaths(playlist, root), tempFile.fileName()};')
    replace('src/assets/model/assetparametermodel.cpp', '#include <QDirIterator>',
            '#include <QDirIterator>\n#include <QFileInfo>\n#include <QUrl>')
    replace('src/assets/model/assetparametermodel.cpp',
            'void AssetParameterModel::internalSetParameter(const QString name, const QString paramValue, const QModelIndex &paramIndex)\n{',
            '''void AssetParameterModel::internalSetParameter(const QString name, QString paramValue, const QModelIndex &paramIndex)
{
    QString resolved = paramValue;
    if (pCore && m_assetId == QLatin1String("sunimo_text_studio") && name == QLatin1String("0")) {
        if (auto *doc = pCore->currentDoc(); doc && !doc->closing) {
            QVariantMap paths = doc->property("_studioTextAssetPaths").toMap();
            const auto known = paths.constFind(paramValue);
            if (known != paths.cend()) resolved = known.value().toString();
            for (const QString &path : {getParam(name), paramValue, resolved}) {
                if (path.isEmpty() || paths.contains(path)) continue;
                const QUrl url(path);
                QString source = url.isLocalFile() ? url.toLocalFile() : path;
                if (QDir::isRelativePath(source)) source = QDir(doc->documentRoot()).absoluteFilePath(source);
                const QFileInfo info(source);
                if (info.isFile() && info.suffix().compare(QLatin1String("stxt"), Qt::CaseInsensitive) == 0)
                    paths.insert(path, QDir::cleanPath(info.absoluteFilePath()));
            }
            doc->setProperty("_studioTextAssetPaths", paths);
        }
    }
    paramValue = resolved;''')
    replace('src/assets/model/assetparametermodel.cpp',
            '''    if (conversionSuccess) {
        m_asset->set(name.toLatin1().constData(), doubleValue);''',
            '''    if (conversionSuccess) {
        const bool exactStudioTime = (m_assetId == QLatin1String("card3d") && name == QLatin1String("17"))
            || (m_assetId == QLatin1String("studiofx") && name == QLatin1String("10"));
        if (exactStudioTime) {
            m_asset->set(name.toLatin1().constData(), paramValue.toUtf8().constData());
        } else {
            m_asset->set(name.toLatin1().constData(), doubleValue);
        }''')
    replace('src/effects/effectstack/model/effectitemmodel.cpp',
            'void EffectItemModel::setEffectStackEnabled(bool enabled)\n{',
            '''void EffectItemModel::setEffectStackEnabled(bool enabled)
{
    if (m_assetId == QLatin1String("sunimo_text_studio")) {
        const QString scene = getParam(QStringLiteral("0"));
        internalSetParameter(QStringLiteral("0"), scene);
        if (getParam(QStringLiteral("0")) != scene) Q_EMIT updateChildren({QStringLiteral("0")});
    }''')
    replace('src/bin/projectitemmodel.cpp', '#include "doc/kdenlivedoc.h"',
            '#include "doc/kdenlivedoc.h"\n#include "effects/effectstack/model/effectstackmodel.hpp"')
    replace('src/bin/projectitemmodel.cpp',
            '        m_allClipItems[clip->clipId().toInt()] = clipItem;',
            '''        m_allClipItems[clip->clipId().toInt()] = clipItem;
        if (auto stack = clipItem->getEffectStack()) stack->setEffectStackEnabled(stack->isStackEnabled());''')
    replace('src/project/dialogs/archivewidget.h', '    QString processMltFile(const QDomDocument &doc, const QString &destPrefix = QString());',
            '    friend struct StudioArchiveTests;\n    QString processMltFile(const QDomDocument &doc, const QString &destPrefix = QString());')
    replace('tests/CMakeLists.txt', 'set(KdenliveTest_SOURCES\n',
            'set_source_files_properties(studioregressiontest.cpp PROPERTIES COMPILE_OPTIONS "-O0")\n'
            'set(KdenliveTest_SOURCES\n    studioregressiontest.cpp\n')
    replace('tests/CMakeLists.txt', '  set_property(TARGET ${_targetname} PROPERTY CXX_STANDARD 14)\nendforeach()',
            '  set_property(TARGET ${_targetname} PROPERTY CXX_STANDARD 17)\nendforeach()\n'
            'target_link_libraries(studioregressiontest kdenliveLibplugin)\n'
            'qt_add_resources(studio_test_gui_SRCS ${CMAKE_SOURCE_DIR}/src/icons.qrc ${CMAKE_SOURCE_DIR}/src/uiresources.qrc)\n'
            'target_sources(studioregressiontest PRIVATE ${studio_test_gui_SRCS})')
    replace('tests/TestMain.cpp', '#include <QString>', '#include <QString>\n#include <QVariant>')
    replace('tests/TestMain.cpp', '''    pCore->cleanup();
    pCore->mediaUnavailable.reset();

    // global clean-up...
    // delete repo;
    pCore->projectItemModel()->clean();
    pCore->cleanup();''', '''    if (qApp->property("studioGuiShutdownComplete").toBool()) {
        // An opt-in GUI fixture closed MainWindow through its normal shutdown.
        // Its widgets and MLT factory are gone; follow the native app teardown.
        Core::clean();
    } else {
        pCore->cleanup();
        pCore->mediaUnavailable.reset();

        // Keep the upstream cleanup for ordinary model-only tests.
        pCore->projectItemModel()->clean();
        pCore->cleanup();
    }''')
    # Keep the friendly XML entry; the generated MLT entry has no curated UI.
    replace('src/effects/effectsrepository.cpp', '    init();', '''    init();
    if (exists(QStringLiteral("card3d"))) {
        m_assets.erase(QStringLiteral("frei0r.card3d"));
    }
    if (exists(QStringLiteral("studiofx"))) {
        m_assets.erase(QStringLiteral("frei0r.studiofx"));
    }
    if (exists(QStringLiteral("studio_lens"))) {
        m_assets.erase(QStringLiteral("frei0r.studio_lens"));
    }
    if (exists(QStringLiteral("studio_vintage"))) {
        m_assets.erase(QStringLiteral("frei0r.studio_vintage"));
    }
    if (exists(QStringLiteral("studio_background"))) {
        m_assets.erase(QStringLiteral("studio.background"));
    }''')
    replace('src/doc/documentvalidator.cpp', '#include <QApplication>', '#include <QApplication>\n#include <QDir>')
    replace('src/doc/documentvalidator.cpp',
            '    double version = -1;',
            '''    // Append-only Card 3D parameters preserve old project rendering.
    QDomNodeList studioFilters = m_doc.elementsByTagName(QStringLiteral("filter"));
    for (int i = 0; i < studioFilters.count(); ++i) {
        QDomElement filter = studioFilters.at(i).toElement();
        const QString service = Xml::getXmlProperty(filter, QStringLiteral("mlt_service"));
        const QString effectId = Xml::getXmlProperty(filter, QStringLiteral("kdenlive_id"));
        if (service == QLatin1String("frei0r.sunimo_text_studio")) {
            const QString scene = Xml::getXmlProperty(filter, QStringLiteral("0"));
            if (!scene.isEmpty() && QDir::isRelativePath(scene))
                Xml::setXmlProperty(filter, QStringLiteral("0"), QDir(mlt.attribute(QStringLiteral("root"))).absoluteFilePath(scene));
        }
        if (service == QLatin1String("frei0r.card3d") || effectId == QLatin1String("card3d")) {
            if (!Xml::hasXmlProperty(filter, QStringLiteral("8"))) {
                const double style = Xml::getXmlProperty(filter, QStringLiteral("0"), QStringLiteral("0")).toDouble();
                const double rounding = style < .25 ? .25 : style < .75 ? .04 : 0.;
                Xml::setXmlProperty(filter, QStringLiteral("8"), QString::number(rounding, 'g', 17));
                m_modified = true;
            }
            if (!Xml::hasXmlProperty(filter, QStringLiteral("9"))) {
                Xml::setXmlProperty(filter, QStringLiteral("9"), QStringLiteral("0"));
                m_modified = true;
            }
            if (!Xml::hasXmlProperty(filter, QStringLiteral("10"))) {
                Xml::setXmlProperty(filter, QStringLiteral("10"), QStringLiteral("0.35"));
                m_modified = true;
            }
            if (!Xml::hasXmlProperty(filter, QStringLiteral("11"))) {
                Xml::setXmlProperty(filter, QStringLiteral("11"), QStringLiteral("0"));
                m_modified = true;
            }
            if (!Xml::hasXmlProperty(filter, QStringLiteral("12")) || !Xml::hasXmlProperty(filter, QStringLiteral("13"))) {
                const int placement = qBound(0, qRound(Xml::getXmlProperty(filter, QStringLiteral("3"), QStringLiteral("0.16666666666666666")).toDouble() * 6), 6);
                const double positions[7][2] = {{.5, .5}, {0, .5}, {1, .5}, {0, 0}, {1, 0}, {0, 1}, {1, 1}};
                Xml::setXmlProperty(filter, QStringLiteral("12"), QString::number(positions[placement][0], 'g', 17));
                Xml::setXmlProperty(filter, QStringLiteral("13"), QString::number(positions[placement][1], 'g', 17));
                m_modified = true;
            }
            if (!Xml::hasXmlProperty(filter, QStringLiteral("14"))) {
                Xml::setXmlProperty(filter, QStringLiteral("14"), QStringLiteral("0"));
                Xml::setXmlProperty(filter, QStringLiteral("15"), QStringLiteral("0"));
                Xml::setXmlProperty(filter, QStringLiteral("16"), QStringLiteral("0.34"));
                Xml::setXmlProperty(filter, QStringLiteral("17"), QStringLiteral("0"));
                m_modified = true;
            }
        }
    }

    double version = -1;''')
    replace('src/assets/CMakeLists.txt', '  assets/assetpanel.cpp',
            '  assets/assetpanel.cpp\n  assets/studio/studiopanel.cpp\n  assets/studio/audio/studioaudio.cpp\n  assets/studio/subtitles/studiosubtitles.cpp\n  assets/studio/text/studiotext.cpp')
    replace('src/CMakeLists.txt', 'add_subdirectory(assets)',
            'add_subdirectory(assets)\nset_source_files_properties(assets/studio/text/studiotext.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")')
    replace('src/mainwindow.cpp', '#include "mainwindow.h"', '#include "mainwindow.h"\n#include "assets/studio/studiopanel.hpp"\n#include "assets/studio/studiobackgroundsource.hpp"')
    replace('src/mainwindow.h', '#include <QProcessEnvironment>', '#include <QProcessEnvironment>\n#include <QPointer>')
    replace('src/mainwindow.h', 'class AssetPanel;', 'class AssetPanel;\nclass StudioPanel;')
    replace('src/mainwindow.h', '    AssetPanel *m_assetPanel{nullptr};',
            '    AssetPanel *m_assetPanel{nullptr};\n    QPointer<StudioPanel> m_studioPanel;')
    replace('src/mainwindow.cpp', 'MainWindow::~MainWindow()\n{', '''MainWindow::~MainWindow()
{
    // The panel holds MLT effects. Release them before Factory::close unloads
    // plugin callbacks; QObject children would otherwise die after this body.
    delete m_studioPanel.data();''')
    replace('src/dialogs/subtitleedit.cpp',
            'void SubtitleEdit::setModel(std::shared_ptr<SubtitleModel> model)\n{',
            'void SubtitleEdit::setModel(std::shared_ptr<SubtitleModel> model)\n{\n    if (m_model == model) return;')
    replace('src/effects/effectstack/model/effectstackmodel.cpp',
            '#include "doc/docundostack.hpp"',
            '#include "doc/docundostack.hpp"\n#include "doc/kdenlivedoc.h"\n#include "bin/model/subtitlemodel.hpp"\n#include "timeline2/model/timelineitemmodel.hpp"')
    replace('src/effects/effectstack/model/effectstackmodel.cpp',
            '                    pCore->window()->slotInitSubtitle(subProperties, uuid);',
            '''                    if (pCore->window()) {
                        pCore->window()->slotInitSubtitle(subProperties, uuid);
                    } else if (auto document = pCore->currentDoc()) {
                        // The subtitle model must still be restored when loading without a GUI.
                        if (auto timeline = document->getTimeline(uuid, true)) {
                            timeline->createSubtitleModel()->loadProperties(subProperties);
                        }
                    }''')
    replace('src/mainwindow.cpp',
            '''void MainWindow::showSubtitleTrack()
{
    if (!getCurrentTimeline()->hasSubtitles() || !m_buttonSubtitleEditTool->isChecked()) {
        m_buttonSubtitleEditTool->setChecked(true);
        slotEditSubtitle();
    }
}''',
            '''void MainWindow::showSubtitleTrack()
{
    if (!getCurrentTimeline()->hasSubtitles() || !m_buttonSubtitleEditTool->isChecked()) {
        m_buttonSubtitleEditTool->setChecked(true);
        slotEditSubtitle();
    }
    // A Studio import may have created the model before the native dock was opened.
    auto subtitles = getCurrentTimeline()->model()->getSubtitleModel();
    if (subtitles) {
        pCore->subtitleWidget()->setModel(subtitles);
        getCurrentTimeline()->connectSubtitleModel(true);
    }
}''')
    replace('src/bin/model/subtitlemodel.cpp',
            '    ulong initialCount = m_subtitleList.size();',
            '    ulong initialCount = m_subtitleList.size();\n    const auto stylesBeforeImport = m_subtitleStyles;')
    replace('src/bin/model/subtitlemodel.cpp',
            '''    if (initialCount == m_subtitleList.size() && externalImport) {
        // Nothing imported
        pCore->displayMessage''',
            '''    if (initialCount == m_subtitleList.size() && externalImport) {
        // Nothing imported
        m_subtitleStyles = stylesBeforeImport;
        pCore->displayMessage''')
    replace('src/bin/model/subtitlemodel.cpp',
            '''    if (externalImport) {
        pCore->pushUndo(undo, redo, i18n("Edit subtitle"));
    }
}

void SubtitleModel::parseSubtitle''',
            '''    if (externalImport) {
        // ASS imports change styles as well as cues; both belong to one Undo step.
        const auto stylesAfterImport = m_subtitleStyles;
        const Fun cuesUndo = undo, cuesRedo = redo;
        undo = [this, stylesBeforeImport, cuesUndo]() {
            if (!cuesUndo()) return false;
            m_subtitleStyles = stylesBeforeImport;
            Q_EMIT modelChanged();
            return true;
        };
        redo = [this, stylesAfterImport, cuesRedo]() {
            m_subtitleStyles = stylesAfterImport;
            if (!cuesRedo()) return false;
            Q_EMIT modelChanged();
            return true;
        };
        pCore->pushUndo(undo, redo, i18n("Edit subtitle"));
    }
}

void SubtitleModel::parseSubtitle''')
    replace('src/bin/model/subtitlemodel.cpp',
            '''    } else {
        m_subtitleStyles[name] = style;
        Q_EMIT modelChanged();
    }
}

void SubtitleModel::deleteSubtitleStyle''',
            '''    } else {
        const auto before = m_subtitleStyles;
        Fun undo = [this, before]() { m_subtitleStyles = before; Q_EMIT modelChanged(); return true; };
        Fun redo = [this, name, style]() { m_subtitleStyles[name] = style; Q_EMIT modelChanged(); return true; };
        redo();
        pCore->pushUndo(undo, redo, i18n("Edit subtitle style"));
    }
}

void SubtitleModel::deleteSubtitleStyle''')
    replace('src/bin/model/subtitlemodel.cpp',
            '''    int id = getIdForStartPos(layer, GenTime(startframe, pCore->getCurrentFps()));
    Fun local_redo = [this, id, startframe, endframe]() {''',
            '''    int id = getIdForStartPos(layer, GenTime(startframe, pCore->getCurrentFps()));
    const SubtitleEvent originalEvent = getSubtitle(layer, GenTime(startframe, pCore->getCurrentFps()));
    Fun local_redo = [this, id, startframe, endframe]() {''')
    replace('src/bin/model/subtitlemodel.cpp',
            '''    Fun local_undo = [this, layer, id, startframe, endframe, text]() {
        addSubtitle(id, {layer, GenTime(startframe, pCore->getCurrentFps())},
                    SubtitleEvent(true, GenTime(endframe, pCore->getCurrentFps()), "Default", "", 0, 0, 0, "", text));''',
            '''    Fun local_undo = [this, layer, id, startframe, endframe, text, originalEvent]() {
        addSubtitle(id, {layer, GenTime(startframe, pCore->getCurrentFps())},
                    originalEvent);''')
    replace('src/mainwindow.cpp', '#include "widgets/progressbutton.h"', '#include "widgets/progressbutton.h"\n#include "xml/xml.hpp"')
    anchor = '    connect(pCore.get(), &Core::requestShowBinEffectStack, m_assetPanel, &AssetPanel::showEffectStack, Qt::QueuedConnection);'
    replace('src/mainwindow.cpp', anchor, '''    auto studio = new StudioPanel(this);
    m_studioPanel = studio;
    addDock(QStringLiteral("FactMontage"), QStringLiteral("video_studio"), studio,
            KDDockWidgets::Location_None, m_effectStackDock);
    connect(m_timelineTabs, &TimelineTabs::studioSelectionChanged, studio, &StudioPanel::refreshSelection, Qt::QueuedConnection);
    connect(m_timelineTabs, &QTabWidget::currentChanged, studio, [studio] { studio->refreshSelection(); }, Qt::QueuedConnection);
''' + anchor)
    replace('src/mainwindow.cpp', '    case QEvent::ApplicationPaletteChange:\n        if (m_assetPanel) {',
            '''    case QEvent::ApplicationPaletteChange:
        if (m_studioPanel) {
            QEvent paletteEvent(QEvent::ApplicationPaletteChange);
            QCoreApplication::sendEvent(m_studioPanel, &paletteEvent);
        }
        if (m_assetPanel) {''')
    replace('src/timeline2/view/timelinetabs.hpp', '    void fitZoom();', '    void fitZoom();\n    void studioSelectionChanged();')
    source = 'src/timeline2/view/timelinetabs.cpp'
    anchor = '    connect(timeline->controller(), &TimelineController::showItemEffectStack, this, &TimelineTabs::showItemEffectStack);'
    replace(source, anchor, anchor + '\n    connect(timeline->controller(), &TimelineController::selectionChanged, this, &TimelineTabs::studioSelectionChanged);')
    anchor = '    disconnect(timeline->controller(), &TimelineController::showItemEffectStack, this, &TimelineTabs::showItemEffectStack);'
    replace(source, anchor, anchor + '\n    disconnect(timeline->controller(), &TimelineController::selectionChanged, this, &TimelineTabs::studioSelectionChanged);\n    Q_EMIT studioSelectionChanged();')
    replace('src/main.cpp', 'aboutData.setDesktopFileName(QStringLiteral("org.kde.kdenlive"));',
            'aboutData.setDesktopFileName(QStringLiteral("local.VideoStudio.Kdenlive"));')
    replace('src/main.cpp', 'QByteArray("kdenlive"), i18n("Kdenlive"),',
            'QByteArray("kdenlive"), QStringLiteral("FactMontage (Kdenlive)"),')
    replace('src/icons.qrc', '<file alias="kdenlive.png">../data/icons/48-apps-kdenlive.png</file>',
            '<file alias="kdenlive.png">../data/icons/48-apps-kdenlive.png</file>\n'
            '        <file alias="factmontage.svg">../factmontage.svg</file>')
    replace('src/main.cpp', 'app.setWindowIcon(QIcon(QStringLiteral(":/pics/kdenlive.png")));',
            'app.setWindowIcon(QIcon(QStringLiteral(":/pics/factmontage.svg")));')
    replace('src/main.cpp', '    KDBusService programDBusService;', '''    // Register only our sandbox's service name. Keep legacy resource/config
    // lookup names after registration; Flatpak provides a separate config root.
    const auto resourceName = QCoreApplication::applicationName();
    const auto resourceDomain = QCoreApplication::organizationDomain();
    QCoreApplication::setApplicationName(QStringLiteral("Kdenlive"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("VideoStudio.local"));
    KDBusService programDBusService;
    QCoreApplication::setApplicationName(resourceName);
    QCoreApplication::setOrganizationDomain(resourceDomain);''')
    replace('src/mainwindow.cpp',
            '    QStringList externalEffectFiles = doc->extractExternalEffectFiles();',
            '''    QStringList externalEffectFiles = doc->extractExternalEffectFiles();
    // Archive only immutable masks that are referenced by this project.
    QDomDocument studioArchiveDocument;
    if (studioArchiveDocument.setContent(sceneData)) {
        const auto filters = studioArchiveDocument.elementsByTagName(QStringLiteral("filter"));
        for (int i = 0; i < filters.count(); ++i) {
            const auto filter = filters.at(i).toElement();
            if (Xml::getXmlProperty(filter, QStringLiteral("mlt_service")) != QLatin1String("studio.background")
                && Xml::getXmlProperty(filter, QStringLiteral("kdenlive_id")) != QLatin1String("studio_background")) continue;
            const QString asset = Xml::getXmlProperty(filter, QStringLiteral("mask_asset"));
            if (!asset.isEmpty() && QFileInfo(asset).fileName() == asset) {
                const QString path = StudioBackground::maskPath(asset, doc->projectDataFolder(), doc->documentRoot(), doc->url());
                if (!path.isEmpty()) externalEffectFiles << path;
            }
        }
        externalEffectFiles.removeDuplicates();
    }''')
    replace('src/render/renderrequest.cpp', '#include <QRegularExpression>',
            '#include <QRegularExpression>\n#include <QCryptographicHash>\n#include <QFile>\n#include <QFileInfo>\n#include "assets/studio/studiobackgroundsource.hpp"\n#include "profiles/profilemodel.hpp"')
    replace('src/render/renderrequest.cpp', 'RenderRequest::RenderRequest()\n{', r'''namespace {
QString studioBackgroundRecipe(const QDomElement &background, int quality, int fpsNum, int fpsDen, qlonglong sourceIn)
{
    static const QByteArray modelSha("2047b95bea344af15575f4279d6fac44bb07a42e7d537cf8d3265f15fe95cc82");
    QByteArray recipe = QByteArray("studio-background-recipe-v2\n") + QByteArray::number(quality) + ':'
        + QByteArray::number(fpsNum) + ':' + QByteArray::number(fpsDen) + ':' + QByteArray::number(sourceIn)
        + ':' + modelSha + '\n';
    const auto owner = background.parentNode().toElement();
    for (auto node = owner.firstChildElement(QStringLiteral("filter")); !node.isNull() && node != background;
         node = node.nextSiblingElement(QStringLiteral("filter"))) {
        const QString id = Xml::getXmlProperty(node, QStringLiteral("kdenlive_id"));
        if (id.isEmpty()) continue;
        recipe += id.toUtf8() + '\n'
            + (Xml::getXmlProperty(node, QStringLiteral("disable")) == QLatin1String("1") ? QByteArray("1\n") : QByteArray("0\n"));
        QMap<QString, QString> parameters;
        for (auto property = node.firstChildElement(QStringLiteral("property")); !property.isNull();
             property = property.nextSiblingElement(QStringLiteral("property"))) {
            const QString name = property.attribute(QStringLiteral("name"));
            if (name.isEmpty() || name == QLatin1String("mlt_service") || name == QLatin1String("kdenlive_id")
                || name == QLatin1String("in") || name == QLatin1String("out") || name == QLatin1String("disable")
                || name.startsWith(QLatin1Char('_')) || name.startsWith(QLatin1String("kdenlive:"))) continue;
            parameters.insert(name, property.text());
        }
        for (auto it = parameters.cbegin(); it != parameters.cend(); ++it) recipe += it.key().toUtf8() + '=' + it.value().toUtf8() + '\n';
    }
    return QString::fromLatin1(QCryptographicHash::hash(recipe, QCryptographicHash::Sha256).toHex());
}

QString validateStudioBackgrounds(QDomDocument &document)
{
    auto project = pCore->currentDoc();
    auto profile = pCore->getCurrentProfile().get();
    if (!project || !profile) return QStringLiteral("Проект недоступен");
    const auto filters = document.elementsByTagName(QStringLiteral("filter"));
    for (int i = 0; i < filters.count(); ++i) {
        auto filter = filters.at(i).toElement();
        if (Xml::getXmlProperty(filter, QStringLiteral("mlt_service")) != QLatin1String("studio.background")
            && Xml::getXmlProperty(filter, QStringLiteral("kdenlive_id")) != QLatin1String("studio_background")) continue;
        if (Xml::getXmlProperty(filter, QStringLiteral("disable")) == QLatin1String("1")
            || Xml::getXmlProperty(filter, QStringLiteral("method"), QStringLiteral("0")).toInt() != 1) continue;
        const QString asset = Xml::getXmlProperty(filter, QStringLiteral("mask_asset"));
        if (asset.isEmpty() || QFileInfo(asset).fileName() != asset) return QStringLiteral("некорректное имя маски");
        const QString path = StudioBackground::maskPath(asset, project->projectDataFolder(), project->documentRoot(), project->url());
        const int quality = Xml::getXmlProperty(filter, QStringLiteral("analysis_size"), QStringLiteral("0")).toInt();
        const QString sourceSha = Xml::getXmlProperty(filter, QStringLiteral("source_sha256"));
        const QString recipeSha = Xml::getXmlProperty(filter, QStringLiteral("recipe_sha256"));
        const QString sourcePath = StudioBackground::sourcePath(filter, project->documentRoot());
        if (sourcePath.isEmpty()) return QStringLiteral("исходный клип для маски %1 не найден в проекте").arg(asset);
        bool offsetOk = false;
        const qlonglong offset = Xml::getXmlProperty(filter, QStringLiteral("sample_offset"), QStringLiteral("0")).toLongLong(&offsetOk);
        const bool staticImage = Xml::getXmlProperty(filter, QStringLiteral("static_image"), QStringLiteral("0")) == QLatin1String("1");
        qlonglong in = 0, duration = 0;
        if (!StudioBackground::entryRange(filter, profile->frame_rate_num(), profile->frame_rate_den(), in, duration))
            return QStringLiteral("положение клипа с маской %1 не определено").arg(asset);
        if (!offsetOk || offset < 0 || in < offset
            || recipeSha != studioBackgroundRecipe(filter, quality, profile->frame_rate_num(), profile->frame_rate_den(), in - offset))
            return QStringLiteral("маска %1 устарела или не соответствует проекту").arg(asset);
        const StudioBackground::MaskRequest request{path, sourcePath, sourceSha, recipeSha, quality,
            profile->frame_rate_num(), profile->frame_rate_den(), profile->width(), profile->height(),
            offset, duration, staticImage};
        const QString error = StudioBackground::validateMask(request);
        if (!error.isEmpty()) return error;
        Xml::setXmlProperty(filter, QStringLiteral("_sbg_mask_path"), path);
        Xml::setXmlProperty(filter, QStringLiteral("_sbg_clip_in"), QString::number(in));
    }
    return {};
}
}

RenderRequest::RenderRequest()
{''')
    replace('src/render/renderrequest.cpp',
            '''    if (m_delayedRendering) {
        project->restoreRenderAssets();
    }

    const QUuid currentUuid''',
            '''    if (m_delayedRendering) {
        project->restoreRenderAssets();
    }
    const QString studioBackgroundError = validateStudioBackgrounds(doc);
    if (!studioBackgroundError.isEmpty()) {
        addErrorMessage(i18n("Экспорт остановлен: %1.", studioBackgroundError));
        return {};
    }
    modified = true;

    const QUuid currentUuid''')
    replace('src/doc/kdenlivedoc.h',
            '    void updateWorkFilesBeforeSave(const QString &newUrl = QString(), bool onRender = false);',
            '    bool copyStudioAssetsForSave(const QString &newUrl);\n    void updateWorkFilesBeforeSave(const QString &newUrl = QString(), bool onRender = false);')
    replace('src/project/projectmanager.cpp',
            'bool ProjectManager::saveFileAs(const QString &outputFileName, bool saveOverExistingFile, bool saveACopy)\n{',
            'bool ProjectManager::saveFileAs(const QString &outputFileName, bool saveOverExistingFile, bool saveACopy)\n{\n    if (!m_project->copyStudioAssetsForSave(outputFileName)) return false;')
    replace('src/project/projectmanager.cpp',
            '    m_project->updateWorkFilesAfterSave();\n    if (!m_project->saveSceneList(outputFileName, scene, saveOverExistingFile)) {',
            '''    QMap<QString, QString> assetPaths;
    QString assetError;
    if (!copyStudioEffectAssets(scene, m_project, outputFileName, assetPaths, assetError)) {
        pCore->displayMessage(i18n("Проект не сохранён: %1.", assetError), ErrorMessage);
        return false;
    }
    m_project->updateWorkFilesAfterSave();
    if (!m_project->saveSceneList(outputFileName, scene, saveOverExistingFile)) {''')
    replace('src/project/projectmanager.cpp',
            '    const QString scene = pCore->projectItemModel()->sceneList(saveFolder, QString(), m_activeTimelineModel->tractor(), duration).first;',
            '    QString scene = pCore->projectItemModel()->sceneList(saveFolder, QString(), m_activeTimelineModel->tractor(), duration).first;')
    replace('src/project/projectmanager.cpp', '    QSaveFile file(outputFileName);',
            '''    QMap<QString, QString> assetPaths;
    QString assetError;
    if (!copyStudioEffectAssets(scene, m_project, outputFileName, assetPaths, assetError)) {
        qWarning() << "Project assets were not saved:" << assetError;
        return false;
    }
    QSaveFile file(outputFileName);''')
    replace('src/project/projectmanager.cpp',
            '    if (!saveACopy) {\n        m_project->setUrl(url);',
            '''    if (!saveACopy) {
        m_project->setProperty("_studioTextSaveRoot", QDir(QFileInfo(outputFileName).absolutePath()).filePath(QStringLiteral("studio-text")));
        m_project->updateStudioEffectAssetPaths(assetPaths);
        m_project->setUrl(url);''')
    replace('src/doc/kdenlivedoc.h',
            '    bool copyStudioAssetsForSave(const QString &newUrl);',
            '    bool copyStudioAssetsForSave(const QString &newUrl);\n    void updateStudioEffectAssetPaths(const QMap<QString, QString> &paths);')
    replace('src/doc/kdenlivedoc.cpp', '#include "assets/studio/studiobackgroundsource.hpp"',
            '#include "assets/studio/studiobackgroundsource.hpp"\n#include "effects/effectstack/model/effectitemmodel.hpp"\n#include "effects/effectstack/model/effectstackmodel.hpp"')
    replace('src/doc/kdenlivedoc.cpp',
            'void KdenliveDoc::updateWorkFilesBeforeSave(const QString &newUrl, bool onRender)\n{',
            r'''void KdenliveDoc::updateStudioEffectAssetPaths(const QMap<QString, QString> &paths)
{
    const QString textRoot = property("_studioTextSaveRoot").toString();
    if (!textRoot.isEmpty()) {
        auto textPaths = property("_studioTextAssetPaths").toMap();
        // copyStudioAssetsForSave verified every historical scene before the project was saved.
        for (auto it = textPaths.begin(); it != textPaths.end(); ++it)
            it.value() = QDir(textRoot).absoluteFilePath(QFileInfo(it.value().toString()).fileName());
        setProperty("_studioTextAssetPaths", textPaths);
    }
    auto audioPaths = property("_studioAudioAssetPaths").toMap();
    for (auto it = audioPaths.begin(); it != audioPaths.end(); ++it) {
        const QString next = paths.value(it.key(), paths.value(it.value().toString()));
        if (!next.isEmpty()) it.value() = next;
    }
    setProperty("_studioAudioAssetPaths", audioPaths);
    if (paths.isEmpty()) return;
    const QDir sourceRoot(documentRoot());
    const auto assetPath = [&paths, &sourceRoot](const QString &old) {
        const QUrl url(old);
        QString absolute = url.isLocalFile() ? url.toLocalFile() : old;
        if (QDir::isRelativePath(absolute)) absolute = sourceRoot.absoluteFilePath(absolute);
        return paths.value(old, paths.value(absolute));
    };
    const auto updateStack = [&assetPath](const std::shared_ptr<EffectStackModel> &stack) {
        if (!stack) return;
        for (int row = 0; row < stack->rowCount(); ++row) {
            const auto item = stack->getEffectStackRow(row);
            for (const auto &leaf : item->getLeaves()) {
                const auto effect = std::dynamic_pointer_cast<EffectItemModel>(leaf);
                if (!effect) continue;
                const bool text = effect->getAssetId() == QLatin1String("sunimo_text_studio");
                if (!text && effect->getAssetId() != QLatin1String("studio_camera")) continue;
                const QString parameter = text ? QStringLiteral("0") : QStringLiteral("track_path");
                const QString old = effect->getParam(parameter);
                const QString next = assetPath(old);
                if (!next.isEmpty() && old != next) effect->setParameter(parameter, next, true);
            }
        }
    };
    if (const auto bin = pCore->projectItemModel()) {
        for (const QString &id : bin->getAllClipIds()) {
            const auto clip = bin->getClipByBinID(id);
            if (!clip) continue;
            updateStack(clip->getEffectStack());
            if (clip->getProducerProperty(QStringLiteral("studio:audio:role")) != QLatin1String("result")) continue;
            QMap<QString, QString> properties;
            for (const QString &parameter : {QStringLiteral("resource"), QStringLiteral("studio:audio:report")}) {
                const QString old = clip->getProducerProperty(parameter);
                const QString next = assetPath(old);
                if (!next.isEmpty() && next != old) properties.insert(parameter, next);
            }
            if (!properties.isEmpty()) clip->setProperties(properties, true);
        }
    }
    for (const auto &timeline : std::as_const(m_timelines)) {
        updateStack(timeline->getMasterEffectStackModel());
        for (int track : timeline->getAllTracksIds()) {
            updateStack(timeline->getTrackEffectStackModel(track));
            for (int clip : timeline->getItemsInRange(track, 0, -1, false)) {
                updateStack(timeline->getClipEffectStackModel(clip));
                const auto producer = timeline->getClipProducer(clip);
                if (!producer || QString::fromUtf8(producer->get("studio:audio:role")) != QLatin1String("result")) continue;
                for (const char *parameter : {"resource", "studio:audio:report"}) {
                    const QString next = assetPath(QString::fromUtf8(producer->get(parameter)));
                    if (!next.isEmpty()) producer->set(parameter, next.toUtf8().constData());
                }
            }
        }
    }
}

void KdenliveDoc::updateWorkFilesBeforeSave(const QString &newUrl, bool onRender)
{''')
    replace('src/doc/kdenlivedoc.cpp',
            'void KdenliveDoc::updateWorkFilesBeforeSave(const QString &newUrl, bool onRender)\n{',
            r'''bool KdenliveDoc::copyStudioAssetsForSave(const QString &newUrl)
{
    QStringList textSources;
    auto candidates = extractExternalEffectFiles();
    for (const auto &path : property("_studioTextAssetPaths").toMap()) candidates.append(path.toString());
    for (const QString &path : std::as_const(candidates)) {
        if (!path.endsWith(QLatin1String(".stxt"), Qt::CaseInsensitive)) continue;
        const QUrl url(path);
        QString source = url.isLocalFile() ? url.toLocalFile() : path;
        if (QDir::isRelativePath(source)) source = QDir(documentRoot()).absoluteFilePath(source);
        if (!QFileInfo(source).isFile()) {
            pCore->displayMessage(i18n("Проект не сохранён: сцена надписи %1 не найдена.", path), ErrorMessage);
            return false;
        }
        textSources.append(source);
    }
    for (const QString &folder : {QStringLiteral("studio-background"), QStringLiteral("studio-text")}) {
        const QString suffix = folder == QLatin1String("studio-text") ? QStringLiteral("*.stxt") : QStringLiteral("*.sbg");
        const QDir target(QDir(folder == QLatin1String("studio-text") ? QFileInfo(newUrl).absolutePath()
                                                                    : projectDataFolder(QFileInfo(newUrl).absolutePath())).absoluteFilePath(folder));
        QStringList assets = folder == QLatin1String("studio-text") ? textSources : QStringList{};
        QStringList sourceFolders{QDir(documentRoot()).absoluteFilePath(QStringLiteral("others")),
                                  QDir(projectDataFolder()).absoluteFilePath(folder)};
        if (folder == QLatin1String("studio-background")) {
            sourceFolders.append(QDir(documentRoot()).absoluteFilePath(folder));
            if (url().isLocalFile() && !url().toLocalFile().isEmpty()) {
                const QDir savedRoot(QFileInfo(url().toLocalFile()).absolutePath());
                sourceFolders.append(savedRoot.absoluteFilePath(QStringLiteral("others")));
                sourceFolders.append(savedRoot.absoluteFilePath(folder));
            }
        }
        for (const QString &sourceFolder : std::as_const(sourceFolders)) {
            const QDir source(sourceFolder);
            for (const QString &asset : source.entryList({suffix}, QDir::Files)) assets.append(source.absoluteFilePath(asset));
        }
        assets.removeDuplicates();
        for (const QString &asset : std::as_const(assets)) {
            const QString destination = target.absoluteFilePath(QFileInfo(asset).fileName());
            if (QFileInfo(asset).absoluteFilePath() == QFileInfo(destination).absoluteFilePath()) continue;
            QFile input(asset);
            bool ok = input.open(QIODevice::ReadOnly);
            if (ok && QFileInfo::exists(destination)) {
                QFile existing(destination);
                QCryptographicHash left(QCryptographicHash::Sha256), right(QCryptographicHash::Sha256);
                ok = existing.open(QIODevice::ReadOnly) && left.addData(&input) && right.addData(&existing) && left.result() == right.result();
            } else if (ok) {
                QSaveFile output(destination);
                ok = QDir().mkpath(target.absolutePath()) && output.open(QIODevice::WriteOnly);
                while (ok && !input.atEnd()) {
                    const QByteArray chunk = input.read(1024 * 1024);
                    ok = input.error() == QFileDevice::NoError && output.write(chunk) == chunk.size();
                }
                ok = ok && output.commit();
            }
            if (!ok) {
                pCore->displayMessage(i18n("Проект не сохранён: не удалось перенести файл %1. Проверьте свободное место и права доступа.", QFileInfo(asset).fileName()), ErrorMessage);
                return false;
            }
        }
    }
    return true;
}

void KdenliveDoc::updateWorkFilesBeforeSave(const QString &newUrl, bool onRender)
{''')
    replace('src/effects/effectstack/model/effectstackmodel.hpp',
            '    void cleanFadeEffects(bool outEffects, Fun &undo, Fun &redo);',
            '''    void cleanFadeEffects(bool outEffects, Fun &undo, Fun &redo);
    /** Preserve absolute Studio Camera and Studio FX time when a clip is split. */
    void preserveStudioCameraCut(const std::shared_ptr<EffectStackModel> &right, int cutOffset, int originalDuration, Fun &undo, Fun &redo);''')
    replace('src/effects/effectstack/model/effectstackmodel.cpp',
            'bool EffectStackModel::fromXml(const QDomElement &effectsXml, Fun &undo, Fun &redo)\n{',
            '''namespace {
QString studioCardExitEnd(int duration)
{
    const double seconds = duration > 1 ? (duration - 1) / pCore->getCurrentFps() : 0.0;
    return QString::number(seconds / 21600.0, 'g', 17);
}
}

bool EffectStackModel::fromXml(const QDomElement &effectsXml, Fun &undo, Fun &redo)
{''')
    replace('src/effects/effectstack/model/effectstackmodel.cpp',
            '        effect->setParameters(parameters);\n        Fun local_undo = removeItem_lambda(effect->getId());',
            '''        effect->setParameters(parameters);
        if (effectId == QLatin1String("card3d")) {
            effect->setParameter(QStringLiteral("17"), studioCardExitEnd(pCore->getItemDuration(m_ownerId)), true);
        }
        Fun local_undo = removeItem_lambda(effect->getId());''')
    replace('src/effects/effectstack/model/effectstackmodel.cpp',
            '    effect->setParameters(sourceEffect->getAllParameters());\n    if (sourceEffect->isBuiltIn()) {',
            '''    effect->setParameters(sourceEffect->getAllParameters());
    if (effectId == QLatin1String("studio_background")) {
        // A copied human matte belongs to the original source. Timeline split
        // restores the identity below through preserveStudioCameraCut.
        effect->setParameter(QStringLiteral("mask_asset"), QString(), true);
        effect->setParameter(QStringLiteral("source_sha256"), QString(), true);
        effect->setParameter(QStringLiteral("recipe_sha256"), QString(), true);
        effect->setParameter(QStringLiteral("sample_offset"), QStringLiteral("0"), true);
    }
    if (effectId == QLatin1String("card3d")) {
        effect->setParameter(QStringLiteral("17"), studioCardExitEnd(pCore->getItemDuration(m_ownerId)), true);
    }
    if (sourceEffect->isBuiltIn()) {''')
    replace('src/effects/effectstack/model/effectstackmodel.cpp',
            '    if (params.contains(QLatin1String("kdenlive:builtin"))) {',
            '''    if (effectId == QLatin1String("card3d")) {
        effect->setParameter(QStringLiteral("17"), studioCardExitEnd(pCore->getItemDuration(m_ownerId)), true);
    }
    if (params.contains(QLatin1String("kdenlive:builtin"))) {''')
    replace('src/effects/effectstack/model/effectstackmodel.cpp',
            '''        PUSH_LAMBDA(local_redo, redo);
        PUSH_LAMBDA(local_undo, undo);
    } else if (makeCurrent) {''',
            '''        PUSH_LAMBDA(local_redo, redo);
        // Remove the effect while its owner still exists in a compound transaction.
        PUSH_FRONT_LAMBDA(local_undo, undo);
    } else if (makeCurrent) {''')
    replace('src/effects/effectstack/view/effectstackview.cpp',
            'void EffectStackView::loadEffects()\n{\n    QMutexLocker lock(&m_mutex);\n',
            '''void EffectStackView::loadEffects()
{
    QMutexLocker lock(&m_mutex);
    // Already queued row updates can arrive after unsetModel.
    if (!m_model) return;
''')
    replace('src/effects/effectstack/model/effectstackmodel.cpp',
            '        std::shared_ptr<EffectItemModel> effect = std::static_pointer_cast<EffectItemModel>(leaf);\n        if (fadeInDuration > 0',
            '''        std::shared_ptr<EffectItemModel> effect = std::static_pointer_cast<EffectItemModel>(leaf);
        if (effect->getAssetId() == QLatin1String("card3d")) {
            const QString previous = effect->getParam(QStringLiteral("17"));
            const QString next = studioCardExitEnd(duration);
            if (previous != next) {
                Fun operation = [effect, next]() {
                    effect->setParameter(QStringLiteral("17"), next, true);
                    return true;
                };
                Fun reverse = [effect, previous]() {
                    effect->setParameter(QStringLiteral("17"), previous, true);
                    return true;
                };
                operation();
                PUSH_LAMBDA(operation, redo);
                PUSH_LAMBDA(reverse, undo);
            }
        }
        if (effect->getAssetId() == QLatin1String("sunimo_text_studio") && m_ownerId.type == KdenliveObjectType::TimelineClip
            && pCore->getItemState(m_ownerId).second == ClipType::Text) {
            const QString previous = effect->getParam(QStringLiteral("1"));
            const QString next = QString::number((newIn + duration) / (pCore->getCurrentFps() * 120.0), 'g', 17);
            if (previous != next) {
                Fun operation = [effect, next]() {
                    effect->setParameter(QStringLiteral("1"), next, true);
                    return true;
                };
                Fun reverse = [effect, previous]() {
                    effect->setParameter(QStringLiteral("1"), previous, true);
                    return true;
                };
                operation();
                PUSH_LAMBDA(operation, redo);
                PUSH_LAMBDA(reverse, undo);
            }
        }
        if (effect->getAssetId() == QLatin1String("studiofx") && !adjustFromEnd && oldIn != newIn) {
            const QString previous = effect->getParam(QStringLiteral("10"));
            const double seconds = previous.toDouble() * 86400.0 + (newIn - oldIn) / pCore->getCurrentFps();
            const QString next = QString::number(seconds / 86400.0, 'g', 17);
            Fun operation = [effect, next]() {
                effect->setParameter(QStringLiteral("10"), next, true);
                return true;
            };
            Fun reverse = [effect, previous]() {
                effect->setParameter(QStringLiteral("10"), previous, true);
                return true;
            };
            operation();
            PUSH_LAMBDA(operation, redo);
            PUSH_LAMBDA(reverse, undo);
        }
        if (effect->getAssetId() == QLatin1String("studio_background") && !adjustFromEnd && oldIn != newIn
            && effect->getParam(QStringLiteral("method")).toInt() == 1) {
            const QString previous = effect->getParam(QStringLiteral("sample_offset"));
            const QString next = QString::number(previous.toLongLong() + newIn - oldIn);
            Fun operation = [effect, next, newIn]() {
                effect->setParameter(QStringLiteral("sample_offset"), next, true);
                StudioHelpers::setBackgroundClipIn(effect, newIn);
                pCore->invalidateItem(effect->getOwnerId());
                return true;
            };
            Fun reverse = [effect, previous, oldIn]() {
                effect->setParameter(QStringLiteral("sample_offset"), previous, true);
                StudioHelpers::setBackgroundClipIn(effect, oldIn);
                pCore->invalidateItem(effect->getOwnerId());
                return true;
            };
            operation();
            PUSH_LAMBDA(operation, redo);
            PUSH_LAMBDA(reverse, undo);
        }
        if (fadeInDuration > 0''')
    replace('src/effects/effectstack/model/effectstackmodel.cpp',
            'void EffectStackModel::cleanFadeEffects(bool outEffects, Fun &undo, Fun &redo)\n{',
            '''void EffectStackModel::preserveStudioCameraCut(const std::shared_ptr<EffectStackModel> &right, int cutOffset, int originalDuration,
                                                     Fun &undo, Fun &redo)
{
    QVector<std::shared_ptr<AssetParameterModel>> leftEffects, rightEffects;
    for (int i = 0; i < rootItem->childCount(); ++i) {
        auto effect = std::dynamic_pointer_cast<EffectItemModel>(rootItem->child(i));
        if (effect && effect->getAssetId() == QLatin1String("studio_camera")) leftEffects << effect;
    }
    for (int i = 0; i < right->rootItem->childCount(); ++i) {
        auto effect = std::dynamic_pointer_cast<EffectItemModel>(right->rootItem->child(i));
        if (effect && effect->getAssetId() == QLatin1String("studio_camera")) rightEffects << effect;
    }
    const int count = qMin(leftEffects.size(), rightEffects.size());
    for (int i = 0; i < count; ++i) {
        auto left = leftEffects[i];
        auto dest = rightEffects[i];
        const QString leftOrigin = left->getParam(QStringLiteral("studio_time_origin"));
        const QString leftSpan = left->getParam(QStringLiteral("studio_time_span"));
        const QString destOrigin = dest->getParam(QStringLiteral("studio_time_origin"));
        const QString destSpan = dest->getParam(QStringLiteral("studio_time_span"));
        const double origin = leftOrigin.toDouble();
        const double span = leftSpan.toDouble() > 0.0 ? leftSpan.toDouble() : qMax(1, originalDuration - 1);
        Fun operation = [left, dest, origin, span, cutOffset]() {
            left->setParameter(QStringLiteral("studio_time_origin"), QString::number(origin, 'g', 17), true);
            left->setParameter(QStringLiteral("studio_time_span"), QString::number(span, 'g', 17), true);
            dest->setParameter(QStringLiteral("studio_time_origin"), QString::number(origin + cutOffset, 'g', 17), true);
            dest->setParameter(QStringLiteral("studio_time_span"), QString::number(span, 'g', 17), true);
            return true;
        };
        Fun reverse = [left, dest, leftOrigin, leftSpan, destOrigin, destSpan]() {
            left->setParameter(QStringLiteral("studio_time_origin"), leftOrigin, true);
            left->setParameter(QStringLiteral("studio_time_span"), leftSpan, true);
            dest->setParameter(QStringLiteral("studio_time_origin"), destOrigin, true);
            dest->setParameter(QStringLiteral("studio_time_span"), destSpan, true);
            return true;
        };
        operation();
        UPDATE_UNDO_REDO_NOLOCK(operation, reverse, undo, redo);
    }
    QVector<std::shared_ptr<AssetParameterModel>> leftStudioFx, rightStudioFx;
    for (int i = 0; i < rootItem->childCount(); ++i) {
        auto effect = std::dynamic_pointer_cast<EffectItemModel>(rootItem->child(i));
        if (effect && effect->getAssetId() == QLatin1String("studiofx")) leftStudioFx << effect;
    }
    for (int i = 0; i < right->rootItem->childCount(); ++i) {
        auto effect = std::dynamic_pointer_cast<EffectItemModel>(right->rootItem->child(i));
        if (effect && effect->getAssetId() == QLatin1String("studiofx")) rightStudioFx << effect;
    }
    const double frameSeconds = cutOffset / pCore->getCurrentFps();
    const int fxCount = qMin(leftStudioFx.size(), rightStudioFx.size());
    for (int i = 0; i < fxCount; ++i) {
        auto dest = rightStudioFx[i];
        const QString previous = dest->getParam(QStringLiteral("10"));
        const double seconds = leftStudioFx[i]->getParam(QStringLiteral("10")).toDouble() * 86400.0 + frameSeconds;
        const QString next = QString::number(seconds / 86400.0, 'g', 17);
        Fun operation = [dest, next]() {
            dest->setParameter(QStringLiteral("10"), next, true);
            return true;
        };
        Fun reverse = [dest, previous]() {
            dest->setParameter(QStringLiteral("10"), previous, true);
            return true;
        };
        operation();
        UPDATE_UNDO_REDO_NOLOCK(operation, reverse, undo, redo);
    }
    QVector<std::shared_ptr<EffectItemModel>> leftBackground, rightBackground;
    for (int i = 0; i < rootItem->childCount(); ++i) {
        auto effect = std::dynamic_pointer_cast<EffectItemModel>(rootItem->child(i));
        if (effect && effect->getAssetId() == QLatin1String("studio_background")) leftBackground << effect;
    }
    for (int i = 0; i < right->rootItem->childCount(); ++i) {
        auto effect = std::dynamic_pointer_cast<EffectItemModel>(right->rootItem->child(i));
        if (effect && effect->getAssetId() == QLatin1String("studio_background")) rightBackground << effect;
    }
    const int backgroundCount = qMin(leftBackground.size(), rightBackground.size());
    const QStringList backgroundNames{QStringLiteral("mask_asset"), QStringLiteral("source_sha256"), QStringLiteral("recipe_sha256"),
                                      QStringLiteral("analysis_size"), QStringLiteral("static_image"), QStringLiteral("sample_offset")};
    for (int i = 0; i < backgroundCount; ++i) {
        auto source = leftBackground[i];
        auto dest = rightBackground[i];
        const int previousClipIn = dest->filter().get_int("_sbg_clip_in");
        const int nextClipIn = source->filter().get_int("_sbg_clip_in") + cutOffset;
        QStringList previous, next;
        for (const auto &name : backgroundNames) previous << dest->getParam(name);
        for (int n = 0; n < backgroundNames.size() - 1; ++n) next << source->getParam(backgroundNames[n]);
        next << QString::number(source->getParam(QStringLiteral("sample_offset")).toLongLong() + cutOffset);
        Fun operation = [dest, backgroundNames, next, nextClipIn]() {
            for (int n = 0; n < backgroundNames.size(); ++n) dest->setParameter(backgroundNames[n], next[n], true);
            StudioHelpers::setBackgroundClipIn(dest, nextClipIn);
            pCore->invalidateItem(dest->getOwnerId());
            return true;
        };
        Fun reverse = [dest, backgroundNames, previous, previousClipIn]() {
            for (int n = 0; n < backgroundNames.size(); ++n) dest->setParameter(backgroundNames[n], previous[n], true);
            StudioHelpers::setBackgroundClipIn(dest, previousClipIn);
            pCore->invalidateItem(dest->getOwnerId());
            return true;
        };
        operation();
        UPDATE_UNDO_REDO_NOLOCK(operation, reverse, undo, redo);
    }
}

void EffectStackModel::cleanFadeEffects(bool outEffects, Fun &undo, Fun &redo)
{''')
    replace('src/timeline2/model/timelinefunctions.cpp',
            '    res = res && (timeline->requestClipMove(newId, trackId, position, true, true, false, true, undo, redo) == TimelineModel::MoveSuccess);',
            '''    res = res && (timeline->requestClipMove(newId, trackId, position, true, true, false, true, undo, redo) == TimelineModel::MoveSuccess);
    if (res) {
        auto sourceStack = timeline->getClipEffectStackModel(clipId);
        auto destStack = timeline->getClipEffectStackModel(newId);
        sourceStack->preserveStudioCameraCut(destStack, newDuration, duration, undo, redo);
    }''')
    replace('src/timeline2/model/trackmodel.hpp',
            '''    bool requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations, bool updateView, bool finalMove, Fun &undo,
                        Fun &redo, bool groupMove);''',
            '''    bool requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations, bool updateView, bool finalMove, Fun &undo,
                        Fun &redo, bool groupMove);
    bool requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations,
                        const QVector<QPair<QString, QVariant>> &initialParameters, bool updateView, bool finalMove, Fun &undo, Fun &redo,
                        bool groupMove);''')
    replace('src/timeline2/model/trackmodel.hpp',
            '    void setMixDuration(int cid, int mixDuration, int mixCut);',
            '    void setStudioAudioDip(int cid, double amount);\n    void setMixDuration(int cid, int mixDuration, int mixCut);')
    replace('src/timeline2/model/trackmodel.cpp', '#include <mlt++/MltTransition.h>', '''#include <mlt++/MltTransition.h>
#include <mlt++/MltFilter.h>

namespace {
void appendStudioMixParameters(Mlt::Properties &transition, QVector<QPair<QString, QVariant>> &parameters)
{
    for (const QString &name : {QStringLiteral("studio:transition"), QStringLiteral("studio:audio_dip"),
                                QStringLiteral("studio:sfx_enabled"), QStringLiteral("studio:sfx_level"),
                                QStringLiteral("studio:sfx_source"),
                                QStringLiteral("studio:sfx_id")}) {
        if (!transition.property_exists(name.toUtf8().constData())) continue;
        const QVariant value = QString::fromUtf8(transition.get(name.toUtf8().constData()));
        auto existing = std::find_if(parameters.begin(), parameters.end(), [&name](const auto &item) { return item.first == name; });
        if (existing == parameters.end()) parameters.append({name, value});
        else existing->second = value;
    }
}

void syncStudioAudioDip(Mlt::Transition &transition)
{
    if (QString::fromUtf8(transition.get("mlt_service")) != QLatin1String("mix")) return;
    const double amount = qBound(0.0, transition.get_double("studio:audio_dip"), 1.0);
    const int duration = transition.get_length() - 1;
    std::unique_ptr<Mlt::Filter> filter;
    for (int i = 0; i < transition.filter_count(); ++i) {
        std::unique_ptr<Mlt::Filter> candidate(transition.filter(i));
        if (candidate && candidate->get_int("studio:audio_dip_filter") == 1) {
            filter = std::move(candidate);
            break;
        }
    }
    if (amount == 0.0 || duration < 3) {
        if (filter) transition.detach(*filter);
        return;
    }
    if (!filter) {
        filter = std::make_unique<Mlt::Filter>(transition.get_profile(), "volume");
        if (!filter->is_valid()) return;
        filter->set("studio:audio_dip_filter", 1);
        if (transition.attach(*filter) != 0) return;
    }
    filter->set_in_and_out(transition.get_in(), transition.get_out());
    const QString level = QStringLiteral("0=0;%1=%2;%3=0")
        .arg(duration / 2).arg(-12.0 * amount, 0, 'g', 6).arg(duration - 1);
    filter->set("level", level.toUtf8().constData());
}
} // namespace''')

    def track_mix(block):
        signature = '''bool TrackModel::requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations, bool updateView, bool finalMove, Fun &undo,
                                Fun &redo, bool groupMove)
{'''
        overload = '''bool TrackModel::requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations, bool updateView, bool finalMove, Fun &undo,
                                Fun &redo, bool groupMove)
{
    return requestClipMix(mixId, clipIds, mixDurations, {}, updateView, finalMove, undo, redo, groupMove);
}

bool TrackModel::requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations,
                                const QVector<QPair<QString, QVariant>> &initialParameters, bool updateView, bool finalMove, Fun &undo, Fun &redo,
                                bool groupMove)
{'''
        if block.count(signature) != 1:
            raise RuntimeError('trackmodel.cpp: requestClipMix signature changed')
        block = block.replace(signature, overload)
        block = block.replace('''    Fun build_mix = [clipIds, mixPosition, mixDurations, dest_track, secondClipCut, mixId, this]() {''',
                              '''    Fun build_mix = [clipIds, mixPosition, mixDurations, dest_track, secondClipCut, mixId, initialParameters, this]() {''')
        anchor = '''            std::shared_ptr<AssetParameterModel> asset(
                new AssetParameterModel(std::move(t), xml, assetName, ObjectId(KdenliveObjectType::TimelineMix, clipIds.second, ptr->uuid()), QString()));'''
        injected = '''            for (const auto &parameter : initialParameters) {
                if (!isAudioTrack() || parameter.first.startsWith(QLatin1String("studio:"))) {
                    t->set(parameter.first.toUtf8().constData(), parameter.second.toString().toUtf8().constData());
                    Xml::setXmlParameter(xml, parameter.first, parameter.second.toString());
                }
            }
            std::shared_ptr<AssetParameterModel> asset(
                new AssetParameterModel(std::move(t), xml, assetName, ObjectId(KdenliveObjectType::TimelineMix, clipIds.second, ptr->uuid()), QString()));'''
        if block.count(anchor) != 1:
            raise RuntimeError('trackmodel.cpp: mix construction changed')
        block = block.replace(anchor, injected.replace('''            std::shared_ptr<AssetParameterModel> asset(''',
                                                       '''            syncStudioAudioDip(*t);\n            std::shared_ptr<AssetParameterModel> asset('''))
        if block.count('newTrans->inherit(*props);') != 2:
            raise RuntimeError('trackmodel.cpp: mix recreation changed')
        return block.replace('newTrans->inherit(*props);', 'newTrans->inherit(*props);\n                        syncStudioAudioDip(*newTrans);')

    rewrite_block('src/timeline2/model/trackmodel.cpp',
                  'bool TrackModel::requestClipMix(', '\nvoid TrackModel::removeMix(', track_mix)
    replace('src/timeline2/model/trackmodel.cpp',
            'void TrackModel::setMixDuration(int cid, int mixDuration, int mixCut)\n{',
            '''void TrackModel::setStudioAudioDip(int cid, double amount)
{
    auto model = m_sameCompositions.at(cid);
    model->setParameter(QStringLiteral("studio:audio_dip"), QString::number(qBound(0.0, amount, 1.0), 'g', 17), true);
    syncStudioAudioDip(*static_cast<Mlt::Transition *>(model->getAsset()));
}

void TrackModel::setMixDuration(int cid, int mixDuration, int mixCut)
{''')
    replace('src/timeline2/model/trackmodel.cpp',
            '    transition.set_in_and_out(in, out);\n    Q_EMIT m_sameCompositions[cid]->dataChanged',
            '    transition.set_in_and_out(in, out);\n    syncStudioAudioDip(transition);\n    Q_EMIT m_sameCompositions[cid]->dataChanged')
    def remove_mix(block):
        block = block.replace('''        QVector<QPair<QString, QVariant>> params = m_sameCompositions[clipIds.second]->getAllParameters();''',
                              '''        QVector<QPair<QString, QVariant>> params = m_sameCompositions[clipIds.second]->getAllParameters();
        const double audioDip = m_sameCompositions[clipIds.second]->getAsset()->get_double("studio:audio_dip");
        const int studioTransition = m_sameCompositions[clipIds.second]->getAsset()->get_int("studio:transition");
        const int sfxEnabled = m_sameCompositions[clipIds.second]->getAsset()->get_int("studio:sfx_enabled");
        const double sfxLevel = m_sameCompositions[clipIds.second]->getAsset()->get_double("studio:sfx_level");
        const QString sfxSource = QString::fromUtf8(m_sameCompositions[clipIds.second]->getAsset()->get("studio:sfx_source"));
        const QString sfxId = QString::fromUtf8(m_sameCompositions[clipIds.second]->getAsset()->get("studio:sfx_id"));''')
        block = block.replace('''Fun reverse = [this, clipIds, assetId, params, tracks, mixDuration''',
                              '''Fun reverse = [this, clipIds, assetId, params, audioDip, studioTransition, sfxEnabled, sfxLevel, sfxSource, sfxId, tracks, mixDuration''')
        block = block.replace('''                t->set("kdenlive_id", assetId.toUtf8().constData());
                t->set_tracks''',
                              '''                t->set("kdenlive_id", assetId.toUtf8().constData());
                if (studioTransition) t->set("studio:transition", studioTransition);
                if (audioDip > 0.0) t->set("studio:audio_dip", audioDip);
                t->set("studio:sfx_enabled", sfxEnabled);
                t->set("studio:sfx_level", sfxLevel);
                t->set("studio:sfx_source", sfxSource.toUtf8().constData());
                t->set("studio:sfx_id", sfxId.toUtf8().constData());
                if (assetId == QLatin1String("mix")) { t->set("start", -1); t->set("accepts_blanks", 1); }
                syncStudioAudioDip(*t);
                t->set_tracks''')
        return block

    rewrite_block('src/timeline2/model/trackmodel.cpp',
                  'bool TrackModel::requestRemoveMix(',
                  '\nbool TrackModel::requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations, bool updateView',
                  remove_mix)
    def recreate_mix(block):
        anchor = '''        std::shared_ptr<AssetParameterModel> asset(
            new AssetParameterModel(std::move(t), xml, assetId'''
        if block.count(anchor) != 1:
            raise RuntimeError('trackmodel.cpp: createMix reconstruction changed')
        return block.replace(anchor, '''        for (const auto &parameter : params.second) {
            if (parameter.first.startsWith(QLatin1String("studio:")))
                t->set(parameter.first.toUtf8().constData(), parameter.second.toString().toUtf8().constData());
        }
        syncStudioAudioDip(*t);
''' + anchor)

    rewrite_block('src/timeline2/model/trackmodel.cpp',
                  'bool TrackModel::createMix(MixInfo info, std::pair<QString, QVector<QPair<QString, QVariant>>> params,',
                  '\nbool TrackModel::createMix(MixInfo info, bool isAudio)', recreate_mix)
    replace('src/timeline2/model/trackmodel.cpp',
            '''    const QString assetId = m_sameCompositions.at(cid)->getAssetId();
    QVector<QPair<QString, QVariant>> params = m_sameCompositions.at(cid)->getAllParameters();
    container.setAttribute''',
            '''    const QString assetId = m_sameCompositions.at(cid)->getAssetId();
    QVector<QPair<QString, QVariant>> params = m_sameCompositions.at(cid)->getAllParameters();
    appendStudioMixParameters(*m_sameCompositions.at(cid)->getAsset(), params);
    container.setAttribute''')
    replace('src/timeline2/model/trackmodel.cpp',
            '''    const QString assetId = m_sameCompositions[cid]->getAssetId();
    QVector<QPair<QString, QVariant>> params = m_sameCompositions[cid]->getAllParameters();
    return {assetId, params};''',
            '''    const QString assetId = m_sameCompositions[cid]->getAssetId();
    QVector<QPair<QString, QVariant>> params = m_sameCompositions[cid]->getAllParameters();
    appendStudioMixParameters(*m_sameCompositions[cid]->getAsset(), params);
    return {assetId, params};''')

    replace('tests/test_utils.hpp', '#include "doc/docundostack.hpp"',
            '#include "doc/docundostack.hpp"\n#include "doc/kdenlivedoc.h"')
    replace('tests/test_utils.hpp',
            '    static void resetNextId(int id = 0);',
            '''    static void resetNextId(int id = 0);
    static void studioDocumentRoot(KdenliveDoc &document, const QString &root)
    {
        document.m_document.documentElement().setAttribute(QStringLiteral("root"), root);
        document.loadDocumentProperties();
    }
    static std::shared_ptr<AssetParameterModel> studioAudioMix(const std::shared_ptr<TimelineModel> &timeline, int clipId)
    {
        return timeline->getTrackById(timeline->getClipTrackId(clipId))->mixModel(clipId);
    }
    static QVector<QPair<QString, QVariant>> studioAudioMixParams(const std::shared_ptr<TimelineModel> &timeline, int clipId)
    {
        return timeline->getTrackById(timeline->getClipTrackId(clipId))->getMixParams(clipId).second;
    }''')
    replace('src/timeline2/model/timelinemodel.hpp',
            '''    /** @brief Create a mix selection with currently selected clip. If delta = -1, mix with previous clip, +1 with next clip and 0 will check cursor position*/
    bool mixClip(int idToMove = -1, const QString &mixId = QStringLiteral("luma"), int delta = 0);
    Q_INVOKABLE bool resizeStartMix(int cid, int duration, bool singleResize);
    void requestResizeMix(int cid, int duration, MixAlignment align, int leftFrames = -1);''',
            '''    /** @brief Create a mix selection with currently selected clip. If delta = -1, mix with previous clip, +1 with next clip and 0 will check cursor position*/
    bool mixClip(int idToMove = -1, const QString &mixId = QStringLiteral("luma"), int delta = 0);
    bool mixClip(int idToMove, const QString &mixId, int delta, int duration,
                 const QVector<QPair<QString, QVariant>> &initialParameters, Fun *studioUndo = nullptr, Fun *studioRedo = nullptr);
    enum class StudioTransitionStatus { NoSelection, OneClip, TooManyClips, DifferentTracks, Gap, Locked, InsufficientFrames, SameSourceFrames,
                                        ExistingStudio, ExistingOther, LinkedAudioConflict, Ready };
    struct StudioTransitionSelection {
        StudioTransitionStatus status{StudioTransitionStatus::NoSelection};
        int first{-1}, second{-1}, track{-1}, audioFirst{-1}, audioSecond{-1}, audioTrack{-1}, maxDuration{0};
        int firstAvailable{0}, secondAvailable{0};
        bool audioMix{false}, sameFrames{false};
    };
    StudioTransitionSelection studioTransitionSelection(int requestedDuration = 1) const;
    std::shared_ptr<AssetParameterModel> studioTransitionModel(int secondClipId) const;
    bool updateStudioTransition(int duration, const QVector<QPair<QString, QVariant>> &parameters,
                                Fun *studioUndo = nullptr, Fun *studioRedo = nullptr);
    bool removeStudioTransition(Fun *studioUndo = nullptr, Fun *studioRedo = nullptr);
    Q_INVOKABLE bool resizeStartMix(int cid, int duration, bool singleResize);
    void requestResizeMix(int cid, int duration, MixAlignment align, int leftFrames = -1);
    bool requestResizeMix(int cid, int duration, MixAlignment align, int leftFrames, Fun &undo, Fun &redo);''')

    replace('src/timeline2/model/timelinemodel.cpp',
            '#include "timelinefunctions.hpp"',
            '#include "timelinefunctions.hpp"\n#include <limits>')

    def timeline_mix(block):
        signature = '''bool TimelineModel::mixClip(int idToMove, const QString &mixId, int delta)
{'''
        overload = '''bool TimelineModel::mixClip(int idToMove, const QString &mixId, int delta)
{
    return mixClip(idToMove, mixId, delta, pCore->getDurationFromString(KdenliveSettings::mix_duration()), {});
}

bool TimelineModel::mixClip(int idToMove, const QString &mixId, int delta, int requestedDuration,
                            const QVector<QPair<QString, QVariant>> &initialParameters, Fun *studioUndo, Fun *studioRedo)
{'''
        if block.count(signature) != 1:
            raise RuntimeError('timelinemodel.cpp: mixClip signature changed')
        block = block.replace(signature, overload)
        block = block.replace('''    int mixDuration = pCore->getDurationFromString(KdenliveSettings::mix_duration());''',
                              '''    const bool studioTransition = mixId == QLatin1String("studio_transition");
    int mixDuration = requestedDuration;''')
        block = block.replace('''    auto processMix = [this, &undo, &redo, mixDuration, mixId](mixStructure mInfo) {''',
                              '''    if (studioTransition) {
        const auto selected = studioTransitionSelection(mixDuration);
        if (selected.status != StudioTransitionStatus::Ready || selected.first != idToMove) return false;
        // Native Kdenlive owns overlap, playlist direction and linked audio.
        // Never extract/ripple unrelated tracks to manufacture source handles.
        if (mixDuration < 2 || delta != 1) return false;
    }

    auto processMix = [this, &undo, &redo, mixDuration, mixId, initialParameters, studioTransition](mixStructure mInfo) {''')
        block = block.replace('''        return requestClipMix(mixId, mInfo.clips, mInfo.durations, mInfo.selectedTrack, mInfo.mixPosition, true, true, true, undo, redo, false);''',
                              '''        if (studioTransition && (mInfo.durations.first + mInfo.durations.second != mixDuration
                                 || mInfo.durations.first < 0 || mInfo.durations.second < 0)) return false;
        return requestClipMix(mixId, mInfo.clips, mInfo.durations, mInfo.selectedTrack, mInfo.mixPosition, initialParameters,
                              true, true, true, undo, redo, false);''')
        block = block.replace('''            processMix(mixInfo);
            clipsToMixList << mixInfo;''',
                              '''            if (studioTransition) {
                if (!processMix(mixInfo)) { undo(); return false; }
            } else {
                processMix(mixInfo);
            }
            clipsToMixList << mixInfo;''')
        if block.count('if (!processMix(mixInfo))') != 2:
            raise RuntimeError('timelinemodel.cpp: expected two mix creation paths')
        block = block.replace('''    pCore->pushUndo(undo, redo, i18n("Create mix"));''',
                              '''    if (studioUndo && studioRedo) {
        Fun &groupUndo = *studioUndo, &groupRedo = *studioRedo;
        UPDATE_UNDO_REDO(redo, undo, groupUndo, groupRedo);
    } else pCore->pushUndo(undo, redo, i18n("Create mix"));''')
        return block

    rewrite_block('src/timeline2/model/timelinemodel.cpp',
                  'bool TimelineModel::mixClip(', '\nbool TimelineModel::requestClipMix(', timeline_mix)

    replace('src/timeline2/model/timelinemodel.hpp',
            '''    bool requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations, int trackId, int position, bool updateView,
                        bool invalidateTimeline, bool finalMove, Fun &undo, Fun &redo, bool groupMove);''',
            '''    bool requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations, int trackId, int position, bool updateView,
                        bool invalidateTimeline, bool finalMove, Fun &undo, Fun &redo, bool groupMove);
    bool requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations, int trackId, int position,
                        const QVector<QPair<QString, QVariant>> &initialParameters, bool updateView, bool invalidateTimeline, bool finalMove,
                        Fun &undo, Fun &redo, bool groupMove);''')

    replace('src/timeline2/model/timelinemodel.cpp',
            '''bool TimelineModel::requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations, int trackId, int position,
                                   bool updateView, bool invalidateTimeline, bool finalMove, Fun &undo, Fun &redo, bool groupMove)
{''',
            '''bool TimelineModel::requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations, int trackId, int position,
                                   bool updateView, bool invalidateTimeline, bool finalMove, Fun &undo, Fun &redo, bool groupMove)
{
    return requestClipMix(mixId, clipIds, mixDurations, trackId, position, {}, updateView, invalidateTimeline, finalMove, undo, redo, groupMove);
}

bool TimelineModel::requestClipMix(const QString &mixId, std::pair<int, int> clipIds, std::pair<int, int> mixDurations, int trackId, int position,
                                   const QVector<QPair<QString, QVariant>> &initialParameters, bool updateView, bool invalidateTimeline,
                                   bool finalMove, Fun &undo, Fun &redo, bool groupMove)
{''')
    replace('src/timeline2/model/timelinemodel.cpp',
            '''    ok = getTrackById(trackId)->requestClipMix(mixId, clipIds, mixDurations, updateView, finalMove, local_undo, local_redo, groupMove);''',
            '''    ok = getTrackById(trackId)->requestClipMix(mixId, clipIds, mixDurations, initialParameters, updateView, finalMove, local_undo, local_redo,
                                                        groupMove);''')

    def resize_mix(block):
        signature = '''void TimelineModel::requestResizeMix(int cid, int duration, MixAlignment align, int leftFrames)
{'''
        helper = '''bool TimelineModel::requestResizeMix(int cid, int duration, MixAlignment align, int leftFrames, Fun &undo, Fun &redo)
{'''
        if block.count(signature) != 1:
            raise RuntimeError('timelinemodel.cpp: requestResizeMix signature changed')
        block = block.replace(signature, helper)
        local = '''            Fun undo = []() { return true; };
            Fun redo = []() { return true; };
'''
        if block.count(local) != 1:
            raise RuntimeError('timelinemodel.cpp: resize undo setup changed')
        block = block.replace(local, '')
        block = block.replace('return;', 'return false;')
        # The caller owns the complete transaction, including any edits preceding this resize.
        # The original void method rolls back internally; doing that here would undo it twice.
        block = block.replace('                    undo();\n', '')
        block = block.replace('                    qDebug() << ":::: ERROR RESIZING CID1\\n\\nAAAAAAAAAAAAAAAAAAAA";',
                              '                    return false;')
        block = block.replace('                        qDebug() << ":::: ERROR RESIZING clipToResize\\n\\nAAAAAAAAAAAAAAAAAAAA";',
                              '                        return false;')
        tail = '''            pCore->pushUndo(undo, redo, i18n("Resize mix"));
        }
    }
}
'''
        replacement = '''            return true;
        }
    }
    return false;
}

void TimelineModel::requestResizeMix(int cid, int duration, MixAlignment align, int leftFrames)
{
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    if (requestResizeMix(cid, duration, align, leftFrames, undo, redo)) pCore->pushUndo(undo, redo, i18n("Resize mix"));
    else undo();
}
'''
        if block.count(tail) != 1:
            raise RuntimeError('timelinemodel.cpp: resize tail changed')
        return block.replace(tail, replacement)

    rewrite_block('src/timeline2/model/timelinemodel.cpp',
                  'void TimelineModel::requestResizeMix(', '\nQVariantList TimelineModel::getMasterEffectZones()', resize_mix)

    studio_methods = r'''
TimelineModel::StudioTransitionSelection TimelineModel::studioTransitionSelection(int requestedDuration) const
{
    StudioTransitionSelection result;
    std::vector<int> clips;
    for (int id : getCurrentSelection()) {
        if (!isClip(id)) continue;
        const int trackId = getClipTrackId(id);
        if (trackId > -1 && !isAudioTrack(trackId)) clips.push_back(id);
    }
    if (clips.empty()) return result;
    if (clips.size() == 1) { result.status = StudioTransitionStatus::OneClip; return result; }
    if (clips.size() != 2) { result.status = StudioTransitionStatus::TooManyClips; return result; }
    std::sort(clips.begin(), clips.end(), [this](int a, int b) { return getItemPosition(a) < getItemPosition(b); });
    result.first = clips[0]; result.second = clips[1];
    result.track = getClipTrackId(result.first);
    if (result.track != getClipTrackId(result.second)) { result.status = StudioTransitionStatus::DifferentTracks; return result; }
    if (trackIsLocked(result.track)) { result.status = StudioTransitionStatus::Locked; return result; }
    const auto track = getTrackById_const(result.track);
    const auto firstBin = pCore->projectItemModel()->getClipByBinID(getClipBinId(result.first));
    const auto secondBin = pCore->projectItemModel()->getClipByBinID(getClipBinId(result.second));
    const auto sourcePath = [](const std::shared_ptr<ProjectClip> &clip) {
        if (!clip || !clip->hasUrl() || clip->url().isEmpty()) return QString();
        // ProjectClip::url resolves kdenlive:originalurl when the producer uses a proxy.
        const QFileInfo file(clip->url());
        const QString canonical = file.canonicalFilePath();
        return canonical.isEmpty() ? file.absoluteFilePath() : canonical;
    };
    const QString firstPath = sourcePath(firstBin), secondPath = sourcePath(secondBin);
    const bool sameSource = getClipBinId(result.first) == getClipBinId(result.second)
        || (!firstPath.isEmpty() && firstPath == secondPath);
    const bool sameFrames = sameSource && firstBin && secondBin
        && !getClipPtr(result.first)->m_endlessResize && !getClipPtr(result.second)->m_endlessResize
        && firstBin->getProducerIntProperty(QStringLiteral("video_index")) == secondBin->getProducerIntProperty(QStringLiteral("video_index"))
        && qFuzzyCompare(getClipSpeed(result.first), getClipSpeed(result.second))
        && getClipIn(result.first) - getItemPosition(result.first) == getClipIn(result.second) - getItemPosition(result.second);
    if (track->hasEndMix(result.first) && track->hasStartMix(result.second)) {
        if (track->getMixInfo(result.second).first.firstClipId != result.first) {
            result.status = StudioTransitionStatus::ExistingOther;
            return result;
        }
        const auto model = track->mixModel(result.second);
        result.status = model && model->getAssetId() == QLatin1String("studio_transition")
            ? StudioTransitionStatus::ExistingStudio : StudioTransitionStatus::ExistingOther;
        result.sameFrames = sameFrames;
        result.maxDuration = qMax(0, getItemPlaytime(result.first) - getMixDuration(result.second)
                                     + getClipPtr(result.second)->getMixCutPosition() - 1);
        result.audioFirst = getClipSplitPartner(result.first);
        result.audioSecond = getClipSplitPartner(result.second);
        if (result.audioFirst > -1 && result.audioSecond > -1) {
            result.audioTrack = getClipTrackId(result.audioFirst);
            if (result.audioTrack > -1 && result.audioTrack == getClipTrackId(result.audioSecond)) {
                const auto audioModel = getTrackById_const(result.audioTrack)->mixModel(result.audioSecond);
                result.audioMix = audioModel && audioModel->getAsset()->get_int("studio:transition") == 1;
            }
        }
        return result;
    }
    if (track->hasEndMix(result.first) || track->hasStartMix(result.second)) {
        result.status = StudioTransitionStatus::ExistingOther;
        return result;
    }
    if (getItemPosition(result.first) + getItemPlaytime(result.first) != getItemPosition(result.second)) {
        result.status = StudioTransitionStatus::Gap;
        return result;
    }
    auto available = [this](int first, int second) {
        const auto left = getClipPtr(first);
        const auto right = getClipPtr(second);
        const int leftFrames = left->m_endlessResize ? right->getPlaytime()
            : qMin(right->getPlaytime(), left->getMaxDuration() - left->getOut() - 1);
        const int rightFrames = right->m_endlessResize ? left->getPlaytime() : qMin(left->getPlaytime(), right->getIn());
        int after = qMax(0, leftFrames), before = qMax(0, rightFrames);
        const auto track = getTrackById_const(getClipTrackId(first));
        if (track->hasStartMix(first))
            before = qMin(before, right->getPosition() - left->getPosition() - left->getMixDuration());
        if (track->hasEndMix(second)) {
            const int next = track->getMixInfo(second).second.secondClipId;
            if (next > -1) after = qMin(after, right->getPlaytime() - getMixDuration(next));
        }
        return std::pair<int, int>{qMax(0, after), qMax(0, before)};
    };
    const auto video = available(result.first, result.second);
    result.firstAvailable = video.first;
    result.secondAvailable = video.second;
    result.audioFirst = getClipSplitPartner(result.first);
    result.audioSecond = getClipSplitPartner(result.second);
    if (result.audioFirst > -1 && result.audioSecond > -1) {
        result.audioTrack = getClipTrackId(result.audioFirst);
        if (result.audioTrack < 0 || result.audioTrack != getClipTrackId(result.audioSecond)
            || trackIsLocked(result.audioTrack)
            || getItemPosition(result.audioFirst) + getItemPlaytime(result.audioFirst) != getItemPosition(result.audioSecond)) {
            result.status = StudioTransitionStatus::LinkedAudioConflict;
            return result;
        }
        const auto audioTrack = getTrackById_const(result.audioTrack);
        if (audioTrack->hasEndMix(result.audioFirst) || audioTrack->hasStartMix(result.audioSecond)) {
            result.status = StudioTransitionStatus::LinkedAudioConflict;
            return result;
        }
        const auto audio = available(result.audioFirst, result.audioSecond);
        result.firstAvailable = qMin(result.firstAvailable, audio.first);
        result.secondAvailable = qMin(result.secondAvailable, audio.second);
        result.audioMix = true;
    }
    // Source handles only: visible timeline media is never silently consumed.
    const qint64 handles = qint64(result.firstAvailable) + result.secondAvailable;
    result.maxDuration = int(qMin(handles, qint64(std::numeric_limits<int>::max())));
    result.sameFrames = sameFrames;
    if (sameFrames) { result.status = StudioTransitionStatus::SameSourceFrames; return result; }
    if (result.maxDuration < requestedDuration) { result.status = StudioTransitionStatus::InsufficientFrames; return result; }
    result.status = StudioTransitionStatus::Ready;
    return result;
}

std::shared_ptr<AssetParameterModel> TimelineModel::studioTransitionModel(int secondClipId) const
{
    if (!isClip(secondClipId)) return {};
    const int trackId = getClipTrackId(secondClipId);
    if (trackId < 0) return {};
    const auto model = getTrackById_const(trackId)->mixModel(secondClipId);
    return model && model->getAssetId() == QLatin1String("studio_transition") ? model : std::shared_ptr<AssetParameterModel>();
}

bool TimelineModel::updateStudioTransition(int duration, const QVector<QPair<QString, QVariant>> &parameters,
                                           Fun *studioUndo, Fun *studioRedo)
{
    const auto selection = studioTransitionSelection(duration);
    if (selection.status != StudioTransitionStatus::ExistingStudio) return false;
    if (!studioTransitionModel(selection.second)) return false;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    if (duration < 2) return false;
    // Parameter-only edits must not recenter a valid asymmetric/one-sided mix.
    // For duration changes use the existing native alignment and one transaction.
    const auto resize = [&](int cid) {
        if (getMixDuration(cid) == duration) return true;
        return requestResizeMix(cid, duration, getMixAlign(cid), -1, undo, redo)
            && getMixDuration(cid) == duration;
    };
    if (!resize(selection.second) || (selection.audioMix && !resize(selection.audioSecond))) {
        undo(); return false;
    }
    auto model = studioTransitionModel(selection.second);
    if (!model) { undo(); return false; }
    QVector<QPair<QString, QVariant>> effectiveParameters;
    for (const auto &parameter : parameters)
        if (parameter.first != QLatin1String("0")) effectiveParameters << parameter;
    // The user controls duration, never stale progress keyframe positions.
    effectiveParameters << qMakePair(QStringLiteral("0"), QVariant(QStringLiteral("0=0;%1=1").arg(duration - 1)));
    for (const auto &parameter : effectiveParameters) {
        if (parameter.first == QLatin1String("studio:transition")) continue;
        const QString previous = model->getParam(parameter.first);
        const QString next = parameter.second.toString();
        if (previous == next) continue;
        Fun operation = [model, name = parameter.first, next]() { model->setParameter(name, next, true); return true; };
        Fun reverse = [model, name = parameter.first, previous]() { model->setParameter(name, previous, true); return true; };
        operation();
        UPDATE_UNDO_REDO_NOLOCK(operation, reverse, undo, redo);
    }
    if (selection.audioMix) {
        const auto audioTrack = getTrackById(selection.audioTrack);
        const auto audioModel = audioTrack->mixModel(selection.audioSecond);
        for (const auto &parameter : effectiveParameters) {
            if (parameter.first != QLatin1String("studio:audio_dip")) continue;
            const double previous = audioModel->getAsset()->get_double("studio:audio_dip");
            const double next = qBound(0.0, parameter.second.toDouble(), 1.0);
            if (previous == next) continue;
            Fun operation = [audioTrack, cid = selection.audioSecond, next]() { audioTrack->setStudioAudioDip(cid, next); return true; };
            Fun reverse = [audioTrack, cid = selection.audioSecond, previous]() { audioTrack->setStudioAudioDip(cid, previous); return true; };
            operation();
            UPDATE_UNDO_REDO_NOLOCK(operation, reverse, undo, redo);
        }
    }
    if (studioUndo && studioRedo) {
        Fun &groupUndo = *studioUndo, &groupRedo = *studioRedo;
        UPDATE_UNDO_REDO(redo, undo, groupUndo, groupRedo);
    } else pCore->pushUndo(undo, redo, i18n("Update Studio transition"));
    return true;
}

bool TimelineModel::removeStudioTransition(Fun *studioUndo, Fun *studioRedo)
{
    const auto selection = studioTransitionSelection();
    if (selection.status != StudioTransitionStatus::ExistingStudio) return false;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    if (!getTrackById(selection.track)->requestRemoveMix({selection.first, selection.second}, undo, redo)) return false;
    if (selection.audioSecond > -1 && selection.audioTrack > -1) {
        const auto audioTrack = getTrackById(selection.audioTrack);
        const auto audioModel = audioTrack->mixModel(selection.audioSecond);
        if (audioModel && audioModel->getAsset()->get_int("studio:transition") == 1
            && !audioTrack->requestRemoveMix({selection.audioFirst, selection.audioSecond}, undo, redo)) {
            undo(); return false;
        }
    }
    if (studioUndo && studioRedo) {
        Fun &groupUndo = *studioUndo, &groupRedo = *studioRedo;
        UPDATE_UNDO_REDO(redo, undo, groupUndo, groupRedo);
    } else pCore->pushUndo(undo, redo, i18n("Remove Studio transition"));
    return true;
}
'''
    replace('src/timeline2/model/timelinemodel.cpp',
            '\nbool TimelineModel::mixClip(int idToMove, const QString &mixId, int delta)\n{',
            studio_methods + '\nbool TimelineModel::mixClip(int idToMove, const QString &mixId, int delta)\n{')
    optimize_tasks()
    optimize_document()
    optimize_job_commits()
    optimize_worker_processes()
    optimize_preview()
    optimize_export()
    print('Applied Studio integration to Kdenlive 26.08.0')


if __name__ == '__main__':
    main()
