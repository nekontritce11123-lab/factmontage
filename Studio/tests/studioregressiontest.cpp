// SPDX-License-Identifier: GPL-3.0-only
#include "catch.hpp"
#include "test_utils.hpp"
#include "doc/docundostack.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "kdenlivesettings.h"
#include "assets/studio/studiopanel.hpp"
#include "assets/studio/studiobackgroundsource.hpp"
#include "assets/studio/text/scenecompiler.hpp"
#include "assets/studio/text/studiotext.hpp"
#include "assets/studio/subtitles/studiosubtitles.hpp"
#include "project/dialogs/archivewidget.h"
#include "profiles/profilemodel.hpp"
#include "profiles/profilerepository.hpp"
#include "xml/xml.hpp"
#include "utils/uiutils.h"
#include <QElapsedTimer>
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QInputDialog>
#include <QLibrary>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QFontComboBox>
#include <QPushButton>
#include <QEvent>
#include <QEventLoop>
#include <QProcess>
#include <QScopeGuard>
#include <QComboBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QDataStream>
#include <QDoubleSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSettings>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabBar>
#include <QToolButton>
#include <QTimer>
#include <QTemporaryDir>
#include <QtPlugin>
#include <QUndoGroup>
#include <KActionCollection>
#include <mlt++/MltFrame.h>
#include <mlt++/MltFilter.h>
#include <mlt++/MltTransition.h>
#include "jobs/taskmanager.h"
#include "assets/studio/studiooptimization.hpp"
#include "assets/studio/studiocfr.hpp"
#include "assets/studio/studioworker.hpp"
#include "timeline2/view/previewmanager.h"
#include "bin/bin.h"
#include <atomic>
#include <cmath>
#include <QThread>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
// Compile the production controller here to exercise its private job lifecycle;
// kdenliveLib supplies the editor/model and Qt meta-object implementation.
#include "assets/studio/audio/studioaudio.cpp"

Q_IMPORT_PLUGIN(org_kde_kdenlivePlugin)

static bool studioWait(const std::function<bool()> &ready, int limit = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < limit) { QApplication::processEvents(); QThread::msleep(5); }
    return ready();
}

struct StudioAudioTests {
    static QSet<int> selected(const std::shared_ptr<TimelineModel> &model) {
        return StudioAudioController::audioSelection(model, nullptr);
    }
    static QSet<int> selectedTrack(const std::shared_ptr<TimelineModel> &model) {
        return StudioAudioController::audioTrackSelection(model, nullptr);
    }
    static void begin(StudioAudioController &controller, const QString &program, const QStringList &arguments = {}) {
        controller.m_job = std::make_unique<StudioAudioController::Job>();
        controller.m_job->outputId = QUuid::createUuid().toString();
        controller.m_job->commands.push_back({program, arguments, QStringLiteral("test")});
        controller.runNext();
    }
    static QProcess *process(StudioAudioController &controller) { return controller.m_process; }
    static void pendingImport(StudioAudioController &controller, int &rollbacks) {
        controller.m_job = std::make_unique<StudioAudioController::Job>();
        controller.m_job->rollbackImport = std::make_shared<Fun>([&rollbacks] { ++rollbacks; return true; });
    }
};

struct StudioArchiveTests {
    static QString rewrite(ArchiveWidget &archive, const QDomDocument &document, const QString &destination) {
        archive.compressed_archive->setChecked(false);
        archive.timeline_archive->setChecked(false);
        archive.archive_url->setUrl(QUrl::fromLocalFile(destination));
        return archive.processMltFile(document, destination + QLatin1Char('/'));
    }
    static bool start(ArchiveWidget &archive, const QString &destination) {
        archive.compressed_archive->setChecked(false);
        archive.timeline_archive->setChecked(false);
        archive.archive_url->setUrl(QUrl::fromLocalFile(destination));
        return archive.slotStartArchiving();
    }
};

TEST_CASE("Heavy native jobs share one serial lane and cancellation leaves the GUI responsive", "[Studio][Optimization]")
{
    struct Probe : AbstractTask {
        std::atomic<int> &active, &peak, &completed;
        Probe(int id, JOBTYPE type, std::atomic<int> &a, std::atomic<int> &p, std::atomic<int> &c)
            : AbstractTask(ObjectId(KdenliveObjectType::BinClip, id, {}), type, nullptr), active(a), peak(p), completed(c) {}
        void run() override {
            AbstractTaskDone done(m_owner.itemId, this);
            QMutexLocker lock(&m_runMutex);
            m_running = true;
            const int count = ++active;
            int old = peak;
            while (old < count && !peak.compare_exchange_weak(old, count)) {}
            QThread::msleep(120);
            --active; ++completed;
        }
    };
    auto &manager = pCore->taskManager;
    manager.unBlock();
    pCore->projectItemModel()->clean();
    KdenliveDoc document(std::make_shared<DocUndoStack>(nullptr));
    pCore->projectManager()->testSetDocument(&document);
    const auto closeDocument = qScopeGuard([&] {
        manager.unBlock();
        if (pCore->currentDoc() == &document) pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    pCore->projectManager()->testSetActiveTimeline(document.getTimeline(document.uuid()));
    std::atomic<int> active{0}, peak{0}, completed{0};
    for (int i = 0; i < 4; ++i)
        manager.startTask(9000 + i, new Probe(9000 + i, i % 2 ? AbstractTask::PROXYJOB : AbstractTask::MELTJOB, active, peak, completed));
    QElapsedTimer timeout;
    timeout.start();
    while (completed < 4 && timeout.elapsed() < 4000) { QApplication::processEvents(); QThread::msleep(5); }
    REQUIRE(completed == 4);
    REQUIRE(peak == 1);
    manager.startTask(9010, new Probe(9010, AbstractTask::MELTJOB, active, peak, completed));
    timeout.restart();
    while (active == 0 && timeout.elapsed() < 1000) { QApplication::processEvents(); QThread::msleep(1); }
    timeout.restart();
    manager.discardJobs(ObjectId(KdenliveObjectType::BinClip, 9010, {}));
    REQUIRE(timeout.elapsed() < 40);
    while (completed < 5 && timeout.elapsed() < 2000) { QApplication::processEvents(); QThread::msleep(5); }
    REQUIRE(completed == 5);
    manager.startTask(9011, new Probe(9011, AbstractTask::MELTJOB, active, peak, completed));
    REQUIRE(studioWait([&] { return active == 1; }));
    bool timerDelivered = false, nestedStayedBlocked = false, nestedCloseRejected = false;
    QTimer::singleShot(20, &document, [&] {
        timerDelivered = true;
        manager.slotCancelJobs();
        nestedStayedBlocked = manager.isBlocked();
        nestedCloseRejected = !pCore->projectManager()->closeCurrentDocument(false, false);
    });
    manager.slotCancelJobs(true);
    REQUIRE(timerDelivered);
    REQUIRE(nestedStayedBlocked);
    REQUIRE(nestedCloseRejected);
    REQUIRE(pCore->currentDoc() == &document);
    REQUIRE(active == 0);
    REQUIRE(manager.backgroundIdle());
    manager.unBlock();
    manager.startTask(9012, new Probe(9012, AbstractTask::PROXYJOB, active, peak, completed));
    REQUIRE(studioWait([&] { return active == 1; }));
    manager.slotCancelJobs(false, {AbstractTask::PROXYJOB});
    REQUIRE(active == 1);
    REQUIRE(studioWait([&] { return active == 0; }));
    const bool wasClosing = pCore->closing;
    const auto restoreClosing = qScopeGuard([wasClosing] { pCore->closing = wasClosing; });
    document.loading = true;
    REQUIRE_FALSE(pCore->projectManager()->closeCurrentDocument(false, true));
    REQUIRE_FALSE(manager.isBlocked());
    REQUIRE(pCore->currentDoc() == &document);
    document.loading = false;
    REQUIRE(pCore->projectManager()->closeCurrentDocument(false, false));
    REQUIRE(pCore->currentDoc() == nullptr);
}

TEST_CASE("Native worker deadlines stop hung processes and honor cancellation", "[Studio][Optimization]")
{
    QAtomicInt canceled(0);
    QProcess hung;
    hung.start(QStringLiteral("/bin/sleep"), {QStringLiteral("30")});
    QElapsedTimer elapsed;
    elapsed.start();
    REQUIRE_FALSE(StudioWorker::wait(hung, canceled, 30));
    REQUIRE(canceled.loadAcquire() == 1);
    REQUIRE(hung.state() == QProcess::NotRunning);
    REQUIRE(elapsed.elapsed() < 1500);
    QProcess success;
    canceled = 0;
    success.start(QStringLiteral("/bin/true"), {});
    REQUIRE(StudioWorker::wait(success, canceled));
    QProcess rejected;
    rejected.start(QStringLiteral("/bin/sleep"), {QStringLiteral("30")});
    canceled = 1;
    REQUIRE_FALSE(StudioWorker::wait(rejected, canceled));
    REQUIRE(rejected.state() == QProcess::NotRunning);
}

TEST_CASE("External jobs preserve priority cancellation export suspension and piped-process ownership", "[Studio][Optimization]")
{
    auto &manager = pCore->taskManager;
    manager.unBlock();
    manager.setExporting(false);
    const auto clean = qScopeGuard([&] { manager.setExporting(false); manager.slotCancelJobs(); });
    QObject owner;
    QStringList trace;
    QUuid automatic, manual;
    manager.setExporting(true);
    automatic = manager.queueExternal(&owner, [&] { trace << QStringLiteral("auto"); manager.finishExternal(automatic); },
        [&] { trace << QStringLiteral("canceled"); }, 0, true);
    manual = manager.queueExternal(&owner, [&] { trace << QStringLiteral("manual"); manager.finishExternal(manual); }, [] {}, 20, false);
    QApplication::processEvents();
    REQUIRE(trace == QStringList{QStringLiteral("canceled")});
    manager.setExporting(false);
    REQUIRE(studioWait([&] { return trace.size() == 2; }));
    REQUIRE(trace.last() == QStringLiteral("manual"));
    manager.setExporting(true);
    trace.clear();
    automatic = manager.queueExternal(&owner, [&] { trace << QStringLiteral("auto"); manager.finishExternal(automatic); }, [] {}, 0, false);
    manual = manager.queueExternal(&owner, [&] { trace << QStringLiteral("manual"); manager.finishExternal(manual); }, [] {}, 20, false);
    manager.setExporting(false);
    REQUIRE(studioWait([&] { return trace.size() == 2; }));
    REQUIRE((trace == QStringList{QStringLiteral("manual"), QStringLiteral("auto")}));
    QProcess sink, source, follower;
    sink.setProgram(QStringLiteral("/bin/sh")); sink.setArguments({QStringLiteral("-c"), QStringLiteral("cat >/dev/null")});
    source.setProgram(QStringLiteral("/bin/sh")); source.setArguments({QStringLiteral("-c"), QStringLiteral("sleep .15; printf data")});
    source.setStandardOutputProcess(&sink);
    QObject::connect(&sink, &QProcess::started, &source, [&] { source.start(); });
    follower.setProgram(QStringLiteral("/bin/true"));
    bool overlap = false;
    QObject::connect(&follower, &QProcess::started, &follower, [&] { overlap = source.state() != QProcess::NotRunning || sink.state() != QProcess::NotRunning; });
    StudioJobs::start(&sink, 20, &source);
    StudioJobs::start(&follower);
    REQUIRE(studioWait([&] { return !StudioJobs::busy(&sink) && !StudioJobs::busy(&source) && !StudioJobs::busy(&follower) && manager.backgroundIdle(); }));
    REQUIRE_FALSE(overlap);
    QProcess missing, untouched;
    missing.setProgram(QStringLiteral("/missing-studio-worker"));
    untouched.setProgram(QStringLiteral("/bin/sleep")); untouched.setArguments({QStringLiteral("30")});
    StudioJobs::start(&missing, 20, &untouched);
    REQUIRE(studioWait([&] { return manager.backgroundIdle() && !StudioJobs::busy(&missing); }));
    REQUIRE(untouched.state() == QProcess::NotRunning);
    manager.setExporting(true);
    QProcess canceled;
    canceled.setProgram(QStringLiteral("/bin/sleep")); canceled.setArguments({QStringLiteral("30")});
    StudioJobs::start(&canceled);
    REQUIRE(StudioJobs::busy(&canceled));
    StudioJobs::cancel(&canceled);
    manager.setExporting(false);
    REQUIRE(studioWait([&] { return !StudioJobs::busy(&canceled) && manager.backgroundIdle(); }));
    REQUIRE(canceled.processId() == 0);
    bool eligible = true, started = false;
    manager.setExporting(true);
    QProcess deferred;
    deferred.setProgram(QStringLiteral("/bin/true"));
    QObject::connect(&deferred, &QProcess::started, &deferred, [&] { started = true; });
    StudioJobs::start(&deferred, 0, nullptr, false, [&] { return eligible; });
    eligible = false; // User resumed work while this preview was waiting behind another job.
    manager.setExporting(false);
    REQUIRE(studioWait([&] { return !StudioJobs::busy(&deferred) && manager.backgroundIdle(); }));
    REQUIRE_FALSE(started);
}

TEST_CASE("Optimization defaults migrate once and preserve explicit proxy settings", "[Studio][Optimization]")
{
    auto undo = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undo);
    REQUIRE(document.getDocumentProperty(QStringLiteral("sunimo:optimizationDefaultsVersion")) == QStringLiteral("1"));
    REQUIRE(document.useProxy());
    REQUIRE(document.getDocumentProperty(QStringLiteral("proxyresize")) == QStringLiteral("640"));
    REQUIRE(UiUtils::checkUnknownProxyParams(document.getDocumentProperty(QStringLiteral("proxyparams"))).isEmpty());
    REQUIRE(UiUtils::checkUnknownProxyParams(KdenliveSettings::proxyalphaparams()).isEmpty());
    REQUIRE(KdenliveSettings::previewScaling() == 4);
    document.setDocumentProperty(QStringLiteral("sunimo:optimizationDefaultsVersion"), QStringLiteral("0"));
    document.setDocumentProperty(QStringLiteral("proxyparams"), QStringLiteral("custom-profile"));
    document.setDocumentProperty(QStringLiteral("proxyextension"), QStringLiteral("custom"));
    REQUIRE(StudioOptimization::defaults(&document));
    REQUIRE(document.getDocumentProperty(QStringLiteral("proxyparams")) == QStringLiteral("custom-profile"));
    REQUIRE(document.getDocumentProperty(QStringLiteral("proxyextension")) == QStringLiteral("custom"));
    document.setDocumentProperty(QStringLiteral("enableproxy"), QStringLiteral("0"));
    document.setDocumentProperty(QStringLiteral("proxyresize"), QStringLiteral("960"));
    KdenliveSettings::setPreviewScaling(2);
    REQUIRE_FALSE(StudioOptimization::defaults(&document));
    REQUIRE_FALSE(document.useProxy());
    REQUIRE(document.getDocumentProperty(QStringLiteral("proxyresize")) == QStringLiteral("960"));
    REQUIRE(KdenliveSettings::previewScaling() == 2);
    KdenliveSettings::setPreviewScaling(4);
    REQUIRE(undo->count() == 0);
}

TEST_CASE("Packet timing check accepts reordered CFR and rejects VFR corrupt input and cancellation", "[Studio][Optimization]")
{
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const auto generate = [&](QString name, QString rate, QStringList filters) {
        const QString path = folder.filePath(name);
        QProcess encode;
        QStringList args{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-threads"), QStringLiteral("2"),
            QStringLiteral("-filter_threads"), QStringLiteral("1"), QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("testsrc2=size=1024x576:rate=%1").arg(rate), QStringLiteral("-frames:v"), QStringLiteral("30")};
        args += filters;
        args += QStringList{QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-threads"), QStringLiteral("2"),
            QStringLiteral("-preset"), QStringLiteral("veryfast"), QStringLiteral("-bf"), QStringLiteral("3"), path};
        encode.start(QStringLiteral("ffmpeg"), args);
        REQUIRE(encode.waitForFinished(30000)); INFO(encode.readAllStandardError().toStdString()); REQUIRE(encode.exitCode() == 0);
        return path;
    };
    const QString cfr = generate(QStringLiteral("cfr.mp4"), QStringLiteral("60000/1001"), {});
    const QString vfr = generate(QStringLiteral("vfr.mp4"), QStringLiteral("60"),
        {QStringLiteral("-vf"), QStringLiteral("select=not(eq(mod(n\\,3)\\,1))"), QStringLiteral("-fps_mode"), QStringLiteral("vfr")});
    QAtomicInt canceled(0);
    qint64 frames = 0;
    REQUIRE(StudioOptimization::constantFrameRate(QStringLiteral("ffprobe"), cfr, 0, 60000. / 1001, canceled, &frames));
    REQUIRE(frames == 30);
    REQUIRE_FALSE(StudioOptimization::constantFrameRate(QStringLiteral("ffprobe"), cfr, 0, 60, canceled));
    REQUIRE_FALSE(StudioOptimization::constantFrameRate(QStringLiteral("ffprobe"), vfr, 0, 60, canceled));
    REQUIRE_FALSE(StudioOptimization::constantFrameRate(QStringLiteral("ffprobe"), folder.filePath(QStringLiteral("missing")), 0, 60, canceled));
    const QString alpha = folder.filePath(QStringLiteral("alpha.mkv"));
    QProcess alphaEncode;
    alphaEncode.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-filter_threads"), QStringLiteral("1"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("testsrc2=size=128x72:rate=60"),
        QStringLiteral("-frames:v"), QStringLiteral("60"), QStringLiteral("-c:v"), QStringLiteral("ffv1"), QStringLiteral("-pix_fmt"), QStringLiteral("bgra"), alpha});
    REQUIRE(alphaEncode.waitForFinished(30000));
    INFO(alphaEncode.readAllStandardError().toStdString());
    REQUIRE(alphaEncode.exitCode() == 0);
    REQUIRE(StudioOptimization::constantFrameRate(QStringLiteral("ffprobe"), alpha, 0, 60, canceled, &frames));
    REQUIRE(frames == 60);
    REQUIRE_FALSE(StudioOptimization::constantFrameRate(QStringLiteral("ffprobe"), alpha, 0, 60000. / 1001, canceled));
    canceled = 1;
    REQUIRE_FALSE(StudioOptimization::constantFrameRate(QStringLiteral("ffprobe"), cfr, 0, 60000. / 1001, canceled));
}

TEST_CASE("Subtitle audio snapshot keeps user filters and removes only Studio mute", "[Studio]")
{
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString path = folder.filePath(QStringLiteral("clip.mlt"));
    QFile source(path);
    REQUIRE(source.open(QIODevice::WriteOnly));
    const QByteArray xml = R"(<mlt><producer>
<filter><property name="studio:audio:role">source-mute</property><property name="mlt_service">volume</property></filter>
<filter><property name="mlt_service">volume</property><property name="level">-6</property></filter>
<filter><property name="studio:audio:role">other</property><property name="mlt_service">volume</property></filter>
</producer></mlt>)";
    REQUIRE(source.write(xml) == xml.size());
    source.close();
    REQUIRE(StudioManagedAudio::removeStudioMutes(path));
    REQUIRE(source.open(QIODevice::ReadOnly));
    QDomDocument result;
    REQUIRE(result.setContent(&source));
    const auto filters = result.elementsByTagName(QStringLiteral("filter"));
    REQUIRE(filters.count() == 2);
    REQUIRE(result.toString().contains(QStringLiteral("-6")));
    REQUIRE(result.toString().contains(QStringLiteral("other")));
    REQUIRE_FALSE(result.toString().contains(QStringLiteral("source-mute")));
}

TEST_CASE("Analysis snapshots use originals without changing streams timing filters or the live producer", "[Studio][Optimization]")
{
    auto bin = pCore->projectItemModel();
    bin->clean();
    auto undo = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undo);
    pCore->projectManager()->testSetDocument(&document);
    const auto clean = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    pCore->projectManager()->testSetActiveTimeline(document.getTimeline(document.uuid()));
    const QString id = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", bin, 120, false);
    auto clip = bin->getClipByBinID(id);
    REQUIRE(clip);
    clip->setProducerProperty(QStringLiteral("kdenlive:proxy"), QStringLiteral("proxy.mov"));
    clip->setProducerProperty(QStringLiteral("kdenlive:originalurl"), QStringLiteral("original.mp4"));
    QTemporaryDir folder;
    const QString path = folder.filePath(QStringLiteral("audio.mlt"));
    QFile input(path);
    REQUIRE(input.open(QIODevice::WriteOnly));
    const QByteArray xml = R"(<mlt root="/sources"><chain in="12" out="71">
<property name="mlt_service">avformat</property><property name="resource">proxy.mov</property>
<property name="audio_index">2</property><property name="video_index">1</property>
<filter><property name="studio:audio:role">source-mute</property></filter>
<filter><property name="mlt_service">volume</property><property name="level">-6</property></filter>
</chain><producer in="5" out="25"><property name="mlt_service">timewarp</property>
<property name="resource">0.5:proxy.mov</property><property name="warp_resource">proxy.mov</property>
<property name="warp_speed">0.5</property><property name="audio_index">2</property></producer></mlt>)";
    REQUIRE(input.write(xml) == xml.size());
    input.close();
    REQUIRE(StudioManagedAudio::removeStudioMutes(path));
    REQUIRE(input.open(QIODevice::ReadOnly));
    QDomDocument result;
    REQUIRE(result.setContent(&input));
    const auto chain = result.elementsByTagName(QStringLiteral("chain")).at(0).toElement();
    const auto speed = result.elementsByTagName(QStringLiteral("producer")).at(0).toElement();
    REQUIRE(Xml::getXmlProperty(chain, QStringLiteral("resource")) == QStringLiteral("/sources/original.mp4"));
    REQUIRE(Xml::getXmlProperty(speed, QStringLiteral("resource")) == QStringLiteral("0.5:/sources/original.mp4"));
    REQUIRE(Xml::getXmlProperty(speed, QStringLiteral("warp_resource")) == QStringLiteral("/sources/original.mp4"));
    REQUIRE(Xml::getXmlProperty(speed, QStringLiteral("warp_speed")) == QStringLiteral("0.5"));
    REQUIRE(Xml::getXmlProperty(chain, QStringLiteral("audio_index")) == QStringLiteral("2"));
    REQUIRE(Xml::getXmlProperty(chain, QStringLiteral("video_index")) == QStringLiteral("1"));
    REQUIRE(chain.attribute(QStringLiteral("in")) == QStringLiteral("12"));
    REQUIRE(chain.attribute(QStringLiteral("out")) == QStringLiteral("71"));
    REQUIRE(result.toString().contains(QStringLiteral("-6")));
    REQUIRE_FALSE(result.toString().contains(QStringLiteral("source-mute")));
    REQUIRE(clip->getProducerProperty(QStringLiteral("kdenlive:proxy")) == QStringLiteral("proxy.mov"));

    // Decode distinguishable original/proxy audio, including the selected stream and trim.
    const QString original = folder.filePath(QStringLiteral("оригинал.mka"));
    const QString cached = folder.filePath(QStringLiteral("proxy.mka"));
    for (const auto &file : {original, cached}) {
        QProcess generate;
        generate.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=400:duration=2:sample_rate=48000"),
            QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("sine=frequency=%1:duration=2:sample_rate=48000").arg(file == original ? 900 : 1800),
            QStringLiteral("-map"), QStringLiteral("0:a"), QStringLiteral("-map"), QStringLiteral("1:a"),
            QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"), file});
        REQUIRE(generate.waitForFinished(30000));
        INFO(generate.readAllStandardError().toStdString()); REQUIRE(generate.exitCode() == 0);
    }
    clip->setProducerProperty(QStringLiteral("kdenlive:proxy"), cached);
    clip->setProducerProperty(QStringLiteral("kdenlive:originalurl"), original);
    QList<QByteArray> audio;
    for (int pass = 0; pass < 3; ++pass) {
        QDomDocument snapshot;
        REQUIRE(snapshot.setContent(QByteArray(R"(<mlt producer="timeline"><profile width="1920" height="1080" frame_rate_num="60" frame_rate_den="1" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="16" display_aspect_den="9" colorspace="709"/><chain id="clip"><property name="mlt_service">avformat</property><property name="audio_index">1</property><property name="video_index">-1</property></chain><playlist id="timeline"><entry producer="clip" in="12" out="71"/></playlist></mlt>)")));
        Xml::setXmlProperty(snapshot.elementsByTagName(QStringLiteral("chain")).at(0).toElement(), QStringLiteral("resource"), pass == 0 ? original : cached);
        const QString scene = folder.filePath(QStringLiteral("analysis-%1.mlt").arg(pass));
        REQUIRE(Xml::docContentToFile(snapshot, scene));
        if (pass == 2) REQUIRE(StudioManagedAudio::removeStudioMutes(scene));
        const QString pcm = folder.filePath(QStringLiteral("audio-%1.pcm").arg(pass));
        QProcess render;
        render.start(QStringLiteral("melt"), {scene, QStringLiteral("-consumer"), QStringLiteral("avformat:%1").arg(pcm),
            QStringLiteral("f=s16le"), QStringLiteral("acodec=pcm_s16le"), QStringLiteral("vn=1"),
            QStringLiteral("ar=48000"), QStringLiteral("ac=1"), QStringLiteral("real_time=-1"), QStringLiteral("threads=1")});
        REQUIRE(render.waitForFinished(30000));
        INFO(render.readAllStandardError().toStdString()); REQUIRE(render.exitCode() == 0);
        QFile decoded(pcm); REQUIRE(decoded.open(QIODevice::ReadOnly)); audio << decoded.readAll();
    }
    REQUIRE(audio[0].size() == 48000 * 2);
    REQUIRE(audio[1] != audio[0]);
    REQUIRE(audio[2] == audio[0]);
    REQUIRE(clip->getProducerProperty(QStringLiteral("kdenlive:proxy")) == cached);
}

TEST_CASE("Native subtitles import edit delete undo and survive reopen", "[Studio]")
{
    pCore->projectItemModel()->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const int previousLocation = KdenliveSettings::videotodefaultfolder();
    auto restoreSettings = qScopeGuard([previousLocation] { KdenliveSettings::setVideotodefaultfolder(previousLocation); });
    KdenliveSettings::setVideotodefaultfolder(KdenliveDoc::SaveToProjectFolder);
    document.m_sameProjectFolder = true;
    document.setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    document.setProjectFolder(QUrl::fromLocalFile(folder.path()));
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", pCore->projectItemModel(), 120, false);
    int clipId = -1;
    REQUIRE(timeline->requestClipInsertion(binId, timeline->getTrackIndexFromPosition(2), 0, clipId));
    auto subtitles = timeline->createSubtitleModel();
    REQUIRE(subtitles);
    const QString styleName = QStringLiteral("Studio-regression");
    const double defaultSize = subtitles->getSubtitleStyle(QStringLiteral("Default")).fontSize();
    const QString assPath = folder.filePath(QStringLiteral("two-lines.ass"));
    QFile ass(assPath);
    REQUIRE(ass.open(QIODevice::WriteOnly));
    const QByteArray contents = R"ASS([Script Info]
ScriptType: v4.00+
PlayResX: 1920
PlayResY: 1080

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Studio-regression,DejaVu Sans,64,&H00FFFFFF,&H0000FFFF,&H00000000,&H66000000,1,0,0,0,100,100,0,0,3,3,0,2,80,80,88,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:02.00,Studio-regression,,0,0,0,,Первая строка
Dialogue: 0,0:00:03.00,0:00:04.00,Studio-regression,,0,0,0,keep-effect,{\alpha&HFF&\t(0,1,\alpha&H00&)}Вторая {\k20}строка
)ASS";
    REQUIRE(ass.write(contents) == contents.size());
    ass.close();

    const int beforeImport = undoStack->index();
    subtitles->importSubtitle(assPath, 0, true);
    REQUIRE(undoStack->index() == beforeImport + 1);
    REQUIRE(subtitles->rowCount() == 2);
    REQUIRE(subtitles->getAllSubtitleStyles().count(styleName) == 1);
    REQUIRE(subtitles->getSubtitleStyle(QStringLiteral("Default")).fontSize() == Approx(defaultSize));
    const double fps = pCore->getCurrentFps();
    const int firstId = subtitles->getIdForStartPos(0, GenTime(qRound(fps), fps));
    const int secondId = subtitles->getIdForStartPos(0, GenTime(qRound(3 * fps), fps));
    REQUIRE(firstId >= 0);
    REQUIRE(secondId >= 0);
    REQUIRE(subtitles->getStyleName(firstId) == styleName);
    REQUIRE(subtitles->getStyleName(secondId) == styleName);
    REQUIRE(subtitles->getEffects(secondId) == QStringLiteral("keep-effect"));
    REQUIRE(subtitles->getText(secondId) == QStringLiteral("{\\alpha&HFF&\\t(0,1,\\alpha&H00&)}Вторая {\\k20}строка"));
    subtitles->importSubtitle(assPath, 0, true);
    REQUIRE(subtitles->rowCount() == 2);
    REQUIRE(undoStack->index() == beforeImport + 1);
    undoStack->undo();
    REQUIRE(subtitles->rowCount() == 0);
    REQUIRE(subtitles->getAllSubtitleStyles().count(styleName) == 0);
    undoStack->redo();
    REQUIRE(subtitles->rowCount() == 2);
    REQUIRE(subtitles->getAllSubtitleStyles().count(styleName) == 1);

    const QString original = subtitles->getText(firstId);
    REQUIRE(original == QStringLiteral("Первая строка"));
    subtitles->editSubtitle(firstId, QStringLiteral("Исправленная строка"), original);
    REQUIRE(subtitles->getText(firstId) == QStringLiteral("Исправленная строка"));
    undoStack->undo();
    REQUIRE(subtitles->getText(firstId) == original);
    undoStack->redo();
    REQUIRE(subtitles->getText(firstId) == QStringLiteral("Исправленная строка"));

    const auto secondRange = subtitles->getInOut(secondId);
    subtitles->deleteSubtitle(0, secondRange.first, secondRange.second, subtitles->getText(secondId));
    REQUIRE(subtitles->rowCount() == 1);
    undoStack->undo();
    REQUIRE(subtitles->rowCount() == 2);
    REQUIRE(subtitles->getStyleName(secondId) == styleName);
    REQUIRE(subtitles->getEffects(secondId) == QStringLiteral("keep-effect"));
    undoStack->redo();
    REQUIRE(subtitles->rowCount() == 1);
    undoStack->undo();
    REQUIRE(subtitles->rowCount() == 2);

    auto changedStyle = subtitles->getSubtitleStyle(styleName);
    changedStyle.setFontSize(72);
    subtitles->setSubtitleStyle(styleName, changedStyle);
    REQUIRE(subtitles->getSubtitleStyle(styleName).fontSize() == Approx(72));
    undoStack->undo();
    REQUIRE(subtitles->getSubtitleStyle(styleName).fontSize() == Approx(64));
    undoStack->redo();
    REQUIRE(subtitles->getSubtitleStyle(styleName).fontSize() == Approx(72));

    const QString pairedPath = folder.filePath(QStringLiteral("paired.ass"));
    QFile paired(pairedPath);
    REQUIRE(paired.open(QIODevice::WriteOnly));
    const QByteArray pairedContents = R"ASS([Script Info]
ScriptType: v4.00+
PlayResX: 1920
PlayResY: 1080

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Studio-paired,DejaVu Sans,64,&H000000FF,&H0000FFFF,&H0000FF00,&H66FF0000,1,0,0,0,100,100,0,0,1,4,0,2,80,80,88,1
Style: Studio-paired-Box,DejaVu Sans,64,&HFFFFFFFF,&HFFFFFFFF,&H66FF0000,&H66FF0000,1,0,0,0,100,100,0,0,3,4,0,2,80,80,88,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:02.20,0:00:02.80,Studio-paired-Box,,0,0,0,,Ёжик
Dialogue: 1,0:00:02.20,0:00:02.80,Studio-paired,,0,0,0,,Ёжик
)ASS";
    REQUIRE(paired.write(pairedContents) == pairedContents.size());
    paired.close();
    subtitles->importSubtitle(pairedPath, 0, true);
    REQUIRE(subtitles->rowCount() == 4);
    REQUIRE(subtitles->getAllSubtitleStyles().count(QStringLiteral("Studio-paired")) == 1);
    REQUIRE(subtitles->getAllSubtitleStyles().count(QStringLiteral("Studio-paired-Box")) == 1);

    const QString saved = folder.filePath(QStringLiteral("Субтитры проект.kdenlive"));
    // testSaveFileAs writes only XML; mirror its subtitle asset copy without the GUI-only time-warp step.
    subtitles->copySubtitle(saved + QStringLiteral(".ass"), 0, false, true);
    QFile savedAss(saved + QStringLiteral(".ass"));
    REQUIRE(savedAss.open(QIODevice::ReadOnly));
    const QByteArray savedAssContents = savedAss.readAll();
    REQUIRE(savedAssContents.contains(QStringLiteral("Исправленная строка").toUtf8()));
    REQUIRE(savedAssContents.contains("Style: Studio-regression"));
    REQUIRE(savedAssContents.contains("Style: Studio-paired-Box"));
    REQUIRE(savedAssContents.contains("{\\alpha&HFF&\\t(0,1,\\alpha&H00&)}Вторая {\\k20}строка"));
    REQUIRE(pCore->projectManager()->testSaveFileAs(saved));
    REQUIRE(QDir(folder.filePath(document.getDocumentProperty(QStringLiteral("documentid")))).exists());
    document.setUrl(QUrl::fromLocalFile(saved));
    pCore->projectManager()->closeCurrentDocument(false, false);
    QUndoGroup reopenedUndo;
    auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(saved), folder.path(), &reopenedUndo, false, nullptr);
    REQUIRE(opened.isSuccessful());
    auto reopened = opened.getDocument();
    pCore->projectManager()->testSetDocument(reopened.get());
    const auto closeReopened = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(saved).lastModified(), 0));
    const auto sequence = pCore->projectItemModel()->getAllSequenceClips().value(reopened->uuid());
    pCore->projectManager()->openTimeline(sequence, -1, reopened->uuid());
    auto reopenedTimeline = reopened->getTimeline(reopened->uuid());
    pCore->projectManager()->testSetActiveTimeline(reopenedTimeline);
    REQUIRE(reopenedTimeline->getClipsCount() == 1);
    auto reopenedSubtitles = reopenedTimeline->getSubtitleModel();
    REQUIRE(reopenedSubtitles);
    REQUIRE(reopenedSubtitles->rowCount() == 4);
    REQUIRE(reopenedSubtitles->getAllSubtitleStyles().count(styleName) == 1);
    REQUIRE(reopenedSubtitles->getSubtitleStyle(styleName).fontSize() == Approx(72));
    const int reopenedFirst = reopenedSubtitles->getIdForStartPos(0, GenTime(qRound(fps), fps));
    const int reopenedSecond = reopenedSubtitles->getIdForStartPos(0, GenTime(qRound(3 * fps), fps));
    REQUIRE(reopenedFirst >= 0);
    REQUIRE(reopenedSecond >= 0);
    REQUIRE(reopenedSubtitles->getText(reopenedFirst) == QStringLiteral("Исправленная строка"));
    REQUIRE(reopenedSubtitles->getText(reopenedSecond) == QStringLiteral("{\\alpha&HFF&\\t(0,1,\\alpha&H00&)}Вторая {\\k20}строка"));
    REQUIRE(reopenedSubtitles->getStyleName(reopenedSecond) == styleName);
    REQUIRE(reopenedSubtitles->getEffects(reopenedSecond) == QStringLiteral("keep-effect"));
    const GenTime pairedStart(qRound(2.2 * fps), fps);
    const int reopenedBox = reopenedSubtitles->getIdForStartPos(0, pairedStart);
    const int reopenedOutline = reopenedSubtitles->getIdForStartPos(1, pairedStart);
    REQUIRE(reopenedBox >= 0);
    REQUIRE(reopenedOutline >= 0);
    REQUIRE(reopenedSubtitles->getStyleName(reopenedBox) == QStringLiteral("Studio-paired-Box"));
    REQUIRE(reopenedSubtitles->getStyleName(reopenedOutline) == QStringLiteral("Studio-paired"));
}

TEST_CASE("Studio restyles selected subtitles without recognition and keeps undo and saved cues", "[Studio]")
{
    const QString previousProfile = pCore->getCurrentProfilePath();
    const auto restoreProfile = qScopeGuard([previousProfile] { pCore->setCurrentProfile(previousProfile); });
    REQUIRE(pCore->setCurrentProfile(QStringLiteral("atsc_1080p_60")));
    const QByteArray previousPrefix = qgetenv("STUDIO_PREFIX");
    const QByteArray qaPrefix = qgetenv("STUDIO_QA_PREFIX");
    if (!qaPrefix.isEmpty()) qputenv("STUDIO_PREFIX", qaPrefix);
    const auto restorePrefix = qScopeGuard([previousPrefix] {
        if (previousPrefix.isNull()) qunsetenv("STUDIO_PREFIX"); else qputenv("STUDIO_PREFIX", previousPrefix);
    });
    pCore->projectItemModel()->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const int previousLocation = KdenliveSettings::videotodefaultfolder();
    auto restoreSettings = qScopeGuard([previousLocation] { KdenliveSettings::setVideotodefaultfolder(previousLocation); });
    KdenliveSettings::setVideotodefaultfolder(KdenliveDoc::SaveToProjectFolder);
    document.m_sameProjectFolder = true;
    document.setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    document.setProjectFolder(QUrl::fromLocalFile(folder.path()));
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", pCore->projectItemModel(), 120, false);
    int clipId = -1;
    REQUIRE(timeline->requestClipInsertion(binId, timeline->getTrackIndexFromPosition(2), 0, clipId));
    auto subtitles = timeline->createSubtitleModel();
    REQUIRE(subtitles);
    const QString firstMarker = QStringLiteral("SUNIMO1:42:1000:2000:%1:1000-1080;1600-1900")
                                    .arg(QString::fromLatin1(QStringLiteral("Я слово").toUtf8().toHex()));
    const QString staleMarker = QStringLiteral("SUNIMO1:43:3000:4000:%1:3000-3300;3600-3900")
                                    .arg(QString::fromLatin1(QStringLiteral("Исходное слово").toUtf8().toHex()));
    const QString script = QStringLiteral(R"ASS([Script Info]
ScriptType: v4.00+
PlayResX: 1920
PlayResY: 1080

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Studio-before,DejaVu Sans,64,&H00FFFFFF,&H0000FFFF,&H00000000,&H66000000,1,0,0,0,100,100,0,0,1,3,0,2,80,80,88,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:02.00,Studio-before,,0,0,0,%1,Я слово
Dialogue: 0,0:00:03.00,0:00:04.00,Studio-before,,0,0,0,%2,Исправлено вручную
Dialogue: 0,0:00:05.00,0:00:06.00,Studio-before,,0,0,0,,Третий блок
)ASS").arg(firstMarker, staleMarker);
    const QString source = folder.filePath(QStringLiteral("original.ass"));
    QFile file(source);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(script.toUtf8()) == script.toUtf8().size());
    file.close();
    subtitles->importSubtitle(source, 0, true);
    REQUIRE(subtitles->rowCount() == 3);
    const double fps = pCore->getCurrentFps();
    const auto idAt = [&](int seconds) { return subtitles->getIdForStartPos(0, GenTime(qRound(seconds * fps), fps)); };
    const int first = idAt(1), second = idAt(3), third = idAt(5);
    REQUIRE(first >= 0); REQUIRE(second >= 0); REQUIRE(third >= 0);
    const auto activeSequence = pCore->projectItemModel()->getAllSequenceClips().value(document.uuid());
    REQUIRE_FALSE(activeSequence.isEmpty());
    pCore->projectManager()->openTimeline(activeSequence, -1, document.uuid());
    timeline->requestSetSelection({first, second});
    StudioSubtitlePage page;
    auto restyle = page.findChild<QPushButton *>(QStringLiteral("studioSubtitleRestyle"));
    auto wholeTrack = page.findChild<QCheckBox *>(QStringLiteral("studioSubtitleRestyleAll"));
    auto background = page.findChild<QComboBox *>(QStringLiteral("studioSubtitle_background"));
    auto animation = page.findChild<QComboBox *>(QStringLiteral("studioSubtitle_animation"));
    auto status = page.findChild<QLabel *>(QStringLiteral("studioSubtitleStatus"));
    REQUIRE(restyle); REQUIRE(wholeTrack); REQUIRE(background); REQUIRE(animation); REQUIRE(status);
    REQUIRE(animation->count() > 7);
    background->setCurrentIndex(0);
    animation->setCurrentIndex(7);
    page.activate();
    REQUIRE_FALSE(wholeTrack->isChecked());
    INFO("selection=" << timeline->getCurrentSelection().size() << " wholeEnabled=" << wholeTrack->isEnabled()
         << " locked=" << subtitles->isLocked() << " target=" << page.findChildren<QLabel *>().first()->text().toStdString());
    REQUIRE(restyle->isEnabled());
    const int undoBefore = undoStack->index();
    restyle->click();
    REQUIRE(studioWait([&] { return status->text().contains(QStringLiteral("Переоформлено блоков: 2")); }, 30000));
    REQUIRE(status->text().contains(QStringLiteral("метки слов устарели")));
    REQUIRE(subtitles->rowCount() == 3);
    REQUIRE(undoStack->index() == undoBefore + 1);
    REQUIRE(subtitles->getText(third) == QStringLiteral("Третий блок"));
    REQUIRE(subtitles->getStyleName(third) == QStringLiteral("Studio-before"));
    const int styledFirst = idAt(1), styledSecond = idAt(3);
    REQUIRE(styledFirst >= 0); REQUIRE(styledSecond >= 0);
    REQUIRE(subtitles->getText(styledFirst).contains(QStringLiteral("Я")));
    REQUIRE(subtitles->getText(styledFirst).contains(QStringLiteral("\\t(600,601,")));
    REQUIRE(subtitles->getText(styledSecond).contains(QStringLiteral("Исправлено вручную")));
    REQUIRE(subtitles->getText(styledSecond).contains(QStringLiteral("\\fad(160,0)")));
    REQUIRE(subtitles->getEffects(styledSecond).endsWith(QLatin1Char(':')));
    undoStack->undo();
    REQUIRE(subtitles->rowCount() == 3);
    REQUIRE(subtitles->getText(second) == QStringLiteral("Исправлено вручную"));
    REQUIRE(subtitles->getEffects(first) == firstMarker);
    REQUIRE(subtitles->getEffects(second) == staleMarker);
    REQUIRE(subtitles->getStyleName(third) == QStringLiteral("Studio-before"));
    undoStack->redo();
    timeline->requestSetSelection({idAt(1), idAt(3)});
    page.refreshSelection();
    restyle->click();
    REQUIRE(studioWait([&] { return status->text().contains(QStringLiteral("Переоформлено блоков: 2")) && restyle->isEnabled(); }, 30000));
    REQUIRE(subtitles->rowCount() == 3);
    const auto requireIsolatedSharedStyle = [&](int selectedId, int untouchedId) {
        const QString sharedName = subtitles->getStyleName(selectedId), oldName = subtitles->getStyleName(untouchedId);
        const int selectedLayer = subtitles->getLayerForId(selectedId), selectedStart = subtitles->getInOut(selectedId).first;
        const bool assigned = sharedName != oldName;
        if (assigned) subtitles->setStyleName(untouchedId, sharedName);
        auto nativeStyle = subtitles->getSubtitleStyle(sharedName);
        const double requestedOutline = nativeStyle.outline();
        nativeStyle.setOutline(17);
        subtitles->setSubtitleStyle(sharedName, nativeStyle);
        const QString styleBefore = nativeStyle.toString(sharedName), textBefore = subtitles->getText(untouchedId);
        const QString effectsBefore = subtitles->getEffects(untouchedId);
        const auto rangeBefore = subtitles->getInOut(untouchedId);
        REQUIRE(timeline->requestSetSelection({selectedId}));
        page.refreshSelection();
        REQUIRE(restyle->isEnabled());
        const int before = undoStack->index(), rows = subtitles->rowCount();
        restyle->click();
        REQUIRE(studioWait([&] { return !status->text().contains(QStringLiteral("Меняю оформление")) && restyle->isEnabled(); }, 30000));
        INFO(status->text().toStdString());
        REQUIRE(status->text().contains(QStringLiteral("Переоформлено блоков: 1")));
        REQUIRE(undoStack->index() == before + 1);
        REQUIRE(subtitles->rowCount() == rows);
        REQUIRE(subtitles->getText(untouchedId) == textBefore);
        REQUIRE(subtitles->getEffects(untouchedId) == effectsBefore);
        REQUIRE(subtitles->getInOut(untouchedId) == rangeBefore);
        REQUIRE(subtitles->getStyleName(untouchedId) == sharedName);
        REQUIRE(subtitles->getSubtitleStyle(sharedName).toString(sharedName) == styleBefore);
        const int restyledId = subtitles->getIdForStartPos(selectedLayer, GenTime(selectedStart, fps));
        REQUIRE(restyledId >= 0);
        REQUIRE(subtitles->getSubtitleStyle(subtitles->getStyleName(restyledId)).outline() == Approx(requestedOutline));
        undoStack->undo();
        REQUIRE(subtitles->getSubtitleStyle(sharedName).toString(sharedName) == styleBefore);
        undoStack->undo();
        if (assigned) undoStack->undo();
        REQUIRE(subtitles->getStyleName(untouchedId) == oldName);
        REQUIRE(subtitles->rowCount() == rows);
    };
    requireIsolatedSharedStyle(idAt(1), third);
    background->setCurrentIndex(1);
    timeline->requestSetSelection({idAt(1), idAt(3)});
    page.refreshSelection();
    restyle->click();
    REQUIRE(studioWait([&] { return subtitles->rowCount() == 5 && restyle->isEnabled(); }, 30000));
    const int boxedFirst = subtitles->getIdForStartPos(1, GenTime(qRound(fps), fps));
    const int boxedSecond = subtitles->getIdForStartPos(1, GenTime(qRound(3 * fps), fps));
    REQUIRE(boxedFirst >= 0); REQUIRE(boxedSecond >= 0);
    const int firstBox = idAt(1);
    REQUIRE(firstBox >= 0);
    const QString mainBeforeEdit = subtitles->getText(boxedFirst), boxBeforeEdit = subtitles->getText(firstBox);
    const QString manualText = QStringLiteral("Я исправлено вручную");
    for (int editedId : {boxedFirst, firstBox}) {
        INFO("native edited subtitle layer=" << subtitles->getLayerForId(editedId));
        subtitles->setText(editedId, manualText);
        const QString mainAfterEdit = subtitles->getText(boxedFirst), boxAfterEdit = subtitles->getText(firstBox);
        REQUIRE(timeline->requestSetSelection({editedId}));
        background->setCurrentIndex(1);
        animation->setCurrentIndex(7);
        page.refreshSelection();
        REQUIRE(restyle->isEnabled());
        const int beforeManualRestyle = undoStack->index();
        restyle->click();
        REQUIRE(studioWait([&] { return !status->text().contains(QStringLiteral("Меняю оформление")) && restyle->isEnabled(); }, 30000));
        INFO(status->text().toStdString());
        REQUIRE(status->text().contains(QStringLiteral("Переоформлено блоков: 1")));
        REQUIRE(status->text().contains(QStringLiteral("метки слов устарели")));
        REQUIRE(undoStack->index() == beforeManualRestyle + 1);
        REQUIRE(subtitles->rowCount() == 5);
        const int newMain = subtitles->getIdForStartPos(1, GenTime(qRound(fps), fps));
        REQUIRE(newMain >= 0);
        REQUIRE(idAt(1) >= 0);
        REQUIRE(subtitles->getText(newMain).contains(manualText));
        REQUIRE(subtitles->getText(idAt(1)).contains(manualText));
        REQUIRE(subtitles->getEffects(newMain).endsWith(QLatin1Char(':')));
        REQUIRE(subtitles->getText(boxedSecond).contains(QStringLiteral("Исправлено вручную")));
        undoStack->undo();
        REQUIRE(subtitles->getText(boxedFirst) == mainAfterEdit);
        REQUIRE(subtitles->getText(firstBox) == boxAfterEdit);
        undoStack->redo();
        REQUIRE(subtitles->getText(subtitles->getIdForStartPos(1, GenTime(qRound(fps), fps))).contains(manualText));
        REQUIRE(subtitles->getText(idAt(1)).contains(manualText));
        undoStack->undo();
        undoStack->undo();
        REQUIRE(subtitles->getText(boxedFirst) == mainBeforeEdit);
        REQUIRE(subtitles->getText(firstBox) == boxBeforeEdit);
        REQUIRE(subtitles->rowCount() == 5);
    }
    subtitles->setText(boxedFirst, QStringLiteral("Правка основного слоя"));
    subtitles->setText(firstBox, QStringLiteral("Другая правка плашки"));
    REQUIRE(timeline->requestSetSelection({boxedFirst}));
    page.refreshSelection();
    REQUIRE(restyle->isEnabled());
    const int beforeConflictingRestyle = undoStack->index();
    restyle->click();
    REQUIRE(status->text().contains(QStringLiteral("Текст двух слоёв")));
    REQUIRE(undoStack->index() == beforeConflictingRestyle);
    REQUIRE(subtitles->rowCount() == 5);
    REQUIRE(subtitles->getText(boxedFirst) == QStringLiteral("Правка основного слоя"));
    REQUIRE(subtitles->getText(firstBox) == QStringLiteral("Другая правка плашки"));
    undoStack->undo();
    undoStack->undo();
    REQUIRE(subtitles->getText(boxedFirst) == mainBeforeEdit);
    REQUIRE(subtitles->getText(firstBox) == boxBeforeEdit);
    requireIsolatedSharedStyle(firstBox, idAt(3));
    background->setCurrentIndex(0);
    timeline->requestSetSelection({boxedFirst, boxedSecond});
    page.refreshSelection();
    restyle->click();
    REQUIRE(studioWait([&] { return subtitles->rowCount() == 3 && restyle->isEnabled(); }, 30000));
    undoStack->undo();
    REQUIRE(subtitles->rowCount() == 5);
    undoStack->redo();
    REQUIRE(subtitles->rowCount() == 3);
    timeline->requestSetSelection({idAt(1)});
    wholeTrack->setChecked(true);
    animation->setCurrentIndex(4);
    restyle->click();
    REQUIRE(studioWait([&] { return status->text().contains(QStringLiteral("Переоформлено блоков: 3")) && restyle->isEnabled(); }, 30000));
    REQUIRE(subtitles->rowCount() == 3);
    REQUIRE(subtitles->getStyleName(idAt(5)) != QStringLiteral("Studio-before"));
    undoStack->undo();
    REQUIRE(subtitles->getStyleName(third) == QStringLiteral("Studio-before"));
    wholeTrack->setChecked(false);
    const QString finalStyle = subtitles->getStyleName(idAt(3));

    const QString saved = folder.filePath(QStringLiteral("styled.kdenlive"));
    subtitles->copySubtitle(saved + QStringLiteral(".ass"), 0, false, true);
    const auto frameHash = [&](const QString &path, int frameNumber) {
        Mlt::Producer producer(pCore->getProjectProfile(), "color", "#182638");
        Mlt::Filter filter(pCore->getProjectProfile(), "avfilter.subtitles");
        REQUIRE(producer.is_valid()); REQUIRE(filter.is_valid());
        filter.set("av.filename", QFile::encodeName(path).constData());
        producer.attach(filter);
        producer.seek(frameNumber);
        std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
        REQUIRE(frame);
        auto format = mlt_image_rgba;
        int width = 320, height = 180;
        const auto pixels = frame->get_image(format, width, height);
        REQUIRE(pixels);
        return QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(pixels), width * height * 4), QCryptographicHash::Sha256);
    };
    const QByteArray baseline = frameHash(saved + QStringLiteral(".ass"), qRound(.5 * fps));
    const QByteArray firstWord = frameHash(saved + QStringLiteral(".ass"), qRound(1.1 * fps));
    const QByteArray pause = frameHash(saved + QStringLiteral(".ass"), qRound(1.6 * fps) - 1);
    const QByteArray secondWord = frameHash(saved + QStringLiteral(".ass"), qRound(1.6 * fps) + 1);
    REQUIRE(firstWord != baseline);
    REQUIRE(pause == firstWord);
    REQUIRE(secondWord != pause);
    REQUIRE(frameHash(saved + QStringLiteral(".ass"), qRound(1.9 * fps)) == secondWord);
    REQUIRE(frameHash(saved + QStringLiteral(".ass"), qRound(2.1 * fps)) == baseline);
    REQUIRE(pCore->projectManager()->testSaveFileAs(saved));
    document.setUrl(QUrl::fromLocalFile(saved));
    page.deactivate();
    pCore->projectManager()->closeCurrentDocument(false, false);
    QUndoGroup reopenedUndo;
    auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(saved), folder.path(), &reopenedUndo, false, nullptr);
    REQUIRE(opened.isSuccessful());
    auto reopened = opened.getDocument();
    pCore->projectManager()->testSetDocument(reopened.get());
    const auto closeReopened = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(saved).lastModified(), 0));
    const auto sequence = pCore->projectItemModel()->getAllSequenceClips().value(reopened->uuid());
    pCore->projectManager()->openTimeline(sequence, -1, reopened->uuid());
    auto reopenedTimeline = reopened->getTimeline(reopened->uuid());
    pCore->projectManager()->testSetActiveTimeline(reopenedTimeline);
    auto reopenedSubtitles = reopenedTimeline->getSubtitleModel();
    REQUIRE(reopenedSubtitles);
    REQUIRE(reopenedSubtitles->rowCount() == 3);
    const int reopenedFirst = reopenedSubtitles->getIdForStartPos(0, GenTime(qRound(fps), fps));
    const int reopenedSecond = reopenedSubtitles->getIdForStartPos(0, GenTime(qRound(3 * fps), fps));
    REQUIRE(reopenedFirst >= 0); REQUIRE(reopenedSecond >= 0);
    REQUIRE(reopenedSubtitles->getText(reopenedSecond).contains(QStringLiteral("Исправлено вручную")));
    REQUIRE(reopenedSubtitles->getEffects(reopenedFirst).contains(QStringLiteral(":1000-1080;1600-1900")));
    REQUIRE(reopenedSubtitles->getEffects(reopenedSecond).endsWith(QLatin1Char(':')));
    REQUIRE(reopenedSubtitles->getStyleName(reopenedSecond) == finalStyle);
    const QString reopenedAss = folder.filePath(QStringLiteral("reopened.ass"));
    reopenedSubtitles->copySubtitle(reopenedAss, 0, false, true);
    REQUIRE(frameHash(reopenedAss, qRound(1.6 * fps) + 1) == secondWord);
}

static QList<QByteArray> frameHashes(const std::shared_ptr<TimelineItemModel> &timeline)
{
    QList<QByteArray> result;
    auto producer = timeline->producer();
    for (int position : {0, 7, 14, 15, 23, 29}) {
        producer->seek(position);
        std::unique_ptr<Mlt::Frame> frame(producer->get_frame());
        REQUIRE(frame);
        auto format = mlt_image_rgba;
        int width = 320, height = 180;
        const auto pixels = frame->get_image(format, width, height);
        REQUIRE(pixels);
        result << QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(pixels), width * height * 4), QCryptographicHash::Sha256);
    }
    return result;
}

static QByteArray studioFrameAt(const std::shared_ptr<TimelineItemModel> &timeline, int position, int width = 320, int height = 180)
{
    auto producer = timeline->producer();
    producer->seek(position);
    std::unique_ptr<Mlt::Frame> frame(producer->get_frame());
    REQUIRE(frame);
    auto format = mlt_image_rgba;
    // MLT treats these as the requested render size; zero creates a 0x0 frame.
    const auto pixels = frame->get_image(format, width, height);
    REQUIRE(pixels);
    REQUIRE(width > 0);
    REQUIRE(height > 0);
    return QByteArray(reinterpret_cast<const char *>(pixels), width * height * 4);
}

static QByteArray studioFrameHash(const std::shared_ptr<TimelineItemModel> &timeline, int position)
{
    return QCryptographicHash::hash(studioFrameAt(timeline, position), QCryptographicHash::Sha256);
}

TEST_CASE("Studio background export resolves the clip referenced by a timeline entry", "[Studio]")
{
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    QDomDocument project;
    REQUIRE(project.setContent(QStringLiteral(R"(<mlt>
        <chain id="other"><property name="resource">other.mp4</property></chain>
        <chain id="chain3"><property name="resource">media/клип 1.mp4</property></chain>
        <playlist><property name="resource">wrong-parent.mp4</property>
          <entry producer="chain3"><filter id="matte"/></entry>
        </playlist>
    </mlt>)")));
    auto source = project.documentElement().firstChildElement(QStringLiteral("chain")).nextSiblingElement(QStringLiteral("chain"));
    auto entry = project.documentElement().firstChildElement(QStringLiteral("playlist")).firstChildElement(QStringLiteral("entry"));
    const auto filter = entry.firstChildElement(QStringLiteral("filter"));
    REQUIRE(StudioBackground::sourcePath(filter, folder.path()) == folder.filePath(QStringLiteral("media/клип 1.mp4")));

    auto original = project.createElement(QStringLiteral("property"));
    original.setAttribute(QStringLiteral("name"), QStringLiteral("kdenlive:originalurl"));
    original.appendChild(project.createTextNode(QUrl::fromLocalFile(folder.filePath(QStringLiteral("оригинал.mp4"))).toString()));
    source.appendChild(original);
    REQUIRE(StudioBackground::sourcePath(filter, folder.path()) == folder.filePath(QStringLiteral("оригинал.mp4")));

    source.appendChild(filter.cloneNode(true));
    REQUIRE(StudioBackground::sourcePath(source.lastChild().toElement(), folder.path()) == folder.filePath(QStringLiteral("оригинал.mp4")));
    entry.setAttribute(QStringLiteral("producer"), QStringLiteral("missing"));
    REQUIRE(StudioBackground::sourcePath(filter, folder.path()).isEmpty());
    entry.setAttribute(QStringLiteral("producer"), QStringLiteral("chain3"));
    project.documentElement().appendChild(source.cloneNode(true));
    REQUIRE(StudioBackground::sourcePath(filter, folder.path()).isEmpty());
}

TEST_CASE("Studio restores background masks before a timeline is built", "[Studio]")
{
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QDir cache(folder.filePath(QStringLiteral("studio-background")));
    REQUIRE(QDir().mkpath(cache.absolutePath()));
    QFile mask(cache.filePath(QStringLiteral("кадр.sbg")));
    REQUIRE(mask.open(QIODevice::WriteOnly));
    REQUIRE(mask.write("SBG0001", 7) == 7);
    mask.close();
    QDomDocument project;
    REQUIRE(project.setContent(QStringLiteral(R"(<mlt><profile frame_rate_num="30000" frame_rate_den="1001"/>
      <playlist><entry producer="clip" in="00:00:44.211" out="00:00:58.759">
      <filter><property name="mlt_service">studio.background</property>
      <property name="mask_asset">кадр.sbg</property></filter>
    </entry></playlist></mlt>)")));
    auto filter = project.elementsByTagName(QStringLiteral("filter")).at(0).toElement();
    StudioBackground::restoreMaskPaths(project, folder.path(), folder.path());
    REQUIRE(Xml::getXmlProperty(filter, QStringLiteral("_sbg_mask_path")) == mask.fileName());
    REQUIRE(Xml::getXmlProperty(filter, QStringLiteral("_sbg_clip_in")) == QLatin1String("1325"));
    qlonglong first = 0, duration = 0;
    REQUIRE(StudioBackground::entryRange(filter, 30000, 1001, first, duration));
    REQUIRE(first == 1325);
    REQUIRE(duration == 437);
    // A saved legacy document may resolve its data folder to Movies while Save As
    // has copied the immutable mask beside the new project.
    const QString otherData = folder.filePath(QStringLiteral("Movies"));
    StudioBackground::restoreMaskPaths(project, otherData, folder.path());
    REQUIRE(Xml::getXmlProperty(filter, QStringLiteral("_sbg_mask_path")) == mask.fileName());
    StudioBackground::restoreMaskPaths(project, {}, folder.path());
    REQUIRE(Xml::getXmlProperty(filter, QStringLiteral("_sbg_mask_path")) == mask.fileName());
    REQUIRE(QDir().mkpath(folder.filePath(QStringLiteral("others"))));
    const QString archived = folder.filePath(QStringLiteral("others/кадр.sbg"));
    REQUIRE(QFile::copy(mask.fileName(), archived));
    StudioBackground::restoreMaskPaths(project, otherData, folder.path());
    REQUIRE(Xml::getXmlProperty(filter, QStringLiteral("_sbg_mask_path")) == archived);
    StudioBackground::restoreMaskPaths(project, folder.path(), folder.path());
    REQUIRE(Xml::getXmlProperty(filter, QStringLiteral("_sbg_mask_path")) == mask.fileName());
    const QString liveRoot = folder.filePath(QStringLiteral("live SaveAs"));
    const QUrl liveUrl = QUrl::fromLocalFile(liveRoot + QStringLiteral("/копия.kdenlive"));
    const QString liveMask = liveRoot + QStringLiteral("/studio-background/кадр.sbg");
    REQUIRE(QDir().mkpath(QFileInfo(liveMask).absolutePath()));
    REQUIRE(QFile::copy(mask.fileName(), liveMask));
    REQUIRE(StudioBackground::maskPath(QStringLiteral("кадр.sbg"), folder.path(), folder.path(), liveUrl) == mask.fileName());
    REQUIRE(StudioBackground::maskPath(QStringLiteral("кадр.sbg"), otherData, folder.path(), liveUrl) == archived);
    REQUIRE(QFile::remove(archived));
    REQUIRE(QFile::remove(mask.fileName()));
    StudioBackground::restoreMaskPaths(project, folder.path(), folder.path());
    REQUIRE(Xml::getXmlProperty(filter, QStringLiteral("_sbg_mask_path")).isEmpty());
    REQUIRE(StudioBackground::maskPath(QStringLiteral("кадр.sbg"), otherData, folder.path(), liveUrl) == liveMask);
    REQUIRE(QDir().mkpath(liveRoot + QStringLiteral("/others")));
    const QString liveArchive = liveRoot + QStringLiteral("/others/кадр.sbg");
    REQUIRE(QFile::copy(liveMask, liveArchive));
    REQUIRE(StudioBackground::maskPath(QStringLiteral("кадр.sbg"), otherData, folder.path(), liveUrl) == liveArchive);
    {
        const QString previousDirectory = QDir::currentPath();
        auto restoreDirectory = qScopeGuard([previousDirectory] { QDir::setCurrent(previousDirectory); });
        REQUIRE(QDir::setCurrent(liveRoot)); // A cwd fallback would incorrectly find the real mask here.
        REQUIRE(StudioBackground::maskPath(QStringLiteral("кадр.sbg"), otherData, folder.path(), QUrl()).isEmpty());
        REQUIRE(StudioBackground::maskPath(QStringLiteral("кадр.sbg"), otherData, folder.path(), QUrl(QStringLiteral("file:"))).isEmpty());
        REQUIRE(StudioBackground::maskPath(QStringLiteral("кадр.sbg"), otherData, folder.path(), QUrl(QStringLiteral("https://example.invalid/project"))).isEmpty());
    }
    REQUIRE(StudioBackground::maskPath(QStringLiteral("../wrong.sbg"), otherData, folder.path(), liveUrl).isEmpty());
    REQUIRE(QFile::remove(liveArchive));
    REQUIRE(QFile::remove(liveMask));
    REQUIRE(StudioBackground::maskPath(QStringLiteral("кадр.sbg"), otherData, folder.path(), liveUrl).isEmpty());
    Xml::setXmlProperty(filter, QStringLiteral("mask_asset"), QStringLiteral("../wrong.sbg"));
    StudioBackground::restoreMaskPaths(project, folder.path(), folder.path());
    REQUIRE(Xml::getXmlProperty(filter, QStringLiteral("_sbg_mask_path")).isEmpty());
}

TEST_CASE("Studio validates a matte before reporting it applied", "[Studio]")
{
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString sourcePath = folder.filePath(QStringLiteral("клип.mp4"));
    QFile source(sourcePath);
    REQUIRE(source.open(QIODevice::WriteOnly));
    REQUIRE(source.write("source", 6) == 6);
    source.close();
    const QString sourceSha = StudioBackground::fileSha256(sourcePath);
    const QString recipeSha(64, QLatin1Char('a'));
    QByteArray mask(272, '\0');
    mask.replace(0, 8, QByteArray("SBG0001", 8));
    const auto put32 = [&mask](int at, quint32 value) {
        for (int i = 0; i < 4; ++i) mask[at + i] = char(value >> (8 * i));
    };
    put32(8, 1); put32(20, 1); put32(24, 30); put32(28, 1);
    put32(32, 320); put32(36, 180); put32(48, 256);
    mask.replace(56, 64, sourceSha.toLatin1());
    mask.replace(120, 64, recipeSha.toLatin1());
    put32(252, StudioBackground::crc32(mask.left(252)));
    const QString maskPath = folder.filePath(QStringLiteral("кадр.sbg"));
    QFile output(maskPath);
    REQUIRE(output.open(QIODevice::WriteOnly));
    REQUIRE(output.write(mask) == mask.size());
    output.close();
    StudioBackground::MaskRequest request{maskPath, sourcePath, sourceSha, recipeSha,
        768, 30, 1, 320, 180, 0, 1, false};
    REQUIRE(StudioBackground::validateMask(request).isEmpty());
    request.duration = 2;
    REQUIRE_FALSE(StudioBackground::validateMask(request).isEmpty());
    request.duration = 1;
    request.fpsNum = 25;
    REQUIRE_FALSE(StudioBackground::validateMask(request).isEmpty());
    request.fpsNum = 30;
    REQUIRE(source.open(QIODevice::WriteOnly | QIODevice::Append));
    REQUIRE(source.write("changed", 7) == 7);
    source.close();
    REQUIRE_FALSE(StudioBackground::validateMask(request).isEmpty());
}

TEST_CASE("Studio Audio page loads the schema from the selected prefix", "[StudioUI][StudioAudioPrefix]")
{
    QFile source(StudioResources::dataFile(QStringLiteral("studio-audio/parameters.json")));
    REQUIRE(source.open(QIODevice::ReadOnly));
    auto schema = QJsonDocument::fromJson(source.readAll()).object();
    auto actions = schema.value(QStringLiteral("actions")).toArray();
    REQUIRE(actions.size() == 4);
    auto voice = actions[0].toObject();
    voice.insert(QStringLiteral("label"), QStringLiteral("Голос из выбранного префикса"));
    actions[0] = voice;
    schema.insert(QStringLiteral("actions"), actions);
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString prefix = folder.filePath(QStringLiteral("префикс с пробелами"));
    REQUIRE(QDir().mkpath(prefix + QStringLiteral("/share/studio-audio")));
    QFile contract(prefix + QStringLiteral("/share/studio-audio/parameters.json"));
    REQUIRE(contract.open(QIODevice::WriteOnly));
    const QByteArray bytes = QJsonDocument(schema).toJson();
    REQUIRE(contract.write(bytes) == bytes.size());
    contract.close();
    const QByteArray oldPrefix = qgetenv("STUDIO_PREFIX"), oldData = qgetenv("STUDIO_DATA_DIR");
    const auto restore = qScopeGuard([&] {
        if (oldPrefix.isNull()) qunsetenv("STUDIO_PREFIX"); else qputenv("STUDIO_PREFIX", oldPrefix);
        if (oldData.isNull()) qunsetenv("STUDIO_DATA_DIR"); else qputenv("STUDIO_DATA_DIR", oldData);
    });
    qputenv("STUDIO_PREFIX", prefix.toUtf8());
    qunsetenv("STUDIO_DATA_DIR");
    REQUIRE(StudioResources::dataFile(QStringLiteral("studio-audio/parameters.json")) == contract.fileName());
    StudioAudioController controller;
    StudioAudioPage page(&controller);
    const auto tabs = page.findChild<QTabBar *>();
    REQUIRE(tabs);
    REQUIRE(tabs->count() == 4);
    REQUIRE(tabs->tabText(0) == voice.value(QStringLiteral("label")).toString());
}

static QByteArray studioAudioProcess(const QString &program, const QStringList &arguments, int limit = 30000) {
    QProcess command;
    command.start(program, arguments);
    if (!command.waitForFinished(limit)) { command.kill(); command.waitForFinished(1000); }
    INFO(command.readAllStandardError().toStdString());
    REQUIRE(command.state() == QProcess::NotRunning);
    REQUIRE(command.exitStatus() == QProcess::NormalExit);
    REQUIRE(command.exitCode() == 0);
    return command.readAllStandardOutput();
}

static void studioAudioCheckOwnership(const std::shared_ptr<TimelineItemModel> &timeline, const QStringList &names, int expected, const QSet<int> &muted) {
    int tracks = 0, results = 0;
    for (int track : timeline->getTracksIds(true)) {
        tracks += timeline->getTrackProperty(track, QStringLiteral("kdenlive:studio_audio_track")).toInt() == 1;
        for (int id : timeline->getItemsInRange(track, 0, -1, false)) {
            if (!timeline->isClip(id)) continue;
            const auto clip = pCore->projectItemModel()->getClipByBinID(timeline->getClipBinId(id));
            results += clip && clip->getProducerProperty(QStringLiteral("studio:audio:role")) == QLatin1String("result");
        }
    }
    REQUIRE(tracks == expected); REQUIRE(results == expected);
    for (int track : timeline->getTracksIds(true)) for (int id : timeline->getItemsInRange(track, 0, -1, false)) {
        if (!timeline->isClip(id)) continue;
        const auto clip = pCore->projectItemModel()->getClipByBinID(timeline->getClipBinId(id));
        if (!clip || !names.contains(QFileInfo(clip->getProducerProperty(QStringLiteral("resource"))).fileName())) continue;
        int own = 0, user = 0;
        for (const auto &effect : effectsById(timeline->getClipEffectStack(id), QStringLiteral("volume"))) {
            if (effect->getParam(QStringLiteral("studio:audio:role")) == QLatin1String("source-mute")) ++own;
            else { ++user; REQUIRE(effect->filter().anim_get_double("level", 0) == Approx(-3)); }
        }
        REQUIRE(user == 1); REQUIRE(own == (muted.contains(id) ? 1 : 0));
    }
}

static void studioAudioCheckPauseSync(const std::shared_ptr<TimelineItemModel> &timeline, bool cut) {
    const int removed = 600 - timeline->duration();
    REQUIRE(cut == (removed > 0));
    int videoEnd = 0;
    for (int track : timeline->getTracksIds(false)) for (int id : timeline->getItemsInRange(track, 0, -1, false))
        if (timeline->isClip(id)) videoEnd = std::max(videoEnd, timeline->getItemPosition(id) + timeline->getClipPlaytime(id));
    REQUIRE(videoEnd == timeline->duration());
    for (int sourceFrame : {60, 420}) {
        const int outputFrame = sourceFrame == 60 ? 60 : sourceFrame - removed;
        int mapped = -1;
        for (int track : timeline->getTracksIds(false)) for (int id : timeline->getItemsInRange(track, outputFrame, outputFrame + 1, false)) {
            if (!timeline->isClip(id)) continue;
            const int position = timeline->getItemPosition(id);
            if (position <= outputFrame && outputFrame < position + timeline->getClipPlaytime(id)) {
                const auto producer = timeline->getClipProducer(id); REQUIRE(producer);
                mapped = producer->get_in() + outputFrame - position;
            }
        }
        REQUIRE(mapped == sourceFrame);
    }
    const auto cues = timeline->getSubtitleModel(); REQUIRE(cues);
    REQUIRE(cues->getAllSubtitles().size() == (cut ? 2 : 3));
    REQUIRE(cues->getIdForStartPos(0, GenTime(420 - removed, 60)) >= 0);
    const auto guides = timeline->getGuideModel()->getAllMarkers();
    REQUIRE(guides.size() == (cut ? 1 : 2));
    REQUIRE(guides.back().time().frames(60) == 420 - removed);
}

static void studioAudioCheckAssets(const std::shared_ptr<TimelineItemModel> &timeline, const QString &saveRoot) {
    int results = 0;
    const QString root = QDir(saveRoot).absolutePath() + QStringLiteral("/studio-audio/");
    for (int track : timeline->getTracksIds(true)) for (int id : timeline->getItemsInRange(track, 0, -1, false)) {
        if (!timeline->isClip(id)) continue;
        const auto clip = pCore->projectItemModel()->getClipByBinID(timeline->getClipBinId(id));
        if (!clip || clip->getProducerProperty(QStringLiteral("studio:audio:role")) != QLatin1String("result")) continue;
        ++results;
        const auto producer = timeline->getClipProducer(id); REQUIRE(producer);
        for (bool timelineProducer : {false, true}) {
            const auto property = [&](const QString &name) {
                return timelineProducer ? QString::fromUtf8(producer->parent().get(name.toUtf8().constData())) : clip->getProducerProperty(name);
            };
            const auto path = [&](const QString &name) {
                const QString value = property(name);
                return QDir::isRelativePath(value) ? QDir(saveRoot).absoluteFilePath(value) : value;
            };
            const QString audio = path(QStringLiteral("resource")), reportPath = path(QStringLiteral("studio:audio:report"));
            INFO("Audio producer=" << timelineProducer << " resource=" << audio.toStdString() << " report=" << reportPath.toStdString());
            REQUIRE(QFileInfo(audio).absoluteFilePath().startsWith(root));
            REQUIRE(QFileInfo(reportPath).absoluteFilePath().startsWith(root));
            REQUIRE(QFileInfo(audio).isFile()); REQUIRE(QFileInfo(reportPath).isFile());
            QFile report(reportPath); REQUIRE(report.open(QIODevice::ReadOnly));
            const auto data = QJsonDocument::fromJson(report.readAll()).object();
            const QString hash = fileSha256(audio);
            REQUIRE(hash.size() == 64);
            REQUIRE(hash == property(QStringLiteral("studio:audio:sha256")));
            REQUIRE(hash == data.value(QStringLiteral("audio_sha256")).toString());
        }
    }
    REQUIRE(results == 1);
}

static double studioAudioMeter(const QString &ffmpeg, const QString &audio, const QString &filter) {
    QProcess measure;
    measure.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-i"), audio,
        QStringLiteral("-af"), filter + QStringLiteral(",ebur128=peak=true"), QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")});
    if (!measure.waitForFinished(30000)) { measure.kill(); measure.waitForFinished(1000); }
    const QString output = QString::fromUtf8(measure.readAllStandardError());
    INFO(output.toStdString());
    REQUIRE(measure.exitStatus() == QProcess::NormalExit); REQUIRE(measure.exitCode() == 0);
    const auto value = QRegularExpression(QStringLiteral("I:\\s*(-?[0-9.]+) LUFS")).match(output.mid(output.lastIndexOf(QStringLiteral("Summary:"))));
    REQUIRE(value.hasMatch());
    return value.captured(1).toDouble();
}

static QJsonObject studioAudioExportEvidence(const std::shared_ptr<TimelineItemModel> &model, const QString &folder, int mode,
    const QString &referenceScene)
{
    const QString ffmpeg = StudioResources::executable(QStringLiteral("ffmpeg"));
    const QString melt = StudioResources::executable(QStringLiteral("melt"));
    REQUIRE_FALSE(ffmpeg.isEmpty()); REQUIRE_FALSE(melt.isEmpty());
    const QString project = pCore->currentDoc()->url().toLocalFile();
    const QString scene = QDir(folder).filePath(QStringLiteral("export-%1.mlt").arg(mode)), audio = QDir(folder).filePath(QStringLiteral("export-%1.wav").arg(mode));
    QFile xml(scene); REQUIRE(xml.open(QIODevice::WriteOnly));
    const QByteArray bytes = pCore->projectManager()->projectSceneList(folder, true).first.toUtf8();
    REQUIRE_FALSE(bytes.isEmpty()); REQUIRE(xml.write(bytes) == bytes.size()); xml.close();
    REQUIRE(model->duration() > 0);
    studioAudioProcess(melt, {scene, QStringLiteral("in=0"), QStringLiteral("out=%1").arg(model->duration() - 1),
        QStringLiteral("-consumer"), QStringLiteral("avformat:") + audio, QStringLiteral("vn=1"),
        QStringLiteral("acodec=pcm_s16le"), QStringLiteral("ar=48000"), QStringLiteral("ac=2"), QStringLiteral("f=wav"),
        QStringLiteral("threads=2"), QStringLiteral("real_time=-1")}, 60000);
    const double loudness = studioAudioMeter(ffmpeg, audio, QStringLiteral("anull")); REQUIRE(loudness > -60);
    QJsonObject row{{QStringLiteral("mode"), mode}, {QStringLiteral("project"), project}, {QStringLiteral("export"), audio},
        {QStringLiteral("audio_sha256"), fileSha256(audio)}, {QStringLiteral("lufs"), loudness}, {QStringLiteral("duration_frames"), model->duration()}};
    if (mode < 2) {
        const double first = studioAudioMeter(ffmpeg, audio, QStringLiteral("atrim=start=0:end=4,asetpts=PTS-STARTPTS"));
        const double second = studioAudioMeter(ffmpeg, audio, QStringLiteral("atrim=start=6:end=10,asetpts=PTS-STARTPTS"));
        row.insert(QStringLiteral("difference_lu"), std::abs(first - second));
        REQUIRE(std::abs(first - second) <= 1.0);
    }
    if (mode == 2) {
        const double speech = studioAudioMeter(ffmpeg, audio, QStringLiteral("atrim=start=1:end=3,asetpts=PTS-STARTPTS,bandpass=f=880:w=40"));
        const double pause = studioAudioMeter(ffmpeg, audio, QStringLiteral("atrim=start=4.5:end=5.5,asetpts=PTS-STARTPTS,bandpass=f=880:w=40"));
        row.insert(QStringLiteral("music_duck_lu"), pause - speech); REQUIRE(pause - speech > 4);
    }
    if (mode == 3) {
        REQUIRE(QFileInfo(referenceScene).isFile());
        QJsonArray controls;
        const int removed = 600 - model->duration();
        for (int sourceFrame : {60, 420}) {
            const int outputFrame = sourceFrame == 60 ? 60 : sourceFrame - removed;
            QStringList hashes;
            for (bool processed : {false, true}) {
                const int frame = processed ? outputFrame : sourceFrame;
                const QString png = QDir(folder).filePath(QStringLiteral("pause-%1-%2.png").arg(processed ? QStringLiteral("output") : QStringLiteral("source")).arg(sourceFrame));
                studioAudioProcess(melt, {processed ? scene : referenceScene, QStringLiteral("in=%1").arg(frame), QStringLiteral("out=%1").arg(frame),
                    QStringLiteral("-consumer"), QStringLiteral("avformat:") + png, QStringLiteral("vcodec=png"), QStringLiteral("an=1"),
                    QStringLiteral("f=image2"), QStringLiteral("threads=1"), QStringLiteral("real_time=-1")}, 60000);
                // Compare the upper half; native ASS subtitles occupy the bottom of these known inputs.
                const QByteArray rgba = studioAudioProcess(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"),
                    QStringLiteral("-filter_threads"), QStringLiteral("1"), QStringLiteral("-i"), png,
                    QStringLiteral("-vf"), QStringLiteral("crop=iw:ih/2:0:0"), QStringLiteral("-pix_fmt"), QStringLiteral("rgba"),
                    QStringLiteral("-f"), QStringLiteral("rawvideo"), QStringLiteral("pipe:1")});
                REQUIRE(rgba.size() == 1920 * 540 * 4);
                hashes << QString::fromLatin1(QCryptographicHash::hash(rgba, QCryptographicHash::Sha256).toHex());
            }
            INFO("Pause image source=" << sourceFrame << " output=" << outputFrame << " hashes=" << hashes.join(QLatin1Char(',')).toStdString());
            REQUIRE(hashes[0] == hashes[1]);
            controls << QJsonObject{{QStringLiteral("source_frame"), sourceFrame}, {QStringLiteral("output_frame"), outputFrame},
                {QStringLiteral("upper_half_sha256"), hashes[0]}};
        }
        row.insert(QStringLiteral("pause_frame_controls"), controls);
    }
    return row;
}

TEST_CASE("Studio Audio real panel processes modes without duplicating or losing user audio", "[StudioAudioGUI][.gui]")
{
    REQUIRE_FALSE(pCore->window()); // Run this GUI fixture in its own test process.
    QTemporaryDir folder(QDir::temp().filePath(QStringLiteral("studio-audio-panel-XXXXXX")));
    REQUIRE(folder.isValid());
    bool passed = false;
    const auto keepFailure = qScopeGuard([&] { if (!passed) folder.setAutoRemove(false); });
    INFO("Audio GUI evidence: " << folder.path().toStdString());
    const int oldLocation = KdenliveSettings::videotodefaultfolder();
    const QString oldProfile = KdenliveSettings::default_profile();
    const auto restore = qScopeGuard([&] {
        KdenliveSettings::setVideotodefaultfolder(oldLocation);
        KdenliveSettings::setDefault_profile(oldProfile);
    });
    KdenliveSettings::setVideotodefaultfolder(KdenliveDoc::SaveToProjectFolder);
    KdenliveSettings::setDefault_profile(QStringLiteral("atsc_1080p_60"));
    const QString ffmpeg = StudioResources::executable(QStringLiteral("ffmpeg"));
    const QString melt = StudioResources::executable(QStringLiteral("melt"));
    REQUIRE_FALSE(ffmpeg.isEmpty()); REQUIRE_FALSE(melt.isEmpty());
    REQUIRE_FALSE(StudioResources::executable(QStringLiteral("studio-audio")).isEmpty());
    const QStringList names{QStringLiteral("громкий голос.wav"), QStringLiteral("тихий голос.wav"), QStringLiteral("музыка.wav")};
    for (int i = 0; i < names.size(); ++i) {
        studioAudioProcess(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i"), QStringLiteral("sine=frequency=%1:sample_rate=48000:duration=%2").arg(i == 2 ? 880 : 440).arg(i == 2 ? 10 : 4),
            QStringLiteral("-af"), QStringLiteral("volume=%1").arg(i == 0 ? .8 : i == 1 ? .08 : .4), folder.filePath(names[i])});
    }
    pCore->initGUI(QString(), QUrl());
    const auto closeDocument = qScopeGuard([] {
        if (pCore->currentDoc()) pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(studioWait([] { return pCore->currentDoc() && pCore->window()->getCurrentTimeline()
        && pCore->window()->getCurrentTimeline()->model(); }, 15000));
    auto model = pCore->window()->getCurrentTimeline()->model();
    const auto audioTracks = model->getTracksIds(true);
    REQUIRE(audioTracks.size() >= 2);
    for (int i = 0; i < names.size(); ++i) {
        const QByteArray path = QFile::encodeName(folder.filePath(names[i]));
        auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), path.constData());
        REQUIRE(producer->is_valid());
        const auto bin = pCore->projectItemModel();
        QString binId = QString::number(bin->getFreeClipId());
        Fun undo = [] { return true; }, redo = [] { return true; };
        REQUIRE(bin->requestAddBinClip(binId, producer, bin->getRootFolder()->clipId(), undo, redo));
        int clipId = -1;
        REQUIRE(model->requestClipInsertion(QStringLiteral("A") + binId, audioTracks[i == 2 ? 1 : 0], i == 1 ? 360 : 0, clipId));
        REQUIRE(model->requestItemResize(clipId, i == 2 ? 600 : 240, true, true) == (i == 2 ? 600 : 240));
        const auto stack = model->getClipEffectStack(clipId);
        REQUIRE(stack->appendEffectWithUndo(QStringLiteral("volume"), undo, redo).first);
        const auto volume = effectsById(stack, QStringLiteral("volume"));
        REQUIRE(volume.size() == 1);
        appendParameterChange(volume.front(), {QStringLiteral("level")}, {QStringLiteral("-3")}, undo, redo);
    }
    const QString videoPath = folder.filePath(QStringLiteral("контрольный видеоряд.mkv"));
    studioAudioProcess(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-filter_threads"), QStringLiteral("1"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("testsrc2=size=320x180:rate=60"),
        QStringLiteral("-frames:v"), QStringLiteral("600"), QStringLiteral("-an"), QStringLiteral("-c:v"), QStringLiteral("ffv1"),
        QStringLiteral("-threads"), QStringLiteral("2"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), videoPath});
    QString videoBin;
    {
        const QByteArray videoBytes = QFile::encodeName(videoPath);
        auto videoProducer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), videoBytes.constData());
        REQUIRE(videoProducer->is_valid()); REQUIRE(videoProducer->get_length() == 600);
        videoBin = QString::number(pCore->projectItemModel()->getFreeClipId());
        Fun videoUndo = [] { return true; }, videoRedo = [] { return true; };
        REQUIRE(pCore->projectItemModel()->requestAddBinClip(videoBin, videoProducer, pCore->projectItemModel()->getRootFolder()->clipId(), videoUndo, videoRedo));
    }
    int videoId = -1;
    REQUIRE(model->requestClipInsertion(videoBin, model->getTracksIds(false).front(), 0, videoId));
    REQUIRE(model->requestItemResize(videoId, 600, true, true) == 600);
    {
        const auto subtitles = model->createSubtitleModel();
        REQUIRE(subtitles);
        const QString srtPath = folder.filePath(QStringLiteral("реплики.srt"));
        QFile srt(srtPath); REQUIRE(srt.open(QIODevice::WriteOnly));
        srt.write(QStringLiteral("1\n00:00:01,000 --> 00:00:02,000\nДо паузы\n\n2\n00:00:04,500 --> 00:00:05,000\nВ паузе\n\n3\n00:00:07,000 --> 00:00:08,000\nПосле паузы\n").toUtf8());
        srt.close(); subtitles->importSubtitle(srtPath, 0, true); REQUIRE(subtitles->getAllSubtitles().size() == 3);
    }
    REQUIRE(model->getGuideModel()->addMarker(GenTime(270, 60), QStringLiteral("В паузе")));
    REQUIRE(model->getGuideModel()->addMarker(GenTime(420, 60), QStringLiteral("После паузы")));
    const QString original = folder.filePath(QStringLiteral("исходный проект"));
    REQUIRE(QDir().mkpath(original));
    const QString baseline = QDir(original).filePath(QStringLiteral("исходник.kdenlive"));
    REQUIRE(pCore->projectManager()->saveFileAs(baseline));
    const QString referenceScene = folder.filePath(QStringLiteral("video-reference.mlt"));
    QDomDocument reference; REQUIRE(reference.setContent(pCore->projectManager()->projectSceneList(folder.path(), true).first));
    const auto referenceFilters = reference.elementsByTagName(QStringLiteral("filter"));
    bool copiedSubtitle = false;
    for (int i = 0; i < referenceFilters.size(); ++i) {
        auto filter = referenceFilters.at(i).toElement();
        if (Xml::getXmlProperty(filter, QStringLiteral("mlt_service")) == QLatin1String("avfilter.subtitles")) {
            const QString ass = folder.filePath(QStringLiteral("video-reference.ass"));
            REQUIRE_FALSE(copiedSubtitle); REQUIRE(QFile::copy(Xml::getXmlProperty(filter, QStringLiteral("av.filename")), ass));
            Xml::setXmlProperty(filter, QStringLiteral("av.filename"), ass); copiedSubtitle = true;
        }
    }
    REQUIRE(copiedSubtitle);
    QFile referenceFile(referenceScene); REQUIRE(referenceFile.open(QIODevice::WriteOnly));
    const auto referenceBytes = reference.toByteArray(); REQUIRE(referenceFile.write(referenceBytes) == referenceBytes.size()); referenceFile.close();
    const auto open = [&](const QString &project) {
        model.reset();
        REQUIRE(pCore->projectManager()->closeCurrentDocument(false, false));
        pCore->projectManager()->openFile(QUrl::fromLocalFile(project));
        REQUIRE(studioWait([&] { return pCore->currentDoc() && pCore->currentDoc()->url().toLocalFile() == project
            && pCore->window()->getCurrentTimeline() && pCore->window()->getCurrentTimeline()->model(); }, 15000));
        return pCore->window()->getCurrentTimeline()->model();
    };
    const auto sources = [&](const std::shared_ptr<TimelineItemModel> &timeline) {
        QMap<QString, int> ids;
        for (int track : timeline->getTracksIds(true)) for (int id : timeline->getItemsInRange(track, 0, -1, false)) {
            if (!timeline->isClip(id)) continue;
            const auto clip = pCore->projectItemModel()->getClipByBinID(timeline->getClipBinId(id));
            if (clip) for (const auto &name : names)
                if (QFileInfo(clip->getProducerProperty(QStringLiteral("resource"))).fileName() == name) ids.insert(name, id);
        }
        REQUIRE(ids.size() == 3);
        return ids;
    };
    QJsonArray evidence;
    for (int mode = 0; mode < 4; ++mode) {
        INFO("Audio panel mode=" << mode);
        model = open(baseline);
        auto ids = sources(model);
        const QSet<int> voice{ids.value(names[0]), ids.value(names[1])}, music{ids.value(names[2])};
        REQUIRE(model->requestSetSelection({ids.value(names[0]), ids.value(names[1])})); QApplication::processEvents();
        StudioPanel *panel = nullptr;
        for (auto widget : pCore->window()->findChildren<QWidget *>())
            if (auto candidate = dynamic_cast<StudioPanel *>(widget)) { panel = candidate; break; }
        REQUIRE(panel);
        QToolButton *navigation = nullptr;
        for (auto button : panel->findChildren<QToolButton *>()) if (button->text() == QStringLiteral("Звук")) navigation = button;
        REQUIRE(navigation); navigation->click(); QApplication::processEvents();
        auto page = panel->findChild<StudioAudioPage *>();
        auto controller = panel->findChild<StudioAudioController *>();
        REQUIRE(page); REQUIRE(controller);
        auto tabs = page->findChild<QTabBar *>(); REQUIRE(tabs); REQUIRE(tabs->count() == 4); tabs->setCurrentIndex(mode);
        for (auto slider : page->findChildren<QSlider *>()) if (slider->accessibleName() == QStringLiteral("Обработка голоса")) slider->setValue(0);
        if (mode == 2) {
            QString error;
            REQUIRE(controller->assignCurrentSelection(true, &error));
            REQUIRE(model->requestSetSelection({ids.value(names[2])})); QApplication::processEvents();
            REQUIRE(controller->assignCurrentSelection(false, &error));
        }
        QPushButton *main = nullptr;
        for (auto button : page->findChildren<QPushButton *>()) if (button->property("studioPrimary").toBool()) main = button;
        REQUIRE(main); REQUIRE(main->isEnabled());
        QStringList status;
        const auto connection = QObject::connect(controller, &StudioAudioController::statusChanged, page,
            [&](const QString &message, bool error) { status << (error ? QStringLiteral("ERROR: ") : QString()) + message; });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        const int before = pCore->undoStack()->index();
        const auto apply = [&] {
            status.clear(); main->click();
            REQUIRE(controller->busy());
            REQUIRE(studioWait([&] { return !controller->busy(); }, 120000));
            INFO(status.join(QLatin1Char('\n')).toStdString());
            REQUIRE_FALSE(status.filter(QStringLiteral("ERROR: ")).size());
        };
        apply();
        REQUIRE(pCore->undoStack()->index() == before + 1);
        const QSet<int> muted = mode == 3 ? QSet<int>{} : mode == 2 ? voice | music : voice;
        studioAudioCheckOwnership(model, names, mode == 3 ? 0 : 1, muted);
        if (mode == 3) { REQUIRE(model->duration() < 530); studioAudioCheckPauseSync(model, true); }
        pCore->undoStack()->undo(); studioAudioCheckOwnership(model, names, 0, {}); REQUIRE(model->duration() == 600);
        if (mode == 3) studioAudioCheckPauseSync(model, false);
        pCore->undoStack()->redo(); studioAudioCheckOwnership(model, names, mode == 3 ? 0 : 1, muted);
        if (mode == 3) studioAudioCheckPauseSync(model, true);
        if (mode != 3) { apply(); studioAudioCheckOwnership(model, names, 1, muted); }
        QString saveRoot = folder.filePath(QStringLiteral("перенос режима %1").arg(mode));
        REQUIRE(QDir().mkpath(saveRoot));
        QString project = QDir(saveRoot).filePath(QStringLiteral("режим-%1.kdenlive").arg(mode));
        REQUIRE(pCore->projectManager()->saveFileAs(project));
        const QString parked = folder.filePath(QStringLiteral("исходный проект недоступен %1").arg(mode));
        REQUIRE(QDir().rename(original, parked));
        if (mode == 3) {
            pCore->undoStack()->undo(); REQUIRE(model->duration() == 600); studioAudioCheckPauseSync(model, false);
            pCore->undoStack()->redo(); studioAudioCheckPauseSync(model, true);
        } else {
            studioAudioCheckAssets(model, saveRoot);
            pCore->undoStack()->undo(); studioAudioCheckOwnership(model, names, 1, muted); studioAudioCheckAssets(model, saveRoot);
            pCore->undoStack()->undo(); studioAudioCheckOwnership(model, names, 0, {});
            if (mode == 0) {
                const QString previousRoot = saveRoot;
                saveRoot = folder.filePath(QStringLiteral("перенос после отмены"));
                REQUIRE(QDir().mkpath(saveRoot));
                project = QDir(saveRoot).filePath(QStringLiteral("после отмены.kdenlive"));
                REQUIRE(pCore->projectManager()->saveFileAs(project));
                REQUIRE(QDir().rename(previousRoot, folder.filePath(QStringLiteral("прежний перенос недоступен"))));
            }
            pCore->undoStack()->redo(); studioAudioCheckOwnership(model, names, 1, muted); studioAudioCheckAssets(model, saveRoot);
            pCore->undoStack()->redo(); studioAudioCheckOwnership(model, names, 1, muted); studioAudioCheckAssets(model, saveRoot);
        }
        if (mode == 0) {
            const auto document = pCore->currentDoc();
            const auto history = document->property("_studioAudioAssetPaths").toMap();
            QSet<QString> waves;
            for (const auto &value : history) if (value.toString().endsWith(QStringLiteral("/result.wav"))) waves.insert(value.toString());
            REQUIRE(waves.size() == 2);
            QString active;
            for (int track : model->getTracksIds(true)) for (int id : model->getItemsInRange(track, 0, -1, false)) {
                if (!model->isClip(id)) continue;
                const auto clip = pCore->projectItemModel()->getClipByBinID(model->getClipBinId(id));
                if (clip && clip->getProducerProperty(QStringLiteral("studio:audio:role")) == QLatin1String("result"))
                    active = clip->getProducerProperty(QStringLiteral("resource"));
            }
            REQUIRE(waves.remove(active)); REQUIRE(waves.size() == 1);
            const QString historical = *waves.cbegin(), historicalReport = QDir(QFileInfo(historical).absolutePath()).filePath(QStringLiteral("report.json"));
            const QString guardRoot = folder.filePath(QStringLiteral("отклонённое сохранение"));
            const QString destination = QDir(guardRoot).filePath(QStringLiteral("studio-audio/%1/result.wav").arg(QFileInfo(historical).dir().dirName()));
            REQUIRE(QDir().mkpath(QFileInfo(destination).absolutePath()));
            const QString guardProject = QDir(guardRoot).filePath(QStringLiteral("не изменять.kdenlive"));
            REQUIRE(QFile::copy(project, guardProject));
            const QString projectHash = fileSha256(project), guardHash = fileSha256(guardProject), historicalHash = fileSha256(historical);
            const int undoIndex = pCore->undoStack()->index();
            const auto rejectedSave = [&] {
                REQUIRE_FALSE(pCore->projectManager()->saveFileAs(guardProject, true));
                REQUIRE(pCore->currentDoc() == document); REQUIRE(document->url().toLocalFile() == project);
                REQUIRE(pCore->undoStack()->index() == undoIndex);
                REQUIRE(document->property("_studioAudioAssetPaths").toMap() == history);
                REQUIRE(fileSha256(project) == projectHash); REQUIRE(fileSha256(guardProject) == guardHash);
                REQUIRE(fileSha256(historical) == historicalHash);
            };
            QFile conflict(destination); REQUIRE(conflict.open(QIODevice::WriteOnly));
            REQUIRE(conflict.write("conflicting historical WAV") == 26); conflict.close();
            rejectedSave(); REQUIRE(QFile::remove(destination));
            REQUIRE(QFile::rename(guardProject + QStringLiteral(".ass"), guardProject + QStringLiteral(".after-conflict.ass")));
            const QString missing = historicalReport + QStringLiteral(".qa-missing");
            REQUIRE(QFile::rename(historicalReport, missing));
            const auto restoreReport = qScopeGuard([&] { if (QFileInfo(missing).exists()) QFile::rename(missing, historicalReport); });
            rejectedSave(); REQUIRE(QFile::rename(missing, historicalReport));
        }
        REQUIRE(pCore->projectManager()->saveFileAs(project));
        model = open(project);
        ids = sources(model);
        QSet<int> reopenedMuted;
        if (mode != 3) { reopenedMuted = {ids.value(names[0]), ids.value(names[1])}; if (mode == 2) reopenedMuted.insert(ids.value(names[2])); }
        studioAudioCheckOwnership(model, names, mode == 3 ? 0 : 1, reopenedMuted);
        if (mode != 3) studioAudioCheckAssets(model, saveRoot);
        if (mode == 3) studioAudioCheckPauseSync(model, true);
        auto row = studioAudioExportEvidence(model, folder.path(), mode, referenceScene);
        row.insert(QStringLiteral("save_guards_checked"), mode == 0);
        evidence << row;
        QFile report(folder.filePath(QStringLiteral("evidence.json"))); REQUIRE(report.open(QIODevice::WriteOnly));
        report.write(QJsonDocument(evidence).toJson());
        REQUIRE(QDir().rename(parked, original));
    }
    model.reset();
    REQUIRE(pCore->projectManager()->closeCurrentDocument(false, false));
    folder.setAutoRemove(false); passed = true;
}

TEST_CASE("Studio Audio Save As resource remap keeps the Undo action enabled", "[StudioAudioUndoGUI][.gui]")
{
    REQUIRE_FALSE(pCore->window()); // A separate process owns the real MainWindow.
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString source = folder.filePath(QStringLiteral("исходник/result.wav"));
    const QString moved = folder.filePath(QStringLiteral("копия/result.wav"));
    REQUIRE(QDir().mkpath(QFileInfo(source).absolutePath()));
    REQUIRE(QDir().mkpath(QFileInfo(moved).absolutePath()));
    QFile sound(source);
    REQUIRE(sound.open(QIODevice::WriteOnly));
    QDataStream wave(&sound);
    wave.setByteOrder(QDataStream::LittleEndian);
    wave.writeRawData("RIFF", 4); wave << quint32(36 + 48000 * 2);
    wave.writeRawData("WAVEfmt ", 8);
    wave << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000) << quint16(2) << quint16(16);
    wave.writeRawData("data", 4); wave << quint32(48000 * 2);
    for (int i = 0; i < 48000; ++i) wave << qint16(i % 48 < 24 ? 4000 : -4000);
    sound.close();
    REQUIRE(QFile::copy(source, moved));
    pCore->initGUI(QString(), QUrl());
    const auto closeDocument = qScopeGuard([] {
        if (pCore->currentDoc()) pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(studioWait([] { return pCore->currentDoc() && pCore->window()->getCurrentTimeline()
        && pCore->window()->getCurrentTimeline()->model(); }, 15000));
    const auto model = pCore->window()->getCurrentTimeline()->model();
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), QFile::encodeName(source).constData());
    REQUIRE(producer->is_valid());
    producer->set("studio:audio:role", "result");
    const auto bin = pCore->projectItemModel();
    QString binId = QString::number(bin->getFreeClipId());
    Fun undo = [] { return true; }, redo = [] { return true; };
    REQUIRE(bin->requestAddBinClip(binId, producer, bin->getRootFolder()->clipId(), undo, redo));
    int clipId = -1;
    REQUIRE_FALSE(model->getTracksIds(true).isEmpty());
    REQUIRE(model->requestClipInsertion(QStringLiteral("A") + binId, model->getTracksIds(true).front(), 0, clipId));
    const ObjectId owner(KdenliveObjectType::BinClip, binId.toInt(), QUuid());
    REQUIRE(studioWait([&] { return !pCore->taskManager.hasPendingJob(owner) && pCore->taskManager.backgroundIdle(); }, 15000));
    auto *undoAction = pCore->window()->actionCollection()->action(QStringLiteral("edit_undo"));
    REQUIRE(undoAction);
    REQUIRE(pCore->undoStack()->canUndo());
    REQUIRE(undoAction->isEnabled());
    const int index = pCore->undoStack()->index();
    // Exact live Save As remap, isolated from the separate cachefiles KIO move/revert path.
    pCore->currentDoc()->updateStudioEffectAssetPaths({{source, moved}});
    REQUIRE(studioWait([&] { return !pCore->taskManager.hasPendingJob(owner) && pCore->taskManager.backgroundIdle(); }, 15000));
    QApplication::processEvents(); // Deliver the queued enableUndo(false) on the unfixed source.
    REQUIRE(bin->getClipByBinID(binId)->getProducerProperty(QStringLiteral("resource")) == moved);
    REQUIRE(pCore->undoStack()->canUndo());
    REQUIRE(undoAction->isEnabled());
    undoAction->trigger();
    REQUIRE(pCore->undoStack()->index() == index - 1);
    REQUIRE_FALSE(model->isClip(clipId));
}

TEST_CASE("Studio saved Audio project reopens and exports in a fresh GUI process", "[StudioAudioColdGUI][.gui]")
{
    REQUIRE_FALSE(pCore->window());
    const QString project = qEnvironmentVariable("STUDIO_QA_AUDIO_PROJECT");
    const QString reference = qEnvironmentVariable("STUDIO_QA_AUDIO_REFERENCE_SCENE");
    const QString expectedHash = qEnvironmentVariable("STUDIO_QA_AUDIO_EXPECTED_SHA256");
    bool modeOk = false, framesOk = false;
    const int mode = qEnvironmentVariable("STUDIO_QA_AUDIO_MODE").toInt(&modeOk);
    const int frames = qEnvironmentVariable("STUDIO_QA_AUDIO_EXPECTED_FRAMES").toInt(&framesOk);
    REQUIRE(modeOk); REQUIRE(mode >= 0); REQUIRE(mode < 4); REQUIRE(framesOk); REQUIRE(frames > 0);
    REQUIRE(expectedHash.size() == 64); REQUIRE(QFileInfo(project).isFile()); REQUIRE(QFileInfo(reference).isFile());
    QTemporaryDir folder(QDir::temp().filePath(QStringLiteral("studio-audio-cold-XXXXXX")));
    REQUIRE(folder.isValid()); folder.setAutoRemove(false);
    INFO("Audio cold GUI evidence: " << folder.path().toStdString());
    pCore->initGUI(QString(), QUrl::fromLocalFile(project));
    const auto closeDocument = qScopeGuard([] {
        if (pCore->currentDoc()) pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(studioWait([&] { return pCore->currentDoc() && pCore->currentDoc()->url().toLocalFile() == project
        && pCore->window()->getCurrentTimeline() && pCore->window()->getCurrentTimeline()->model(); }, 15000));
    const auto model = pCore->window()->getCurrentTimeline()->model();
    REQUIRE(model->duration() == frames);
    const QStringList names{QStringLiteral("громкий голос.wav"), QStringLiteral("тихий голос.wav"), QStringLiteral("музыка.wav")};
    QSet<int> muted;
    for (int track : model->getTracksIds(true)) for (int id : model->getItemsInRange(track, 0, -1, false)) {
        if (!model->isClip(id)) continue;
        const auto clip = pCore->projectItemModel()->getClipByBinID(model->getClipBinId(id));
        const QString name = clip ? QFileInfo(clip->getProducerProperty(QStringLiteral("resource"))).fileName() : QString();
        if (mode != 3 && (name == names[0] || name == names[1] || (mode == 2 && name == names[2]))) muted.insert(id);
    }
    REQUIRE(muted.size() == (mode == 3 ? 0 : mode == 2 ? 3 : 2));
    studioAudioCheckOwnership(model, names, mode == 3 ? 0 : 1, muted);
    if (mode != 3) studioAudioCheckAssets(model, QFileInfo(project).absolutePath());
    if (mode == 3) studioAudioCheckPauseSync(model, true);
    auto row = studioAudioExportEvidence(model, folder.path(), mode, reference);
    REQUIRE(row.value(QStringLiteral("audio_sha256")).toString() == expectedHash);
    row.insert(QStringLiteral("fresh_process"), true);
    QFile evidence(folder.filePath(QStringLiteral("evidence.json"))); REQUIRE(evidence.open(QIODevice::WriteOnly));
    evidence.write(QJsonDocument(row).toJson());
}

TEST_CASE("Studio audio failed launch and cancellation release the job", "[Studio]")
{
    auto &manager = pCore->taskManager;
    const bool wasBlocked = manager.isBlocked();
    manager.unBlock(); // This isolated controller fixture has no new document to reopen the lane.
    const auto restoreManager = qScopeGuard([&manager, wasBlocked] { manager.slotCancelJobs(wasBlocked); });
    StudioAudioController controller;
    QEventLoop loop;
    QObject::connect(&controller, &StudioAudioController::busyChanged, &loop, [&loop](bool busy) { if (!busy) loop.quit(); });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool reportedError = false;
    QString message;
    QObject::connect(&controller, &StudioAudioController::statusChanged, &loop,
                     [&reportedError, &message](const QString &text, bool error) { reportedError |= error; message = text; });
    StudioAudioTests::begin(controller, QStringLiteral("/definitely-missing-studio-worker"));
    timeout.start(3000);
    if (controller.busy()) loop.exec();
    REQUIRE_FALSE(controller.busy());
    REQUIRE(reportedError);
    REQUIRE(StudioAudioTests::process(controller)->error() == QProcess::FailedToStart);
    // The real installed 30-minute failure showed internal PROGRESS output as
    // its reason. Exercise that callback without replacing its production
    // deadline; the elapsed-time acceptance lives in the installed receipt.
    bool progressRead = false;
    QObject::connect(&controller, &StudioAudioController::progressChanged, &loop,
                     [&progressRead](int progress) { progressRead |= progress == 2; });
    reportedError = false;
    StudioAudioTests::begin(controller, QStringLiteral("/bin/sh"),
                            {QStringLiteral("-c"), QStringLiteral("printf 'PROGRESS\\t2\\tChecking filters\\n'; exec sleep 30")});
    REQUIRE(studioWait([&] { return progressRead && StudioAudioTests::process(controller)->state() == QProcess::Running; }, 3000));
    QList<QTimer *> deadlines;
    for (auto *timer : pCore->findChildren<QTimer *>()) {
        if (timer->isActive() && timer->interval() == 1800000) deadlines.push_back(timer);
    }
    REQUIRE(deadlines.size() == 1);
    REQUIRE(QMetaObject::invokeMethod(deadlines.front(), "timeout", Qt::DirectConnection));
    REQUIRE(studioWait([&] { return !controller.busy() && manager.backgroundIdle(); }, 3000));
    REQUIRE(reportedError);
    REQUIRE(message.contains(QStringLiteral("30")));
    REQUIRE(message.contains(QStringLiteral("лимит")));
    REQUIRE_FALSE(message.contains(QStringLiteral("PROGRESS")));
    // The same QProcess must distinguish a later ordinary worker failure.
    reportedError = false;
    StudioAudioTests::begin(controller, QStringLiteral("/bin/sh"),
                            {QStringLiteral("-c"), QStringLiteral("printf 'worker failure'; exit 7")});
    REQUIRE(studioWait([&] { return !controller.busy() && manager.backgroundIdle(); }, 3000));
    REQUIRE(reportedError);
    REQUIRE(message.contains(QStringLiteral("worker failure")));
    REQUIRE_FALSE(message.contains(QStringLiteral("лимит")));
    StudioAudioTests::begin(controller, QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exec sleep 30")});
    REQUIRE(studioWait([&] { return StudioAudioTests::process(controller)->state() == QProcess::Running; }, 3000));
    QElapsedTimer elapsed;
    elapsed.start();
    controller.cancel();
    REQUIRE(elapsed.elapsed() < 200);
    timeout.start(3000);
    if (controller.busy()) loop.exec();
    REQUIRE_FALSE(controller.busy());
    REQUIRE(StudioAudioTests::process(controller)->state() == QProcess::NotRunning);
    REQUIRE(message.contains(QStringLiteral("отменена")));
    REQUIRE_FALSE(message.contains(QStringLiteral("лимит")));
    int rollbacks = 0;
    StudioAudioTests::pendingImport(controller, rollbacks);
    controller.cancel();
    controller.cancel();
    REQUIRE(rollbacks == 1);
    REQUIRE_FALSE(controller.busy());
}

TEST_CASE("Studio rejects malformed pause cuts before editing", "[Studio]")
{
    QVector<QPoint> cuts;
    REQUIRE(parsePauseCuts({QJsonArray{.5, 1.0}, QJsonArray{2.0, 3.0}}, 60, 300, 60, cuts));
    REQUIRE(cuts == QVector<QPoint>{QPoint(180, 240), QPoint(90, 120)});
    const auto original = cuts;
    for (const auto &invalid : {QJsonArray{QJsonArray{-.5, 1.0}}, QJsonArray{QJsonArray{1.0, 9.0}},
                                QJsonArray{QJsonArray{.5, 1.5}, QJsonArray{1.0, 2.0}}, QJsonArray{QJsonArray{QStringLiteral("bad"), 1.0}}}) {
        REQUIRE_FALSE(parsePauseCuts(invalid, 60, 300, 60, cuts));
        REQUIRE(cuts == original);
    }
}

TEST_CASE("Studio selects the audio member of an AV pair", "[Studio]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QString binId = KdenliveTests::createProducerWithSound(pCore->getProjectProfile(), binModel, 100);
    QMap<int, QString> streams;
    streams.insert(1, QStringLiteral("stream1"));
    KdenliveTests::setAudioTargets(timeline, streams);
    int videoId = -1;
    REQUIRE(timeline->requestClipInsertion(binId, timeline->getTrackIndexFromPosition(2), 0, videoId));
    const int audioId = timeline->getClipSplitPartner(videoId);
    REQUIRE(audioId >= 0);
    REQUIRE(timeline->requestSetSelection({videoId, audioId}));
    const auto selected = StudioAudioTests::selected(timeline);
    REQUIRE(selected == QSet<int>{audioId});
    REQUIRE(timeline->requestItemResize(videoId, 30, true, true));
    int secondId = -1;
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/5/34").arg(binId), timeline->getTrackIndexFromPosition(2), 30, secondId));
    REQUIRE(timeline->requestSetSelection({videoId, secondId}));
    REQUIRE(timeline->studioTransitionSelection(10).status == TimelineModel::StudioTransitionStatus::Ready);
    REQUIRE(timeline->studioTransitionSelection(10).firstAvailable >= 5);
    REQUIRE(timeline->studioTransitionSelection(10).secondAvailable == 5);
    REQUIRE(timeline->studioTransitionSelection(1000).status == TimelineModel::StudioTransitionStatus::InsufficientFrames);
    const int before = undoStack->index();
    const QVector<QPair<QString, QVariant>> parameters{{QStringLiteral("0"), QStringLiteral("0=0;9=1")}, {QStringLiteral("1"), 0}, {QStringLiteral("2"), .45},
        {QStringLiteral("3"), 0}, {QStringLiteral("4"), .35}, {QStringLiteral("studio:audio_dip"), .25}, {QStringLiteral("studio:transition"), 1}};
    REQUIRE(timeline->mixClip(videoId, QStringLiteral("studio_transition"), 1, 10, parameters));
    REQUIRE(undoStack->index() == before + 1);
    REQUIRE(timeline->studioTransitionSelection().status == TimelineModel::StudioTransitionStatus::ExistingStudio);
    REQUIRE(timeline->studioTransitionModel(secondId)->getParam(QStringLiteral("0")) == QLatin1String("0=0;9=1"));
    const int secondAudio = timeline->getClipSplitPartner(secondId);
    auto audioMix = KdenliveTests::studioAudioMix(timeline, secondAudio);
    REQUIRE(audioMix);
    auto audioTransition = static_cast<Mlt::Transition *>(audioMix->getAsset());
    REQUIRE(audioTransition->get_double("studio:audio_dip") == Approx(.25));
    REQUIRE(audioTransition->filter_count() == 1);
    REQUIRE(QString::fromUtf8(std::unique_ptr<Mlt::Filter>(audioTransition->filter(0))->get("level")) == QLatin1String("0=0;5=-3;9=0"));
    REQUIRE(timeline->requestSetSelection({audioId}));
    REQUIRE(StudioAudioTests::selectedTrack(timeline) == QSet<int>{audioId, secondAudio});
    REQUIRE(timeline->requestSetSelection({videoId, secondId}));
    REQUIRE(timeline->getMixDuration(secondAudio) > 0);
    undoStack->undo();
    REQUIRE_FALSE(timeline->studioTransitionModel(secondId));
    REQUIRE(timeline->getMixDuration(secondAudio) == 0);
    undoStack->redo();
    REQUIRE(timeline->studioTransitionModel(secondId));
    audioMix = KdenliveTests::studioAudioMix(timeline, secondAudio);
    audioTransition = static_cast<Mlt::Transition *>(audioMix->getAsset());
    REQUIRE(audioTransition->filter_count() == 1);
    REQUIRE(timeline->getMixAlign(secondId) == MixAlignment::AlignCenter);
    REQUIRE(timeline->getMixAlign(secondAudio) == MixAlignment::AlignCenter);
    const int beforeDip = undoStack->index();
    REQUIRE(timeline->updateStudioTransition(10, {{QStringLiteral("studio:audio_dip"), .75}}));
    REQUIRE(undoStack->index() == beforeDip + 1);
    REQUIRE(timeline->studioTransitionModel(secondId)->getParam(QStringLiteral("studio:audio_dip")).toDouble() == Approx(.75));
    REQUIRE(QString::fromUtf8(std::unique_ptr<Mlt::Filter>(audioTransition->filter(0))->get("level")) == QLatin1String("0=0;5=-9;9=0"));
    bool copiedDip = false;
    for (const auto &parameter : KdenliveTests::studioAudioMixParams(timeline, secondAudio)) {
        if (parameter.first != QLatin1String("studio:audio_dip")) continue;
        REQUIRE(parameter.second.toDouble() == Approx(.75));
        copiedDip = true;
    }
    REQUIRE(copiedDip);
    undoStack->undo();
    REQUIRE(audioTransition->get_double("studio:audio_dip") == Approx(.25));
    undoStack->redo();
    REQUIRE(audioTransition->get_double("studio:audio_dip") == Approx(.75));
    REQUIRE(timeline->updateStudioTransition(10, {{QStringLiteral("studio:audio_dip"), 0.0}}));
    REQUIRE(audioTransition->filter_count() == 0);
    undoStack->undo();
    REQUIRE(audioTransition->get_double("studio:audio_dip") == Approx(.75));
    REQUIRE(audioTransition->filter_count() == 1);
    // Duration-only edits used to keep the old last keyframe (hard cut/plateau).
    REQUIRE(timeline->updateStudioTransition(8, {}));
    REQUIRE(timeline->getMixDuration(secondId) == 8);
    REQUIRE(timeline->getMixDuration(secondAudio) == 8);
    REQUIRE(timeline->studioTransitionModel(secondId)->getParam(QStringLiteral("0")) == QLatin1String("0=0;7=1"));
    REQUIRE(QString::fromUtf8(std::unique_ptr<Mlt::Filter>(audioTransition->filter(0))->get("level")) == QLatin1String("0=0;4=-9;7=0"));
    undoStack->undo();
    REQUIRE(timeline->studioTransitionModel(secondId)->getParam(QStringLiteral("0")) == QLatin1String("0=0;9=1"));
    undoStack->redo();
    REQUIRE(timeline->studioTransitionModel(secondId)->getParam(QStringLiteral("0")) == QLatin1String("0=0;7=1"));
    const int beforeFailedResize = undoStack->index();
    const int firstPosition = timeline->getItemPosition(videoId), secondPosition = timeline->getItemPosition(secondId);
    REQUIRE_FALSE(timeline->updateStudioTransition(1000, {{QStringLiteral("studio:audio_dip"), .5}}));
    REQUIRE(undoStack->index() == beforeFailedResize);
    REQUIRE(timeline->getMixDuration(secondId) == 8);
    REQUIRE(timeline->getMixDuration(secondAudio) == 8);
    REQUIRE(timeline->getItemPosition(videoId) == firstPosition);
    REQUIRE(timeline->getItemPosition(secondId) == secondPosition);
    REQUIRE(timeline->studioTransitionModel(secondId)->getParam(QStringLiteral("0")) == QLatin1String("0=0;7=1"));
    REQUIRE(timeline->removeStudioTransition());
    REQUIRE_FALSE(timeline->studioTransitionModel(secondId));
    undoStack->undo();
    REQUIRE(timeline->studioTransitionModel(secondId));
    audioMix = KdenliveTests::studioAudioMix(timeline, secondAudio);
    audioTransition = static_cast<Mlt::Transition *>(audioMix->getAsset());
    REQUIRE(audioTransition->get_double("studio:audio_dip") == Approx(.75));
    REQUIRE(audioTransition->filter_count() == 1);
    QTemporaryDir savedProjectDir;
    REQUIRE(savedProjectDir.isValid());
    const QString savedProject = savedProjectDir.filePath(QStringLiteral("transition-audio-dip.kdenlive"));
    REQUIRE(pCore->projectManager()->testSaveFileAs(savedProject));
    QFile savedFile(savedProject);
    REQUIRE(savedFile.open(QIODevice::ReadOnly));
    REQUIRE(savedFile.readAll().contains("studio:audio_dip"));
    const QString exported = savedProjectDir.filePath(QStringLiteral("стык с приглушением.mkv"));
    QProcess render;
    render.start(QStringLiteral("melt"), {savedProject, QStringLiteral("-consumer"), QStringLiteral("avformat:") + exported,
                                         QStringLiteral("vcodec=ffv1"), QStringLiteral("acodec=pcm_s16le"), QStringLiteral("real_time=-1")});
    REQUIRE(render.waitForFinished(120000));
    INFO(render.readAllStandardError().toStdString());
    REQUIRE(render.exitStatus() == QProcess::NormalExit);
    REQUIRE(render.exitCode() == 0);
    REQUIRE(QFileInfo(exported).size() > 1024);
    QProcess probe;
    probe.start(QStringLiteral("ffprobe"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                                             QStringLiteral("stream=codec_type"), QStringLiteral("-of"), QStringLiteral("csv=p=0"), exported});
    REQUIRE(probe.waitForFinished(30000));
    REQUIRE(probe.exitCode() == 0);
    const QByteArray exportedStreams = probe.readAllStandardOutput();
    REQUIRE(exportedStreams.contains("video"));
    REQUIRE(exportedStreams.contains("audio"));
    pCore->projectManager()->closeCurrentDocument(false, false);
    QUndoGroup reopenedUndo;
    auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(savedProject), savedProjectDir.path(), &reopenedUndo, false, nullptr);
    REQUIRE(opened.isSuccessful());
    auto reopened = opened.getDocument();
    pCore->projectManager()->testSetDocument(reopened.get());
    KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(savedProject).lastModified(), 0);
    const auto sequenceId = pCore->projectItemModel()->getAllSequenceClips().value(reopened->uuid());
    pCore->projectManager()->openTimeline(sequenceId, -1, reopened->uuid());
    auto reopenedTimeline = reopened->getTimeline(reopened->uuid());
    pCore->projectManager()->testSetActiveTimeline(reopenedTimeline);
    const int reopenedSecond = reopenedTimeline->getClipByPosition(reopenedTimeline->getTrackIndexFromPosition(2), 40);
    REQUIRE(reopenedSecond >= 0);
    const int reopenedAudio = reopenedTimeline->getClipSplitPartner(reopenedSecond);
    REQUIRE(reopenedAudio >= 0);
    auto reopenedAudioMix = KdenliveTests::studioAudioMix(reopenedTimeline, reopenedAudio);
    REQUIRE(reopenedAudioMix);
    auto reopenedTransition = static_cast<Mlt::Transition *>(reopenedAudioMix->getAsset());
    REQUIRE(reopenedTransition->get_double("studio:audio_dip") == Approx(.75));
    REQUIRE(reopenedTransition->filter_count() == 1);
    REQUIRE(QString::fromUtf8(std::unique_ptr<Mlt::Filter>(reopenedTransition->filter(0))->get("level")) == QLatin1String("0=0;4=-9;7=0"));
    pCore->projectManager()->closeCurrentDocument(false, false);
}

TEST_CASE("Studio mixes adjacent clips with handles on one side", "[Studio]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel, 100, true);
    const int track = timeline->getTrackIndexFromPosition(2);
    int first = -1, second = -1;
    SECTION("no source frames before the second clip") {
        REQUIRE(timeline->requestClipInsertion(binId, track, 0, first));
        REQUIRE(timeline->requestItemResize(first, 30, true, true));
        REQUIRE(timeline->requestClipInsertion(binId, track, 30, second));
        REQUIRE(timeline->requestItemResize(second, 30, true, true));
    }
    SECTION("no source frames after the first clip") {
        REQUIRE(timeline->requestClipInsertion(binId, track, 0, first));
        REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/10/39").arg(binId), track, 100, second));
    }
    REQUIRE(timeline->requestSetSelection({first, second}));
    const auto selection = timeline->studioTransitionSelection(10);
    REQUIRE(selection.status == TimelineModel::StudioTransitionStatus::Ready);
    REQUIRE(selection.maxDuration >= 10);
    REQUIRE((selection.firstAvailable == 0 || selection.secondAvailable == 0));
    const int before = undoStack->index();
    const QVector<QPair<QString, QVariant>> parameters{{QStringLiteral("0"), QStringLiteral("0=0;9=1")},
                                                          {QStringLiteral("1"), 0}};
    REQUIRE(timeline->mixClip(first, QStringLiteral("studio_transition"), 1, 10, parameters));
    REQUIRE(undoStack->index() == before + 1);
    REQUIRE(timeline->getMixDuration(second) == 10);
    undoStack->undo();
    REQUIRE(timeline->getMixDuration(second) == 0);
    REQUIRE(timeline->getItemPosition(second) == (selection.firstAvailable == 0 ? 100 : 30));
    undoStack->redo();
    REQUIRE(timeline->getMixDuration(second) == 10);
    const auto alignment = timeline->getMixAlign(second);
    const int position = timeline->getItemPosition(second);
    const auto strength = timeline->studioTransitionModel(second)->getParam(QStringLiteral("2"));
    const int beforeEdit = undoStack->index();
    REQUIRE(timeline->updateStudioTransition(10, {{QStringLiteral("2"), .8}}));
    REQUIRE(undoStack->index() == beforeEdit + 1);
    REQUIRE(timeline->getMixAlign(second) == alignment);
    REQUIRE(timeline->getItemPosition(second) == position);
    REQUIRE(timeline->getMixDuration(second) == 10);
    undoStack->undo();
    REQUIRE(timeline->studioTransitionModel(second)->getParam(QStringLiteral("2")) == strength);
    REQUIRE(timeline->getItemPosition(second) == position);
    undoStack->redo();
    REQUIRE(timeline->getMixAlign(second) == alignment);
    REQUIRE(timeline->updateStudioTransition(8, {}));
    REQUIRE(timeline->getMixDuration(second) == 8);
    REQUIRE(timeline->getMixAlign(second) == alignment);

}

TEST_CASE("Studio refuses missing handles without rippling following clips", "[Studio]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QString red = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel, 30, true);
    const QString blue = KdenliveTests::createProducer(pCore->getProjectProfile(), "blue", binModel, 30, true);
    const QString green = KdenliveTests::createProducer(pCore->getProjectProfile(), "green", binModel, 30, true);
    const int track = timeline->getTrackIndexFromPosition(2);
    int first = -1, second = -1, third = -1;
    REQUIRE(timeline->requestClipInsertion(red, track, 0, first));
    REQUIRE(timeline->requestClipInsertion(blue, track, 30, second));
    REQUIRE(timeline->requestClipInsertion(green, track, 60, third));
    REQUIRE(timeline->requestSetSelection({first, second}));
    const int before = undoStack->index();
    const auto selection = timeline->studioTransitionSelection(10);
    REQUIRE(selection.status == TimelineModel::StudioTransitionStatus::InsufficientFrames);
    REQUIRE(selection.maxDuration == 0);
    REQUIRE_FALSE(timeline->mixClip(first, QStringLiteral("studio_transition"), 1, 10, {}));
    REQUIRE(undoStack->index() == before);
    REQUIRE(timeline->getMixDuration(second) == 0);
    REQUIRE(timeline->getItemPosition(second) == 30);
    REQUIRE(timeline->getItemPosition(third) == 60);
    REQUIRE(timeline->getClipPlaytime(first) == 30);
    REQUIRE(timeline->getClipPlaytime(second) == 30);
    REQUIRE(timeline->getClipPlaytime(third) == 30);
}

TEST_CASE("Studio refuses missing handles without touching a later AV group", "[Studio]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QString red = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel, 30, true);
    const QString blue = KdenliveTests::createProducer(pCore->getProjectProfile(), "blue", binModel, 30, true);
    const QString av = KdenliveTests::createProducerWithSound(pCore->getProjectProfile(), binModel, 100);
    KdenliveTests::setAudioTargets(timeline, {{1, QStringLiteral("stream1")}});
    const int videoTrack = timeline->getTrackIndexFromPosition(2);
    const int audioTrack = timeline->getTrackIndexFromPosition(1);
    int first = -1, second = -1, precedingAudio = -1, later = -1;
    REQUIRE(timeline->requestClipInsertion(red, videoTrack, 0, first));
    REQUIRE(timeline->requestClipInsertion(blue, videoTrack, 30, second));
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("A") + av, audioTrack, 0, precedingAudio, true, false, false));
    REQUIRE(timeline->requestItemResize(precedingAudio, 60, true, true));
    REQUIRE(timeline->requestClipInsertion(av, videoTrack, 60, later));
    const int laterAudio = timeline->getClipSplitPartner(later);
    REQUIRE(laterAudio >= 0);
    REQUIRE(timeline->getItemPosition(laterAudio) == 60);
    REQUIRE(timeline->requestSetSelection({first, second}));
    const int before = undoStack->index();
    REQUIRE(timeline->studioTransitionSelection(10).status == TimelineModel::StudioTransitionStatus::InsufficientFrames);
    REQUIRE_FALSE(timeline->mixClip(first, QStringLiteral("studio_transition"), 1, 10, {}));
    REQUIRE(undoStack->index() == before);
    REQUIRE(timeline->getMixDuration(second) == 0);
    REQUIRE(timeline->getItemPosition(second) == 30);
    REQUIRE(timeline->getItemPosition(later) == 60);
    REQUIRE(timeline->getItemPosition(laterAudio) == 60);
    REQUIRE(timeline->getClipPlaytime(precedingAudio) == 60);
    REQUIRE(timeline->getClipIn(precedingAudio) == 0);
}

TEST_CASE("Studio transition ignores an unrelated clip spanning the cut", "[Studio]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QString red = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel, 100, true);
    const QString blue = KdenliveTests::createProducer(pCore->getProjectProfile(), "blue", binModel, 100, true);
    const QString green = KdenliveTests::createProducer(pCore->getProjectProfile(), "green", binModel, 90, true);
    const int videoTrack = timeline->getTrackIndexFromPosition(2);
    const int overlayTrack = timeline->getTrackIndexFromPosition(3);
    int first = -1, second = -1, overlay = -1;
    REQUIRE(timeline->requestClipInsertion(red, videoTrack, 0, first));
    REQUIRE(timeline->requestItemResize(first, 30, true, true));
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/10/39").arg(blue), videoTrack, 30, second));
    REQUIRE(timeline->requestClipInsertion(green, overlayTrack, 0, overlay));
    REQUIRE(timeline->requestSetSelection({first, second}));
    REQUIRE(timeline->studioTransitionSelection(10).status == TimelineModel::StudioTransitionStatus::Ready);
    const int before = undoStack->index();
    auto panel = std::make_unique<StudioPanel>();
    panel->resize(440, 700);
    panel->show();
    QToolButton *transitions = nullptr;
    for (auto button : panel->findChildren<QToolButton *>())
        if (button->property("studioNav").toBool() && button->text() == QStringLiteral("Переходы")) transitions = button;
    REQUIRE(transitions);
    transitions->click();
    QPushButton *add = nullptr;
    for (auto button : panel->findChildren<QPushButton *>())
        if (button->property("studioPrimary").toBool()) add = button;
    REQUIRE(add);
    REQUIRE(add->isEnabled());
    add->click();
    QApplication::processEvents();
    REQUIRE(timeline->getMixDuration(second) > 0);
    REQUIRE(timeline->studioTransitionSelection().status == TimelineModel::StudioTransitionStatus::ExistingStudio);
    REQUIRE(timeline->getItemPosition(overlay) == 0);
    REQUIRE(timeline->getClipPlaytime(overlay) == 90);
    REQUIRE(undoStack->index() == before + 1);
    undoStack->undo();
    REQUIRE(timeline->getMixDuration(second) == 0);
    REQUIRE(timeline->getItemPosition(second) == 30);
    REQUIRE(timeline->getItemPosition(overlay) == 0);
    REQUIRE(timeline->getClipPlaytime(overlay) == 90);
}

TEST_CASE("Studio does not mix a continuous source with the same frames", "[Studio]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QString source = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel, 100, true);
    const int track = timeline->getTrackIndexFromPosition(2);
    int first = -1, second = -1, third = -1;
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/0/29").arg(source), track, 0, first));
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/30/59").arg(source), track, 30, second));
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/60/79").arg(source), track, 60, third));
    REQUIRE(timeline->requestSetSelection({first, second}));
    const int before = undoStack->index();
    REQUIRE(timeline->studioTransitionSelection(10).status == TimelineModel::StudioTransitionStatus::SameSourceFrames);
    REQUIRE_FALSE(timeline->mixClip(first, QStringLiteral("studio_transition"), 1, 10, {}));
    REQUIRE(undoStack->index() == before);
    REQUIRE(timeline->getMixDuration(second) == 0);
    REQUIRE(timeline->getClipIn(first) == 0);
    REQUIRE(timeline->getClipIn(second) == 30);
    REQUIRE(timeline->getItemPosition(second) == 30);
    REQUIRE(timeline->getItemPosition(third) == 60);
}

TEST_CASE("Studio edits a saved same-frame mix without destructive repair", "[StudioRepair][.external]")
{
    const QString profilePath = GENERATE(QStringLiteral("atsc_1080p_25"), QStringLiteral("atsc_1080p_2997"));
    const QString previousProfile = pCore->getCurrentProfilePath();
    const auto restoreProfile = qScopeGuard([=] { pCore->setCurrentProfile(previousProfile); });
    REQUIRE(pCore->setCurrentProfile(profilePath));
    const bool throughPanel = GENERATE(false, true);
    const QString media = qEnvironmentVariable("STUDIO_QA_AV_MP4");
    REQUIRE(QFileInfo(media).isFile());
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), QFile::encodeName(media).constData());
    REQUIRE(producer->is_valid());
    REQUIRE(producer->get_length() >= 100);
    QString source = QString::number(binModel->getFreeClipId());
    Fun binUndo = [] { return true; }, binRedo = [] { return true; };
    REQUIRE(binModel->requestAddBinClip(source, producer, binModel->getRootFolder()->clipId(), binUndo, binRedo));
    const int track = timeline->getTrackIndexFromPosition(2);
    int first = -1, second = -1, third = -1;
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/0/29").arg(source), track, 0, first));
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/30/59").arg(source), track, 30, second));
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/60/79").arg(source), track, 60, third));
    Fun legacyUndo = [] { return true; }, legacyRedo = [] { return true; };
    REQUIRE(timeline->requestClipMix(QStringLiteral("studio_transition"), {first, second}, {5, 5}, track, 30,
        {{QStringLiteral("0"), QStringLiteral("0=0;9=1")}, {QStringLiteral("1"), .2727272727272727},
         {QStringLiteral("2"), .7}, {QStringLiteral("studio:sfx_level"), .3}}, true, true, true, legacyUndo, legacyRedo, false));
    REQUIRE(timeline->getClipIn(first) - timeline->getItemPosition(first)
            == timeline->getClipIn(second) - timeline->getItemPosition(second));
    const QString saved = folder.filePath(QStringLiteral("старый переход.kdenlive"));
    REQUIRE(pCore->projectManager()->testSaveFileAs(saved));
    legacyUndo = {}; legacyRedo = {};
    binUndo = {}; binRedo = {};
    producer.reset();
    undoStack->clear();
    binModel->clean();
    pCore->projectManager()->closeCurrentDocument(false, false);
    QUndoGroup reopenedUndo;
    auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(saved), folder.path(), &reopenedUndo, false, nullptr);
    REQUIRE(opened.isSuccessful());
    auto reopened = opened.getDocument();
    pCore->projectManager()->testSetDocument(reopened.get());
    const auto closeReopened = qScopeGuard([] {
        pCore->projectItemModel()->clean();
        pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(saved).lastModified(), 0));
    const auto sequence = binModel->getAllSequenceClips().value(reopened->uuid());
    pCore->projectManager()->openTimeline(sequence, -1, reopened->uuid());
    timeline = reopened->getTimeline(reopened->uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const int liveTrack = timeline->getTrackIndexFromPosition(2);
    first = timeline->getClipByPosition(liveTrack, 0);
    second = timeline->getClipByPosition(liveTrack, 40);
    third = timeline->getClipByPosition(liveTrack, 70);
    REQUIRE(timeline->requestSetSelection({first, second}));
    REQUIRE(timeline->studioTransitionSelection(10).status == TimelineModel::StudioTransitionStatus::ExistingStudio);
    REQUIRE(timeline->studioTransitionSelection(10).sameFrames);
    auto edits = pCore->undoStack();
    const int before = edits->index();
    const int originalSecondPosition = timeline->getItemPosition(second);
    if (throughPanel) {
        StudioPanel panel;
        panel.show();
        for (auto button : panel.findChildren<QToolButton *>())
            if (button->property("studioNav").toBool() && button->text() == QStringLiteral("Переходы")) { button->click(); break; }
        QDoubleSpinBox *softness = nullptr;
        for (auto button : panel.findChildren<QPushButton *>())
            REQUIRE(button->text() != QStringLiteral("Исправить переход"));
        for (auto spin : panel.findChildren<QDoubleSpinBox *>())
            if (spin->accessibleName() == QStringLiteral("Мягкость")) softness = spin;
        REQUIRE(softness);
        softness->setValue(80);
        QApplication::processEvents();
        REQUIRE(timeline->studioTransitionModel(second)->getParam(QStringLiteral("4")).toDouble() == Approx(.8));
    } else REQUIRE(timeline->updateStudioTransition(10, {{QStringLiteral("2"), .8}}));
    REQUIRE(edits->index() == before + 1);
    REQUIRE(timeline->getItemPosition(second) == originalSecondPosition);
    REQUIRE(timeline->getItemPosition(third) == 60);
    REQUIRE(timeline->studioTransitionSelection(10).sameFrames);
    REQUIRE(timeline->studioTransitionModel(second)->getParam(QStringLiteral("1")).toDouble() == Approx(.2727272727272727));
    REQUIRE(timeline->studioTransitionModel(second)->getParam(QStringLiteral("studio:sfx_level")).toDouble() == Approx(.3));
    edits->undo();
    REQUIRE(timeline->getItemPosition(third) == 60);
    edits->redo();
    REQUIRE(timeline->getItemPosition(third) == 60);
    REQUIRE(timeline->requestSetSelection({first, second}));
    REQUIRE(timeline->removeStudioTransition());
    REQUIRE(timeline->getMixDuration(second) == 0);
    REQUIRE(timeline->getItemPosition(third) == 60);
    edits->undo();
    REQUIRE(timeline->getMixDuration(second) == 10);
    REQUIRE(timeline->getItemPosition(third) == 60);
}

TEST_CASE("Studio refuses absent linked audio handles without editing the timeline", "[Studio]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    QMap<int, QString> streams;
    streams.insert(1, QStringLiteral("stream1"));
    KdenliveTests::setAudioTargets(timeline, streams);
    const QString binId = KdenliveTests::createProducerWithSound(pCore->getProjectProfile(), binModel, 30);
    const int track = timeline->getTrackIndexFromPosition(2);
    int first = -1, second = -1, third = -1;
    REQUIRE(timeline->requestClipInsertion(binId, track, 0, first));
    REQUIRE(timeline->requestClipInsertion(binId, track, 30, second));
    REQUIRE(timeline->requestClipInsertion(binId, track, 60, third));
    const int audioFirst = timeline->getClipSplitPartner(first);
    const int audioSecond = timeline->getClipSplitPartner(second);
    const int audioThird = timeline->getClipSplitPartner(third);
    REQUIRE(audioFirst >= 0);
    REQUIRE(audioSecond >= 0);
    REQUIRE(audioThird >= 0);
    REQUIRE(timeline->requestSetSelection({first, second}));
    const int before = undoStack->index();
    const auto selection = timeline->studioTransitionSelection(10);
    REQUIRE(selection.status == TimelineModel::StudioTransitionStatus::InsufficientFrames);
    REQUIRE(selection.audioMix);
    REQUIRE_FALSE(timeline->mixClip(first, QStringLiteral("studio_transition"), 1, 10, {}));
    REQUIRE(undoStack->index() == before);
    REQUIRE(timeline->getMixDuration(second) == 0);
    REQUIRE(timeline->getMixDuration(audioSecond) == 0);
    REQUIRE(timeline->getItemPosition(second) == 30);
    REQUIRE(timeline->getItemPosition(audioSecond) == 30);
    REQUIRE(timeline->getItemPosition(third) == 60);
    REQUIRE(timeline->getItemPosition(audioThird) == 60);
}

TEST_CASE("Studio renders all transition styles across a centered cut", "[Studio]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const int track = timeline->getTrackIndexFromPosition(2);
    const QString red = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel, 100, true);
    const QString blue = KdenliveTests::createProducer(pCore->getProjectProfile(), "blue", binModel, 100, true);
    int first = -1, second = -1;
    REQUIRE(timeline->requestClipInsertion(red, track, 0, first));
    REQUIRE(timeline->requestItemResize(first, 30, true, true));
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/5/34").arg(blue), track, 30, second));
    REQUIRE(timeline->requestSetSelection({first, second}));
    REQUIRE(timeline->studioTransitionSelection(10).status == TimelineModel::StudioTransitionStatus::Ready);
    const QVector<QPair<QString, QVariant>> parameters{{QStringLiteral("0"), QStringLiteral("0=0;9=1")},
                                                          {QStringLiteral("1"), 0}};
    REQUIRE(timeline->mixClip(first, QStringLiteral("studio_transition"), 1, 10, parameters));
    REQUIRE(timeline->getMixDuration(second) == 10);
    REQUIRE(timeline->getMixAlign(second) == MixAlignment::AlignCenter);
    const auto hashAt = [timeline](int position) {
        auto producer = timeline->producer();
        producer->seek(position);
        std::unique_ptr<Mlt::Frame> frame(producer->get_frame());
        REQUIRE(frame);
        auto format = mlt_image_rgba;
        int width = 320, height = 180;
        const auto pixels = frame->get_image(format, width, height);
        REQUIRE(pixels);
        return QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(pixels), width * height * 4), QCryptographicHash::Sha256);
    };
    const QByteArray before = hashAt(24), after = hashAt(35);
    REQUIRE(before != after);
    for (int style = 0; style < 15; ++style) {
        const double code = style < 12 ? double(style) / 11 : double(2 * (style - 12) + 1) / 22;
        REQUIRE(timeline->updateStudioTransition(10, {{QStringLiteral("1"), code}}));
        bool mixed = false;
        for (int position = 25; position < 35; ++position) {
            const QByteArray frame = hashAt(position);
            mixed |= frame != before && frame != after;
        }
        INFO(style);
        REQUIRE(mixed);
        REQUIRE(timeline->getMixDuration(second) == 10);
    }
}

TEST_CASE("Studio transition sound follows the panel mix and export", "[Studio]")
{
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString dataRoot = folder.filePath(QStringLiteral("share"));
    REQUIRE(QDir().mkpath(dataRoot + QStringLiteral("/kdenlive/studio/sfx/v1")));
    const QString soundPath = dataRoot + QStringLiteral("/kdenlive/studio/sfx/v1/transition_00.wav");
    QFile soundFile(soundPath);
    REQUIRE(soundFile.open(QIODevice::WriteOnly));
    QDataStream wave(&soundFile);
    wave.setByteOrder(QDataStream::LittleEndian);
    wave.writeRawData("RIFF", 4); wave << quint32(36 + 48000 * 2);
    wave.writeRawData("WAVEfmt ", 8);
    wave << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000) << quint16(2) << quint16(16);
    wave.writeRawData("data", 4); wave << quint32(48000 * 2);
    for (int i = 0; i < 48000; ++i) wave << qint16(i % 48 < 24 ? 4000 : -4000);
    soundFile.close();
    const QByteArray previousData = qgetenv("STUDIO_DATA_DIR");
    qputenv("STUDIO_DATA_DIR", QFile::encodeName(dataRoot));
    const auto restoreData = qScopeGuard([previousData] {
        if (previousData.isNull()) qunsetenv("STUDIO_DATA_DIR");
        else qputenv("STUDIO_DATA_DIR", previousData);
    });

    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const int track = timeline->getTrackIndexFromPosition(2);
    const QString red = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel, 100, true);
    const QString blue = KdenliveTests::createProducer(pCore->getProjectProfile(), "blue", binModel, 100, true);
    int first = -1, second = -1;
    REQUIRE(timeline->requestClipInsertion(red, track, 0, first));
    REQUIRE(timeline->requestItemResize(first, 30, true, true));
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/20/49").arg(blue), track, 30, second));
    REQUIRE(timeline->requestSetSelection({first, second}));
    const int before = undoStack->index();

    const QString project = folder.filePath(QStringLiteral("transition-sound.kdenlive"));
    auto panel = std::make_unique<StudioPanel>();
    panel->resize(440, 700);
    panel->show();
    QToolButton *transitions = nullptr;
    for (auto button : panel->findChildren<QToolButton *>())
        if (button->property("studioNav").toBool() && button->text() == QStringLiteral("Переходы")) transitions = button;
    REQUIRE(transitions);
    transitions->click();
    QApplication::processEvents();
    QCheckBox *sound = nullptr;
    for (auto check : panel->findChildren<QCheckBox *>())
        if (check->accessibleName() == QStringLiteral("Звук перехода")) sound = check;
    REQUIRE(sound);
    sound->setChecked(true);
    QPushButton *add = nullptr;
    for (auto button : panel->findChildren<QPushButton *>())
        if (button->property("studioPrimary").toBool()) add = button;
    REQUIRE(add);
    REQUIRE(add->isEnabled());
    add->click();
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 10000 && !timeline->studioTransitionModel(second))
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    auto mix = timeline->studioTransitionModel(second);
    REQUIRE(mix);
    REQUIRE(mix->getParam(QStringLiteral("studio:sfx_enabled")).toInt() == 1);
    REQUIRE_FALSE(mix->getParam(QStringLiteral("studio:sfx_id")).isEmpty());
    int soundTrack = -1;
    for (int id : timeline->getTracksIds(true))
        if (timeline->getTrackProperty(id, QStringLiteral("kdenlive:studio_transition_sfx_track")).toInt() == 1) soundTrack = id;
    REQUIRE(soundTrack >= 0);
    const auto soundItems = timeline->getItemsInRange(soundTrack, 0, -1, false);
    REQUIRE(soundItems.size() == 1);
    REQUIRE(timeline->isClip(*soundItems.begin()));
    REQUIRE(undoStack->index() == before + 1);
    undoStack->undo();
    REQUIRE_FALSE(timeline->studioTransitionModel(second));
    REQUIRE_FALSE((timeline->isTrack(soundTrack) && !timeline->getItemsInRange(soundTrack, 0, -1, false).empty()));
    undoStack->redo();
    REQUIRE(timeline->studioTransitionModel(second));
    REQUIRE(timeline->getItemsInRange(soundTrack, 0, -1, false).size() == 1);

    REQUIRE(pCore->projectManager()->testSaveFileAs(project));
    const QString exported = folder.filePath(QStringLiteral("transition-sound.mkv"));
    QProcess render;
    render.start(QStringLiteral("melt"), {project, QStringLiteral("-consumer"), QStringLiteral("avformat:") + exported,
                                         QStringLiteral("vcodec=ffv1"), QStringLiteral("acodec=pcm_s16le"), QStringLiteral("real_time=-1")});
    REQUIRE(render.waitForFinished(120000));
    INFO(render.readAllStandardError().toStdString());
    REQUIRE(render.exitCode() == 0);
    QProcess probe;
    probe.start(QStringLiteral("ffprobe"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                                            QStringLiteral("stream=codec_type"), QStringLiteral("-of"), QStringLiteral("csv=p=0"), exported});
    REQUIRE(probe.waitForFinished(30000));
    REQUIRE(probe.exitCode() == 0);
    REQUIRE(probe.readAllStandardOutput().contains("audio"));
    panel.reset();
    undoStack->clear();
    timeline.reset();
    pCore->projectManager()->closeCurrentDocument(false, false);
    QUndoGroup reopenedUndo;
    auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(project), folder.path(), &reopenedUndo, false, nullptr);
    REQUIRE(opened.isSuccessful());
    auto reopened = opened.getDocument();
    pCore->projectManager()->testSetDocument(reopened.get());
    const auto closeReopened = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(project).lastModified(), 0);
    const auto sequenceId = pCore->projectItemModel()->getAllSequenceClips().value(reopened->uuid());
    pCore->projectManager()->openTimeline(sequenceId, -1, reopened->uuid());
    auto reopenedTimeline = reopened->getTimeline(reopened->uuid());
    pCore->projectManager()->testSetActiveTimeline(reopenedTimeline);
    const int reopenedSecond = reopenedTimeline->getClipByPosition(reopenedTimeline->getTrackIndexFromPosition(2), 40);
    REQUIRE(reopenedSecond >= 0);
    REQUIRE(reopenedTimeline->studioTransitionModel(reopenedSecond));
    int reopenedSoundTrack = -1;
    for (int id : reopenedTimeline->getTracksIds(true))
        if (reopenedTimeline->getTrackProperty(id, QStringLiteral("kdenlive:studio_transition_sfx_track")).toInt() == 1)
            reopenedSoundTrack = id;
    REQUIRE(reopenedSoundTrack >= 0);
    REQUIRE(reopenedTimeline->getItemsInRange(reopenedSoundTrack, 0, -1, false).size() == 1);
    reopenedTimeline.reset();
    pCore->projectManager()->closeCurrentDocument(false, false);
}

TEST_CASE("Studio exports a two-sided transition and audible seam on decoded AV files", "[StudioAVTransition][.external]")
{
    const QString firstPath = qEnvironmentVariable("STUDIO_QA_TRANSITION_A");
    const QString secondPath = qEnvironmentVariable("STUDIO_QA_TRANSITION_B");
    INFO("Set STUDIO_QA_TRANSITION_A and STUDIO_QA_TRANSITION_B to readable AV MP4 files; this external test must not silently skip.");
    REQUIRE(QFileInfo(firstPath).isFile());
    REQUIRE(QFileInfo(secondPath).isFile());
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    auto bin = pCore->projectItemModel();
    bin->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const auto addMedia = [&](const QString &path) {
        const QByteArray encoded = QFile::encodeName(path);
        auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), encoded.constData());
        REQUIRE(producer->is_valid());
        REQUIRE(producer->get_length() >= 60);
        QString id = QString::number(bin->getFreeClipId());
        Fun undo = [] { return true; }, redo = [] { return true; };
        REQUIRE(bin->requestAddBinClip(id, producer, bin->getRootFolder()->clipId(), undo, redo));
        const auto clip = bin->getClipByBinID(id);
        REQUIRE(clip);
        REQUIRE(clip->hasVideo());
        REQUIRE(clip->hasAudio());
        return id;
    };
    const QString firstBin = addMedia(firstPath), secondBin = addMedia(secondPath);
    const int track = timeline->getTrackIndexFromPosition(2);
    KdenliveTests::setAudioTargets(timeline, {{1, QStringLiteral("stream1")}});
    int first = -1, second = -1;
    REQUIRE(timeline->requestClipInsertion(firstBin, track, 0, first));
    REQUIRE(timeline->requestItemResize(first, 30, true, true) == 30);
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/20/49").arg(secondBin), track, 30, second));
    REQUIRE(timeline->requestSetSelection({first, second}));
    const auto selected = timeline->studioTransitionSelection(10);
    REQUIRE(selected.status == TimelineModel::StudioTransitionStatus::Ready);
    REQUIRE(selected.firstAvailable >= 5);
    REQUIRE(selected.secondAvailable >= 5);
    REQUIRE(selected.audioMix);

    const auto frameAt = [](const std::shared_ptr<TimelineItemModel> &model, int position) {
        auto producer = model->producer();
        producer->seek(position);
        std::unique_ptr<Mlt::Frame> frame(producer->get_frame());
        REQUIRE(frame);
        auto format = mlt_image_rgba;
        int width = 320, height = 180;
        const auto pixels = frame->get_image(format, width, height);
        REQUIRE(pixels);
        REQUIRE(width == 320);
        REQUIRE(height == 180);
        return QByteArray(reinterpret_cast<const char *>(pixels), width * height * 4);
    };
    const auto distance = [](const QByteArray &a, const QByteArray &b) {
        REQUIRE(a.size() == b.size());
        double total = 0;
        for (qsizetype i = 0; i < a.size(); i += 4)
            for (int channel = 0; channel < 3; ++channel)
                total += std::abs(int(quint8(a[i + channel])) - int(quint8(b[i + channel])));
        return total / (a.size() / 4 * 3);
    };
    const QByteArray plain24 = frameAt(timeline, 24), plain29 = frameAt(timeline, 29);
    const QByteArray plain30 = frameAt(timeline, 30), plain35 = frameAt(timeline, 35);
    const double separation = distance(plain29, plain30);
    INFO("source separation=" << separation);
    REQUIRE(separation > 12);

    StudioPanel panel;
    panel.resize(440, 700);
    panel.show();
    QToolButton *transitions = nullptr, *style = nullptr;
    for (auto button : panel.findChildren<QToolButton *>())
        if (button->property("studioNav").toBool() && button->text() == QStringLiteral("Переходы")) transitions = button;
    REQUIRE(transitions);
    transitions->click();
    QApplication::processEvents();
    for (auto button : panel.findChildren<QToolButton *>())
        if (button->property("studioBaseText").toString() == QStringLiteral("Через расфокус")) style = button;
    REQUIRE(style);
    style->click();
    QDoubleSpinBox *duration = nullptr, *dip = nullptr;
    for (auto spin : panel.findChildren<QDoubleSpinBox *>()) {
        if (spin->accessibleName() == QStringLiteral("Длительность")) duration = spin;
        if (spin->accessibleName() == QStringLiteral("Приглушение звука на стыке")) dip = spin;
    }
    REQUIRE(duration);
    REQUIRE(dip);
    duration->setValue(10.0 / pCore->getCurrentFps());
    dip->setValue(0);
    QPushButton *add = nullptr;
    for (auto button : panel.findChildren<QPushButton *>())
        if (button->property("studioPrimary").toBool()) add = button;
    REQUIRE(add);
    REQUIRE(add->isEnabled());
    add->click();
    REQUIRE(timeline->studioTransitionModel(second));
    REQUIRE(timeline->getMixDuration(second) == 10);
    REQUIRE(timeline->getMixAlign(second) == MixAlignment::AlignCenter);
    const int secondAudio = timeline->getClipSplitPartner(second);
    REQUIRE(secondAudio >= 0);
    REQUIRE(timeline->getMixDuration(secondAudio) == 10);
    const int mixStart = timeline->getItemPosition(second);
    REQUIRE(mixStart == 25);
    const QByteArray mixed24 = frameAt(timeline, 24), mixed29 = frameAt(timeline, 29);
    const QByteArray mixed30 = frameAt(timeline, 30), mixed35 = frameAt(timeline, 35);
    REQUIRE(distance(mixed24, plain24) < 3);
    REQUIRE(distance(mixed35, plain35) < 3);
    for (const auto &middle : {mixed29, mixed30}) {
        INFO("distance to A=" << distance(middle, plain29) << " distance to B=" << distance(middle, plain30));
        REQUIRE(distance(middle, plain29) > separation * .12);
        REQUIRE(distance(middle, plain30) > separation * .12);
        REQUIRE(distance(middle, plain29) < separation * .95);
        REQUIRE(distance(middle, plain30) < separation * .95);
    }

    const int lastFrame = timeline->duration() - 1;
    const auto render = [&](const QString &name) {
        const QString project = folder.filePath(name + QStringLiteral(".kdenlive"));
        const QString output = folder.filePath(name + QStringLiteral(".mkv"));
        REQUIRE(pCore->projectManager()->testSaveFileAs(project));
        QProcess process;
        QStringList arguments{project, QStringLiteral("in=0"), QStringLiteral("out=%1").arg(lastFrame),
                              QStringLiteral("-consumer"), QStringLiteral("avformat:") + output,
                              QStringLiteral("vcodec=ffv1"), QStringLiteral("acodec=pcm_s16le"),
                              QStringLiteral("s=640x360"), QStringLiteral("real_time=-1")};
        process.start(QStringLiteral("melt"), arguments);
        REQUIRE(process.waitForFinished(120000));
        INFO(process.readAllStandardError().toStdString());
        REQUIRE(process.exitStatus() == QProcess::NormalExit);
        REQUIRE(process.exitCode() == 0);
        REQUIRE(QFileInfo(output).size() > 1024);
        return output;
    };
    const int firstAudio = timeline->getClipSplitPartner(first);
    REQUIRE(firstAudio >= 0);
    const auto userStack = timeline->getClipEffectStack(firstAudio);
    REQUIRE(userStack);
    Fun volumeUndo = [] { return true; }, volumeRedo = [] { return true; };
    REQUIRE(userStack->appendEffectWithUndo(QStringLiteral("volume"), volumeUndo, volumeRedo).first);
    const auto userVolume = effectsById(userStack, QStringLiteral("volume"));
    REQUIRE(userVolume.size() == 1);
    appendParameterChange(userVolume.front(), {QStringLiteral("level")}, {QStringLiteral("-6")}, volumeUndo, volumeRedo);
    const QString plainAudio = render(QStringLiteral("av-plain"));
    dip->setValue(75);
    REQUIRE(timeline->studioTransitionModel(second)->getParam(QStringLiteral("studio:audio_dip")).toDouble() == Approx(.75));
    const QString dippedAudio = render(QStringLiteral("av-dipped"));
    QCheckBox *sound = nullptr;
    for (auto check : panel.findChildren<QCheckBox *>())
        if (check->accessibleName() == QStringLiteral("Звук перехода")) sound = check;
    REQUIRE(sound);
    sound->setChecked(true);
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 10000 && timeline->studioTransitionModel(second)->getParam(QStringLiteral("studio:sfx_enabled")).toInt() != 1)
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    REQUIRE(timeline->studioTransitionModel(second)->getParam(QStringLiteral("studio:sfx_enabled")).toInt() == 1);
    int managedSoundTrack = -1;
    for (int id : timeline->getTracksIds(true))
        if (timeline->getTrackProperty(id, QStringLiteral("kdenlive:studio_transition_sfx_track")).toInt() == 1) managedSoundTrack = id;
    REQUIRE(managedSoundTrack >= 0);
    const auto soundItems = timeline->getItemsInRange(managedSoundTrack, 0, -1, false);
    REQUIRE(soundItems.size() == 1);
    const int oldSound = *soundItems.begin();
    const QString soundIdentity = timeline->studioTransitionModel(second)->getParam(QStringLiteral("studio:sfx_id"));
    REQUIRE_FALSE(soundIdentity.isEmpty());
    const std::unordered_set<int> sourceClips{first, second, firstAudio, secondAudio};
    const auto sourceState = [&] {
        QMap<int, QStringList> state;
        for (int id : sourceClips) {
            REQUIRE(timeline->isClip(id));
            state.insert(id, {timeline->getClipBinId(id), QString::number(timeline->getClipIn(id)),
                QString::number(timeline->getItemPlaytime(id)), QString::number(timeline->getItemPosition(id)),
                QString::number(timeline->getItemTrackId(id)), QString::number(timeline->getClipSplitPartner(id))});
        }
        return state;
    };
    const auto originalSources = sourceState();
    const QString originalStyle = timeline->studioTransitionModel(second)->getParam(QStringLiteral("1"));
    REQUIRE(timeline->requestSetSelection({first, second, firstAudio, secondAudio, oldSound}));
    REQUIRE(timeline->getCurrentSelection().size() == 5);
    QDoubleSpinBox *soundLevel = nullptr;
    for (auto spin : panel.findChildren<QDoubleSpinBox *>())
        if (spin->accessibleName() == QStringLiteral("Громкость звука")) soundLevel = spin;
    REQUIRE(soundLevel);
    REQUIRE(soundLevel->value() == Approx(50));
    const int beforeRepeat = undoStack->index();
    soundLevel->setValue(40);
    REQUIRE(undoStack->index() == beforeRepeat + 1);
    const auto checkReplacement = [&](double level) {
        const auto currentMix = timeline->studioTransitionModel(second);
        REQUIRE(currentMix);
        REQUIRE(currentMix->getParam(QStringLiteral("studio:sfx_level")).toDouble() == Approx(level));
        REQUIRE(currentMix->getParam(QStringLiteral("studio:sfx_id")) == soundIdentity);
        REQUIRE(currentMix->getParam(QStringLiteral("1")) == originalStyle);
        REQUIRE(currentMix->getParam(QStringLiteral("studio:audio_dip")).toDouble() == Approx(.75));
        REQUIRE(timeline->getMixDuration(second) == 10);
        REQUIRE(timeline->getMixDuration(secondAudio) == 10);
        REQUIRE(sourceState() == originalSources);
        REQUIRE(effectsById(timeline->getClipEffectStack(firstAudio), QStringLiteral("volume")).size() == 1);
        REQUIRE(userVolume.front()->getParam(QStringLiteral("level")) == QStringLiteral("-6"));
        const auto items = timeline->getItemsInRange(managedSoundTrack, 0, -1, false);
        REQUIRE(items.size() == 1);
        const auto producer = timeline->getClipProducer(*items.begin());
        REQUIRE(producer);
        REQUIRE(QString::fromUtf8(producer->get("studio:sfx:id")) == soundIdentity);
        const auto volume = effectsById(timeline->getClipEffectStack(*items.begin()), QStringLiteral("volume"));
        REQUIRE(volume.size() == 1);
        REQUIRE(volume.front()->getParam(QStringLiteral("level")).toDouble() == Approx(20 * std::log10(level / .5)));
    };
    checkReplacement(.4);
    undoStack->undo();
    REQUIRE(undoStack->index() == beforeRepeat);
    REQUIRE(timeline->isClip(oldSound));
    checkReplacement(.5);
    undoStack->redo();
    REQUIRE(undoStack->index() == beforeRepeat + 1);
    checkReplacement(.4);
    panel.hide();
    const QString saved = folder.filePath(QStringLiteral("av-dipped-sfx.kdenlive"));
    REQUIRE(pCore->projectManager()->testSaveFileAs(saved));
    pCore->projectManager()->closeCurrentDocument(false, false);
    QUndoGroup reopenedUndo;
    auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(saved), folder.path(), &reopenedUndo, false, nullptr);
    REQUIRE(opened.isSuccessful());
    auto reopened = opened.getDocument();
    pCore->projectManager()->testSetDocument(reopened.get());
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(saved).lastModified(), 0));
    const auto sequence = pCore->projectItemModel()->getAllSequenceClips().value(reopened->uuid());
    pCore->projectManager()->openTimeline(sequence, -1, reopened->uuid());
    auto live = reopened->getTimeline(reopened->uuid());
    pCore->projectManager()->testSetActiveTimeline(live);
    const int reopenedSecond = live->getClipByPosition(live->getTrackIndexFromPosition(2), 40);
    REQUIRE(reopenedSecond >= 0);
    REQUIRE(live->studioTransitionModel(reopenedSecond));
    REQUIRE(live->getMixDuration(reopenedSecond) == 10);
    REQUIRE(live->studioTransitionModel(reopenedSecond)->getParam(QStringLiteral("studio:audio_dip")).toDouble() == Approx(.75));
    REQUIRE(live->studioTransitionModel(reopenedSecond)->getParam(QStringLiteral("studio:sfx_enabled")).toInt() == 1);
    int soundTrack = -1;
    for (int id : live->getTracksIds(true))
        if (live->getTrackProperty(id, QStringLiteral("kdenlive:studio_transition_sfx_track")).toInt() == 1) soundTrack = id;
    REQUIRE(soundTrack >= 0);
    REQUIRE(live->getItemsInRange(soundTrack, 0, -1, false).size() == 1);
    REQUIRE(distance(frameAt(live, 24), mixed24) < 3);
    REQUIRE(distance(frameAt(live, 29), mixed29) < 3);
    REQUIRE(distance(frameAt(live, 30), mixed30) < 3);
    REQUIRE(distance(frameAt(live, 35), mixed35) < 3);
    const QString finalVideo = render(QStringLiteral("av-dipped-sfx-export"));

    const auto decodeAudio = [](const QString &path) {
        QProcess decode;
        decode.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), path,
                          QStringLiteral("-map"), QStringLiteral("0:a:0"), QStringLiteral("-ac"), QStringLiteral("1"),
                          QStringLiteral("-ar"), QStringLiteral("48000"), QStringLiteral("-f"), QStringLiteral("s16le"), QStringLiteral("pipe:1")});
        REQUIRE(decode.waitForFinished(30000));
        INFO(decode.readAllStandardError().toStdString());
        REQUIRE(decode.exitCode() == 0);
        return decode.readAllStandardOutput();
    };
    const QByteArray pcmPlain = decodeAudio(plainAudio), pcmDipped = decodeAudio(dippedAudio), pcmSfx = decodeAudio(finalVideo);
    const int sampleStart = qRound((mixStart + 3) * 48000.0 / pCore->getCurrentFps());
    const int sampleEnd = qRound((mixStart + 7) * 48000.0 / pCore->getCurrentFps());
    for (const auto &pcm : {pcmPlain, pcmDipped, pcmSfx}) REQUIRE(pcm.size() >= sampleEnd * 2);
    const auto sample = [](const QByteArray &pcm, int index) {
        return qint16(quint16(quint8(pcm[index * 2])) | (quint16(quint8(pcm[index * 2 + 1])) << 8));
    };
    double unchangedBeforeMix = 0;
    const int preStart = qRound(8 * 48000.0 / pCore->getCurrentFps());
    const int preEnd = qRound(20 * 48000.0 / pCore->getCurrentFps());
    for (int i = preStart; i < preEnd; ++i)
        unchangedBeforeMix += std::abs(int(sample(pcmPlain, i)) - int(sample(pcmDipped, i)))
            + std::abs(int(sample(pcmDipped, i)) - int(sample(pcmSfx, i)));
    REQUIRE(unchangedBeforeMix / (2 * (preEnd - preStart)) < 2);
    double ordinary = 0, dipChange = 0, sfxChange = 0;
    for (int i = sampleStart; i < sampleEnd; ++i) {
        const int original = sample(pcmPlain, i), dipped = sample(pcmDipped, i), withSfx = sample(pcmSfx, i);
        ordinary += std::abs(original);
        dipChange += std::abs(dipped - original);
        sfxChange += std::abs(withSfx - dipped);
    }
    ordinary /= sampleEnd - sampleStart;
    dipChange /= sampleEnd - sampleStart;
    sfxChange /= sampleEnd - sampleStart;
    INFO("ordinary PCM=" << ordinary << " dip difference=" << dipChange << " SFX difference=" << sfxChange);
    REQUIRE(ordinary > 100);
    REQUIRE(dipChange > ordinary * .1);
    REQUIRE(sfxChange > 100);

    const auto decodeVideo = [](const QString &path) {
        QProcess decode;
        decode.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), path,
            QStringLiteral("-vf"), QStringLiteral("select=eq(n\\,24)+eq(n\\,29)+eq(n\\,30)+eq(n\\,35),scale=320:180"),
            QStringLiteral("-fps_mode"), QStringLiteral("passthrough"), QStringLiteral("-f"), QStringLiteral("rawvideo"),
            QStringLiteral("-pix_fmt"), QStringLiteral("rgba"), QStringLiteral("pipe:1")});
        REQUIRE(decode.waitForFinished(30000));
        INFO(decode.readAllStandardError().toStdString());
        REQUIRE(decode.exitCode() == 0);
        return decode.readAllStandardOutput();
    };
    const QByteArray frames = decodeVideo(finalVideo);
    const QByteArray unchangedVideo = decodeVideo(plainAudio);
    const int frameBytes = 320 * 180 * 4;
    REQUIRE(frames.size() == 4 * frameBytes);
    REQUIRE(unchangedVideo.size() == frames.size());
    for (int i = 0; i < 4; ++i)
        REQUIRE(distance(frames.mid(i * frameBytes, frameBytes), unchangedVideo.mid(i * frameBytes, frameBytes)) < 3);
    pCore->projectManager()->closeCurrentDocument(false, false);
}

TEST_CASE("Studio camera export preserves head track paths through XML serialization", "[Studio]")
{
    QTemporaryDir files;
    REQUIRE(files.isValid());
    files.setAutoRemove(false);
    qInfo().noquote() << "Camera input evidence:" << files.path();
    ProfileParam cameraProfile(320, 180, 25, 1, 16, 9, 1, 1, 709, false);
    const QString profile = ProfileRepository::get()->saveProfile(&cameraProfile);
    REQUIRE_FALSE(profile.isEmpty());
    const QString previousProfile = pCore->getCurrentProfilePath();
    const auto restoreProfile = qScopeGuard([previousProfile, profile] {
        pCore->setCurrentProfile(previousProfile.isEmpty() ? QStringLiteral("dv_pal") : previousProfile);
        ProfileRepository::get()->deleteProfile(profile);
    });
    REQUIRE(pCore->setCurrentProfile(profile));
    QImage pattern(320, 180, QImage::Format_RGBA8888);
    for (int y = 0; y < pattern.height(); ++y)
        for (int x = 0; x < pattern.width(); ++x)
            pattern.setPixelColor(x, y, QColor(x * 255 / 319, y * 255 / 179, ((x / 20 + y / 20) % 2) * 255));
    const QString image = files.filePath(QStringLiteral("camera-grid.png"));
    REQUIRE(pattern.save(image));
    pCore->projectItemModel()->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const int track = timeline->getTrackIndexFromPosition(2);
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), "qimage", QFile::encodeName(image).constData());
    REQUIRE(producer->is_valid());
    const auto bin = pCore->projectItemModel();
    QString binId = QString::number(bin->getFreeClipId());
    Fun binUndo = [] { return true; }, binRedo = [] { return true; };
    REQUIRE(bin->requestAddBinClip(binId, producer, bin->getRootFolder()->clipId(), binUndo, binRedo));
    int clipId = -1;
    REQUIRE(timeline->requestClipInsertion(binId, track, 0, clipId));
    REQUIRE(timeline->requestItemResize(clipId, 51, true, true) == 51);
    REQUIRE(timeline->duration() == 51);
    auto stack = timeline->getClipEffectStack(clipId);
    REQUIRE(stack->appendEffect(QStringLiteral("studio_camera")));
    auto camera = effectsById(stack, QStringLiteral("studio_camera")).front();
    camera->setParameter(QStringLiteral("mode"), QStringLiteral("0"), true);
    camera->setParameter(QStringLiteral("live"), QStringLiteral("0"), true);
    camera->setParameter(QStringLiteral("zoom"), QStringLiteral("160"), true);
    camera->setParameter(QStringLiteral("tracking"), QStringLiteral("1"), true);
    QFile headTrack(files.filePath(QStringLiteral("голова тест.scam")));
    REQUIRE(headTrack.open(QIODevice::WriteOnly));
    const QByteArray trackBytes = "SUNIMO_CAMERA_TRACK_V1\n# aspect=1.777777777778\n0,0.25,0.35,1\n1,0.5,0.5,1\n2,0.75,0.65,1\n";
    REQUIRE(headTrack.write(trackBytes) == trackBytes.size());
    headTrack.close();
    camera->setParameter(QStringLiteral("track_path"), headTrack.fileName(), true);
    QByteArray reference;
    for (int position : {0, 25, 50}) reference += studioFrameAt(timeline, position);
    const auto distance = [](const QByteArray &a, const QByteArray &b) {
        REQUIRE(a.size() == b.size());
        REQUIRE_FALSE(a.isEmpty());
        double total = 0;
        for (int i = 0; i < a.size(); ++i) total += qAbs(int(uchar(a[i])) - int(uchar(b[i])));
        return total / a.size();
    };
    constexpr int frameBytes = 320 * 180 * 4;
    REQUIRE(reference.size() == 3 * frameBytes);
    camera->setParameter(QStringLiteral("tracking"), QStringLiteral("0"), true);
    REQUIRE(distance(reference.left(frameBytes), studioFrameAt(timeline, 0)) > 5);
    REQUIRE(distance(reference.right(frameBytes), studioFrameAt(timeline, 50)) > 5);
    camera->setParameter(QStringLiteral("tracking"), QStringLiteral("1"), true);
    QString ordinaryPlaylist;
    for (const QString &path : {headTrack.fileName(), QUrl::fromLocalFile(headTrack.fileName()).toString()}) {
        camera->setParameter(QStringLiteral("track_path"), path, true);
        for (const QString &aspect : {QString(), QStringLiteral("vertical")}) {
            const auto scene = pCore->projectItemModel()->sceneList(files.path(), QString(), timeline->tractor(), timeline->duration(), true, aspect);
            QString playlist = scene.first;
            if (!scene.second.isEmpty()) {
                QFile actualPlaylist(scene.second);
                REQUIRE(actualPlaylist.open(QIODevice::ReadOnly));
                playlist = QString::fromUtf8(actualPlaylist.readAll());
                actualPlaylist.close();
                REQUIRE(QFile::remove(scene.second));
            }
            QDomDocument xml;
            REQUIRE(xml.setContent(playlist));
            int matches = 0;
            const auto filters = xml.elementsByTagName(QStringLiteral("filter"));
            for (int i = 0; i < filters.count(); ++i) {
                const auto filter = filters.at(i).toElement();
                if (Xml::getXmlProperty(filter, QStringLiteral("mlt_service")) != QLatin1String("studio.camera")) continue;
                const QString stored = Xml::getXmlProperty(filter, QStringLiteral("track_path"));
                const QUrl url(stored);
                REQUIRE((url.isLocalFile() ? url.toLocalFile() : stored) == headTrack.fileName());
                REQUIRE(Xml::getXmlProperty(filter, QStringLiteral("tracking")) == QLatin1String("1"));
                ++matches;
            }
            REQUIRE(matches > 0);
            if (ordinaryPlaylist.isEmpty() && aspect.isEmpty()) ordinaryPlaylist = playlist;
        }
    }
    REQUIRE_FALSE(ordinaryPlaylist.isEmpty());
    // The render worker runs outside the project root: a relative .scam silently disables tracking.
    QTemporaryDir worker;
    REQUIRE(worker.isValid());
    worker.setAutoRemove(false);
    qInfo().noquote() << "Camera export evidence:" << worker.path();
    REQUIRE(worker.path() != files.path());
    QFile scene(worker.filePath(QStringLiteral("camera-export.mlt")));
    REQUIRE(scene.open(QIODevice::WriteOnly));
    const QByteArray sceneBytes = ordinaryPlaylist.toUtf8();
    REQUIRE(scene.write(sceneBytes) == sceneBytes.size());
    scene.close();
    const QString video = worker.filePath(QStringLiteral("camera-export.mkv"));
    QProcess render;
    render.setWorkingDirectory(worker.path());
    render.start(QStringLiteral("melt"), {scene.fileName(), QStringLiteral("in=0"), QStringLiteral("out=50"),
                                          QStringLiteral("-consumer"), QStringLiteral("avformat:") + video,
                                          QStringLiteral("vcodec=ffv1"), QStringLiteral("pix_fmt=bgra"),
                                          QStringLiteral("an=1"), QStringLiteral("real_time=-1")});
    const bool rendered = render.waitForFinished(120000);
    if (!rendered) { render.kill(); render.waitForFinished(1000); }
    INFO(render.readAllStandardError().toStdString());
    REQUIRE(rendered);
    REQUIRE(render.exitStatus() == QProcess::NormalExit);
    REQUIRE(render.exitCode() == 0);
    REQUIRE(QFileInfo(video).size() > 0);
    QProcess decode;
    decode.setWorkingDirectory(worker.path());
    decode.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), video,
                                            QStringLiteral("-vf"), QStringLiteral("select=eq(n\\,0)+eq(n\\,25)+eq(n\\,50)"),
                                            QStringLiteral("-fps_mode"), QStringLiteral("passthrough"), QStringLiteral("-f"), QStringLiteral("rawvideo"),
                                            QStringLiteral("-pix_fmt"), QStringLiteral("rgba"), QStringLiteral("-")});
    const bool decoded = decode.waitForFinished(30000);
    if (!decoded) { decode.kill(); decode.waitForFinished(1000); }
    INFO(decode.readAllStandardError().toStdString());
    REQUIRE(decoded);
    REQUIRE(decode.exitStatus() == QProcess::NormalExit);
    REQUIRE(decode.exitCode() == 0);
    const QByteArray exported = decode.readAllStandardOutput();
    REQUIRE(exported.size() == 3 * frameBytes);
    for (int i = 0; i < 3; ++i) {
        INFO("Camera export anchor " << i * 25);
        const double difference = distance(exported.mid(i * frameBytes, frameBytes), reference.mid(i * frameBytes, frameBytes));
        qInfo() << "Camera export frame" << i * 25 << "mean RGBA difference:" << difference;
        REQUIRE(difference < 3);
    }
}

TEST_CASE("Studio effect chain retains time and matte identity after cut and undo", "[Studio]")
{
    pCore->projectItemModel()->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const int track = timeline->getTrackIndexFromPosition(2);
    const QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", pCore->projectItemModel(), 100, false);
    int clipId = -1;
    REQUIRE(timeline->requestClipInsertion(binId, track, 0, clipId));
    REQUIRE(timeline->requestItemResize(clipId, 30, true, true));
    auto stack = timeline->getClipEffectStack(clipId);
    for (const auto &id : {QStringLiteral("studio_background"), QStringLiteral("studio_color"), QStringLiteral("studio_camera"),
                           QStringLiteral("studiofx"), QStringLiteral("card3d")}) REQUIRE(stack->appendEffect(id));
    auto matte = effectsById(stack, QStringLiteral("studio_background")).front();
    QTemporaryDir tracks;
    QFile trackFile(tracks.filePath(QStringLiteral("голова тест.scam")));
    REQUIRE(trackFile.open(QIODevice::WriteOnly));
    trackFile.write("SUNIMO_CAMERA_TRACK_V1\n0,0.5,0.5,1\n");
    trackFile.close();
    QFile textScene(tracks.filePath(QStringLiteral("надпись тест.stxt")));
    const QJsonObject textSettings{{QStringLiteral("text"), QStringLiteral("Надпись тест")},
                                   {QStringLiteral("size"), 64},
                                   {QStringLiteral("duration"), 30.0 / pCore->getCurrentFps()}};
    const auto &profile = pCore->getProjectProfile();
    QString sceneError;
    const QByteArray sceneBytes = SunimoTextQt::compile(textSettings, {profile.width(), profile.height()});
    REQUIRE(SunimoTextQt::save(textScene.fileName(), sceneBytes, &sceneError));
    auto camera = effectsById(stack, QStringLiteral("studio_camera")).front();
    camera->setParameter(QStringLiteral("track_path"), trackFile.fileName(), true);
    REQUIRE(stack->externalFiles().contains(trackFile.fileName()));
    const auto frameWithoutText = frameHashes(timeline).at(1);
    REQUIRE(stack->appendEffect(QStringLiteral("sunimo_text_studio")));
    auto textEffect = effectsById(stack, QStringLiteral("sunimo_text_studio")).front();
    textEffect->setParameter(QStringLiteral("0"), textScene.fileName(), true);
    REQUIRE(stack->externalFiles().contains(textScene.fileName()));
    REQUIRE(frameHashes(timeline).at(1) != frameWithoutText);
    QDomDocument archivedScene;
    auto xml = archivedScene.createElement(QStringLiteral("mlt"));
    archivedScene.appendChild(xml);
    auto filter = archivedScene.createElement(QStringLiteral("filter"));
    xml.appendChild(filter);
    Xml::setXmlProperty(filter, QStringLiteral("mlt_service"), QStringLiteral("studio.camera"));
    Xml::setXmlProperty(filter, QStringLiteral("track_path"), trackFile.fileName());
    auto textFilter = archivedScene.createElement(QStringLiteral("filter"));
    xml.appendChild(textFilter);
    Xml::setXmlProperty(textFilter, QStringLiteral("mlt_service"), QStringLiteral("frei0r.sunimo_text_studio"));
    Xml::setXmlProperty(textFilter, QStringLiteral("0"), textScene.fileName());
    ArchiveWidget archive(QStringLiteral("project.kdenlive"), archivedScene.toString(), {}, {trackFile.fileName(), textScene.fileName()});
    const QString destination = tracks.filePath(QStringLiteral("перенос"));
    QDomDocument rewritten;
    REQUIRE(rewritten.setContent(StudioArchiveTests::rewrite(archive, archivedScene, destination)));
    REQUIRE(Xml::getXmlProperty(rewritten.elementsByTagName(QStringLiteral("filter")).at(0).toElement(), QStringLiteral("track_path"))
            == destination + QStringLiteral("/others/голова тест.scam"));
    REQUIRE(Xml::getXmlProperty(rewritten.elementsByTagName(QStringLiteral("filter")).at(1).toElement(), QStringLiteral("0"))
            == destination + QStringLiteral("/others/надпись тест.stxt"));
    matte->setParameter(QStringLiteral("mask_asset"), QStringLiteral("owned.sbg"), true);
    matte->setParameter(QStringLiteral("sample_offset"), QStringLiteral("7"), true);
    matte->filter().set("_sbg_mask_path", "/original.sbg");
    {
        auto child = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), "color", "red");
        REQUIRE(child->is_valid());
        const int initialFilters = child->filter_count();
        StudioHelpers::setBackgroundClipIn(matte, 1325);
        matte->plantClone(child);
        REQUIRE(child->filter_count() == initialFilters + 1);
        auto clone = std::unique_ptr<Mlt::Filter>(child->filter(initialFilters));
        REQUIRE(clone->get_int("_sbg_clip_in") == 1325);
        REQUIRE(QString::fromUtf8(clone->get("_sbg_mask_path")) == QStringLiteral("/original.sbg"));
        StudioHelpers::setBackgroundClipIn(matte, 1330);
        REQUIRE(clone->get_int("_sbg_clip_in") == 1330);
        StudioHelpers::setBackgroundClipIn(matte, 0);
        REQUIRE(clone->get_int("_sbg_clip_in") == 0);
        matte->unplantClone(child);
    }
    int maskUpdates = 0;
    QObject::connect(matte.get(), &AssetParameterModel::updateChildren, matte.get(), [&maskUpdates](const QStringList &names) {
        if (names.contains(QStringLiteral("_sbg_mask_path"))) ++maskUpdates;
    });
    Fun undoMask = [] { return true; }, redoMask = [] { return true; };
    StudioHelpers::appendMaskPathChange(matte, QStringLiteral("/replacement.sbg"), undoMask, redoMask);
    REQUIRE(QString::fromUtf8(matte->filter().get("_sbg_mask_path")) == QStringLiteral("/replacement.sbg"));
    REQUIRE(maskUpdates == 1);
    REQUIRE(undoMask());
    REQUIRE(QString::fromUtf8(matte->filter().get("_sbg_mask_path")) == QStringLiteral("/original.sbg"));
    REQUIRE(maskUpdates == 2);
    REQUIRE(redoMask());
    REQUIRE(QString::fromUtf8(matte->filter().get("_sbg_mask_path")) == QStringLiteral("/replacement.sbg"));
    REQUIRE(maskUpdates == 3);
    auto card = effectsById(stack, QStringLiteral("card3d")).front();
    const double fps = pCore->getCurrentFps();
    card->setParameter(QStringLiteral("17"), QString::number(29.0 / fps / 21600.0, 'g', 17), true);
    const int before = undoStack->index();
    REQUIRE(TimelineFunctions::requestClipCut(timeline, clipId, 15));
    REQUIRE(undoStack->index() == before + 1);
    const int rightId = timeline->getClipByPosition(track, 15);
    REQUIRE(rightId != clipId);
    auto right = timeline->getClipEffectStack(rightId);
    REQUIRE(effectsById(right, QStringLiteral("studio_camera")).front()->getParam(QStringLiteral("studio_time_origin")).toInt() == 15);
    REQUIRE(effectsById(right, QStringLiteral("studio_camera")).front()->getParam(QStringLiteral("studio_time_span")).toInt() == 29);
    REQUIRE(effectsById(right, QStringLiteral("studiofx")).front()->getParam(QStringLiteral("10")).toDouble() * 86400.0 == Approx(15.0 / fps));
    REQUIRE(effectsById(right, QStringLiteral("studio_background")).front()->getParam(QStringLiteral("mask_asset")) == QStringLiteral("owned.sbg"));
    REQUIRE(effectsById(right, QStringLiteral("studio_background")).front()->getParam(QStringLiteral("sample_offset")).toInt() == 22);
    REQUIRE(card->getParam(QStringLiteral("17")).toDouble() * 21600.0 == Approx(14.0 / fps));
    undoStack->undo();
    REQUIRE(timeline->getClipPlaytime(clipId) == 30);
    REQUIRE(card->getParam(QStringLiteral("17")).toDouble() * 21600.0 == Approx(29.0 / fps));
    REQUIRE(matte->getParam(QStringLiteral("sample_offset")).toInt() == 7);
    undoStack->redo();
    REQUIRE(timeline->getClipPlaytime(clipId) == 15);
    const auto audioTracks = timeline->getTracksIds(true);
    REQUIRE_FALSE(audioTracks.isEmpty());
    timeline->setTrackProperty(audioTracks.front(), QStringLiteral("kdenlive:studio_audio_track"), QStringLiteral("1"));
    const auto originalFrames = frameHashes(timeline);
    const QString saved = tracks.filePath(QStringLiteral("цепочка эффектов.kdenlive"));
    REQUIRE(pCore->projectManager()->testSaveFileAs(saved));
    const QString savedTextScene = QDir(QFileInfo(saved).absolutePath()).filePath(QStringLiteral("studio-text/надпись тест.stxt"));
    QFile savedFile(saved);
    REQUIRE(savedFile.open(QIODevice::ReadOnly));
    const QByteArray savedProject = savedFile.readAll();
    REQUIRE(savedProject.contains("kdenlive:studio_audio_track"));
    REQUIRE_FALSE(savedProject.contains("_sbg_mask_path"));
    QDomDocument savedXml;
    REQUIRE(savedXml.setContent(savedProject));
    bool foundTextScene = false;
    const auto savedFilters = savedXml.elementsByTagName(QStringLiteral("filter"));
    for (int i = 0; i < savedFilters.count(); ++i) {
        const auto savedFilter = savedFilters.at(i).toElement();
        if (Xml::getXmlProperty(savedFilter, QStringLiteral("mlt_service")) != QLatin1String("frei0r.sunimo_text_studio")) continue;
        REQUIRE(Xml::getXmlProperty(savedFilter, QStringLiteral("0")) == savedTextScene);
        foundTextScene = true;
    }
    REQUIRE(foundTextScene);
    QFile savedScene(savedTextScene);
    REQUIRE(savedScene.open(QIODevice::ReadOnly));
    REQUIRE(QCryptographicHash::hash(savedScene.readAll(), QCryptographicHash::Sha256)
            == QCryptographicHash::hash(sceneBytes, QCryptographicHash::Sha256));
    // Mirror the live remap explicitly: testSaveFileAs only serializes the headless model.
    document.updateStudioEffectAssetPaths({{textScene.fileName(), savedTextScene}});
    REQUIRE(textEffect->getParam(QStringLiteral("0")) == savedTextScene);
    document.setUrl(QUrl::fromLocalFile(saved));
    REQUIRE(QFile::remove(textScene.fileName()));
    const QString archiveDir = tracks.filePath(QStringLiteral("архив проекта"));
    REQUIRE(QDir().mkpath(archiveDir));
    const QStringList linkedFiles = document.extractExternalEffectFiles();
    REQUIRE(linkedFiles.contains(trackFile.fileName()));
    REQUIRE(linkedFiles.contains(savedTextScene));
    ArchiveWidget fullArchive(QFileInfo(saved).fileName(), QString::fromUtf8(savedProject), {}, linkedFiles);
    const QString archivedTrack = archiveDir + QStringLiteral("/others/голова тест.scam");
    const QString archivedTextScene = archiveDir + QStringLiteral("/others/надпись тест.stxt");
    const QString archivedProject = archiveDir + QStringLiteral("/цепочка эффектов.kdenlive");
    REQUIRE(StudioArchiveTests::start(fullArchive, archiveDir));
    QEventLoop archiveLoop;
    QTimer archivePoll;
    QObject::connect(&archivePoll, &QTimer::timeout, &archiveLoop, [&] {
        if (QFileInfo::exists(archivedTrack) && QFileInfo::exists(archivedTextScene) && QFileInfo::exists(archivedProject)) archiveLoop.quit();
    });
    archivePoll.start(25);
    QTimer::singleShot(10000, &archiveLoop, &QEventLoop::quit);
    archiveLoop.exec();
    QFile archivedTrackFile(archivedTrack);
    REQUIRE(archivedTrackFile.open(QIODevice::ReadOnly));
    REQUIRE(archivedTrackFile.readAll() == QByteArray("SUNIMO_CAMERA_TRACK_V1\n0,0.5,0.5,1\n"));
    QFile archivedTextFile(archivedTextScene);
    REQUIRE(archivedTextFile.open(QIODevice::ReadOnly));
    REQUIRE(archivedTextFile.readAll() == sceneBytes);
    QFile archivedProjectFile(archivedProject);
    REQUIRE(archivedProjectFile.open(QIODevice::ReadOnly));
    const QByteArray archivedProjectBytes = archivedProjectFile.readAll();
    REQUIRE(archivedProjectBytes.contains("others/голова тест.scam"));
    REQUIRE(archivedProjectBytes.contains("others/надпись тест.stxt"));
    pCore->projectManager()->closeCurrentDocument(false, false);
    QUndoGroup undoGroup;
    auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(saved), tracks.path(), &undoGroup, false, nullptr);
    REQUIRE(opened.isSuccessful());
    auto reopened = opened.getDocument();
    pCore->projectManager()->testSetDocument(reopened.get());
    KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(saved).lastModified(), 0);
    const auto sequenceId = pCore->projectItemModel()->getAllSequenceClips().value(reopened->uuid());
    pCore->projectManager()->openTimeline(sequenceId, -1, reopened->uuid());
    auto reopenedTimeline = reopened->getTimeline(reopened->uuid());
    pCore->projectManager()->testSetActiveTimeline(reopenedTimeline);
    REQUIRE(frameHashes(reopenedTimeline) == originalFrames);
    int managedTrackCount = 0;
    for (int id : reopenedTimeline->getTracksIds(true))
        managedTrackCount += reopenedTimeline->getTrackProperty(id, QStringLiteral("kdenlive:studio_audio_track")).toInt() == 1;
    REQUIRE(managedTrackCount == 1); // reload must not cause a second managed track
    QString trackError;
    Fun trackUndo = [] { return true; }, trackRedo = [] { return true; };
    const int beforeTracks = reopenedTimeline->getTracksCount();
    const int managedTrack = StudioManagedAudio::ensureTrack(reopenedTimeline, trackUndo, trackRedo, &trackError);
    REQUIRE(managedTrack >= 0);
    REQUIRE(StudioManagedAudio::ensureTrack(reopenedTimeline, trackUndo, trackRedo, &trackError) == managedTrack);
    REQUIRE(reopenedTimeline->getTracksCount() == beforeTracks);

    pCore->projectManager()->closeCurrentDocument(false, false);
    QUndoGroup archivedUndo;
    auto archivedOpened = KdenliveDoc::Open(QUrl::fromLocalFile(archivedProject), archiveDir, &archivedUndo, false, nullptr);
    REQUIRE(archivedOpened.isSuccessful());
    auto archivedDoc = archivedOpened.getDocument();
    pCore->projectManager()->testSetDocument(archivedDoc.get());
    KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(archivedProject).lastModified(), 0);
    const auto archivedSequence = pCore->projectItemModel()->getAllSequenceClips().value(archivedDoc->uuid());
    pCore->projectManager()->openTimeline(archivedSequence, -1, archivedDoc->uuid());
    auto archivedTimeline = archivedDoc->getTimeline(archivedDoc->uuid());
    pCore->projectManager()->testSetActiveTimeline(archivedTimeline);
    REQUIRE(frameHashes(archivedTimeline) == originalFrames);
    pCore->projectManager()->closeCurrentDocument(false, false);
}

TEST_CASE("Studio mask save-as rejects failed copies and conflicting assets", "[Studio]")
{
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    const int previousLocation = KdenliveSettings::videotodefaultfolder();
    auto restoreSettings = qScopeGuard([previousLocation] { KdenliveSettings::setVideotodefaultfolder(previousLocation); });
    KdenliveSettings::setVideotodefaultfolder(KdenliveDoc::SaveToProjectFolder);
    document.m_sameProjectFolder = true;
    const QString sourceFolder = folder.filePath(QStringLiteral("исходник"));
    document.setProjectFolder(QUrl::fromLocalFile(sourceFolder));
    document.setUrl(QUrl::fromLocalFile(sourceFolder + QStringLiteral("/original.kdenlive")));
    const QString sourceMask = sourceFolder + QStringLiteral("/studio-background/owned.sbg");
    REQUIRE(QDir().mkpath(QFileInfo(sourceMask).absolutePath()));
    QFile source(sourceMask);
    REQUIRE(source.open(QIODevice::WriteOnly));
    REQUIRE(source.write("original-mask") == 13);
    source.close();
    const QString destination = folder.filePath(QStringLiteral("копия/new.kdenlive"));
    REQUIRE(document.copyStudioAssetsForSave(destination));
    QFile copied(QFileInfo(destination).dir().absoluteFilePath(QStringLiteral("studio-background/owned.sbg")));
    REQUIRE(copied.open(QIODevice::ReadOnly));
    REQUIRE(copied.readAll() == QByteArray("original-mask"));
    copied.close();
    KdenliveDoc coldDocument(undoStack);
    coldDocument.setUrl(QUrl::fromLocalFile(destination));
    KdenliveTests::studioDocumentRoot(coldDocument, QFileInfo(destination).absolutePath());
    REQUIRE(!coldDocument.documentRoot().isEmpty());
    REQUIRE(QDir(coldDocument.documentRoot()).absolutePath() == QFileInfo(destination).absolutePath());
    REQUIRE(QFileInfo(QDir(coldDocument.documentRoot()).absoluteFilePath(QStringLiteral("studio-background/owned.sbg"))).isFile());
    REQUIRE(coldDocument.projectDataFolder() != QFileInfo(destination).absolutePath());
    const QString secondDestination = folder.filePath(QStringLiteral("вторая копия/new.kdenlive"));
    REQUIRE(coldDocument.projectDataFolder(QFileInfo(secondDestination).absolutePath()) == QFileInfo(secondDestination).absolutePath());
    REQUIRE(coldDocument.copyStudioAssetsForSave(secondDestination));
    QFile secondMask(QFileInfo(secondDestination).dir().absoluteFilePath(QStringLiteral("studio-background/owned.sbg")));
    REQUIRE(secondMask.open(QIODevice::ReadOnly));
    REQUIRE(secondMask.readAll() == QByteArray("original-mask"));
    secondMask.close();
    coldDocument.setUrl(QUrl::fromLocalFile(secondDestination));
    REQUIRE(QDir(coldDocument.documentRoot()).absolutePath() == QFileInfo(destination).absolutePath());
    REQUIRE(QFile::rename(copied.fileName(), copied.fileName() + QStringLiteral(".offline")));
    const QString thirdDestination = folder.filePath(QStringLiteral("третья копия/new.kdenlive"));
    REQUIRE(coldDocument.copyStudioAssetsForSave(thirdDestination));
    QFile thirdMask(QFileInfo(thirdDestination).dir().absoluteFilePath(QStringLiteral("studio-background/owned.sbg")));
    REQUIRE(thirdMask.open(QIODevice::ReadOnly));
    REQUIRE(thirdMask.readAll() == QByteArray("original-mask"));
    REQUIRE(document.copyStudioAssetsForSave(destination));
    REQUIRE(copied.open(QIODevice::WriteOnly | QIODevice::Truncate));
    copied.write("different-mask");
    copied.close();
    REQUIRE_FALSE(document.copyStudioAssetsForSave(destination));
    QFile blocked(folder.filePath(QStringLiteral("blocked")));
    REQUIRE(blocked.open(QIODevice::WriteOnly));
    blocked.close();
    REQUIRE_FALSE(document.copyStudioAssetsForSave(blocked.fileName() + QStringLiteral("/new.kdenlive")));
    REQUIRE(source.open(QIODevice::ReadOnly));
    REQUIRE(source.readAll() == QByteArray("original-mask"));
    REQUIRE(document.url().fileName() == QStringLiteral("original.kdenlive"));
}

TEST_CASE("Studio clip camera motion keeps its actual frames after Save and Reopen", "[StudioCameraReopen]")
{
    const int mode = GENERATE(1, 2, 4);
    INFO("camera mode: " << mode);
    pCore->projectItemModel()->clean();
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString picture = folder.filePath(QStringLiteral("камера кадр.mkv"));
    const auto &profile = pCore->getProjectProfile();
    QProcess encode;
    encode.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("testsrc2=size=%1x%2:rate=%3").arg(profile.width()).arg(profile.height()).arg(pCore->getCurrentFps()),
        QStringLiteral("-frames:v"), QStringLiteral("100"), QStringLiteral("-c:v"), QStringLiteral("ffv1"), QStringLiteral("-an"), picture});
    REQUIRE(encode.waitForFinished(30000));
    INFO(encode.readAllStandardError().toStdString());
    REQUIRE(encode.exitCode() == 0);
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] {
        if (pCore->currentDoc()) {
            pCore->projectItemModel()->clean();
            pCore->projectManager()->closeCurrentDocument(false, false);
        }
    });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    const auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QByteArray path = QFile::encodeName(picture);
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), path.constData());
    REQUIRE(producer->is_valid());
    producer->set("length", 100);
    producer->set_in_and_out(0, 99);
    producer->set("kdenlive:duration", 100);
    const auto bin = pCore->projectItemModel();
    QString binId = QString::number(bin->getFreeClipId());
    Fun binUndo = [] { return true; }, binRedo = [] { return true; };
    REQUIRE(bin->requestAddBinClip(binId, producer, bin->getRootFolder()->clipId(), binUndo, binRedo));
    const int track = timeline->getTrackIndexFromPosition(2);
    int clipId = -1;
    REQUIRE(timeline->requestClipInsertion(binId, track, 0, clipId));
    REQUIRE(timeline->requestItemResize(clipId, 90, true, true) == 90);
    const auto stack = timeline->getClipEffectStack(clipId);
    REQUIRE(stack->appendEffect(QStringLiteral("studio_camera")));
    const auto camera = effectsById(stack, QStringLiteral("studio_camera")).front();
    for (const auto &[name, value] : {std::pair{QStringLiteral("mode"), QString::number(mode)},
                                     std::pair{QStringLiteral("live"), QStringLiteral("2")},
                                     std::pair{QStringLiteral("zoom"), QStringLiteral("180")},
                                     std::pair{QStringLiteral("start_x"), QStringLiteral("20")},
                                     std::pair{QStringLiteral("end_x"), QStringLiteral("80")},
                                     std::pair{QStringLiteral("tracking"), QStringLiteral("0")}})
        camera->setParameter(name, value, true);
    const int effectIn = camera->filter().get_in();
    const int effectOut = camera->filter().get_out();
    REQUIRE(effectOut - effectIn + 1 >= 90);
    const QList<int> positions{0, 30, 60};
    QList<QByteArray> before;
    for (int position : positions) before << studioFrameHash(timeline, position);
    camera->filter().set("disable", 1);
    QList<QByteArray> sourceBefore;
    for (int position : positions) sourceBefore << studioFrameHash(timeline, position);
    camera->filter().set("disable", 0);
    for (int index = 0; index < positions.size(); ++index) REQUIRE(before[index] != sourceBefore[index]);
    REQUIRE(before[0] != before[1]);
    REQUIRE(before[1] != before[2]);
    const QString saved = folder.filePath(QStringLiteral("движение камеры.kdenlive"));
    REQUIRE(pCore->projectManager()->testSaveFileAs(saved));
    undoStack->clear();
    binUndo = {};
    binRedo = {};
    pCore->projectItemModel()->clean();
    pCore->projectManager()->closeCurrentDocument(false, false);
    QUndoGroup reopenedUndo;
    auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(saved), folder.path(), &reopenedUndo, false, nullptr);
    REQUIRE(opened.isSuccessful());
    auto reopened = opened.getDocument();
    pCore->projectManager()->testSetDocument(reopened.get());
    const auto clearReopened = qScopeGuard([] {
        pCore->projectItemModel()->clean();
        pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(saved).lastModified(), 0));
    const auto sequence = pCore->projectItemModel()->getAllSequenceClips().value(reopened->uuid());
    pCore->projectManager()->openTimeline(sequence, -1, reopened->uuid());
    const auto live = reopened->getTimeline(reopened->uuid());
    pCore->projectManager()->testSetActiveTimeline(live);
    const int reopenedClip = live->getClipByPosition(live->getTrackIndexFromPosition(2), 0);
    REQUIRE(reopenedClip >= 0);
    REQUIRE(live->getClipPlaytime(reopenedClip) == 90);
    const auto loaded = effectsById(live->getClipEffectStack(reopenedClip), QStringLiteral("studio_camera"));
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded.front()->getParam(QStringLiteral("mode")) == QString::number(mode));
    REQUIRE(loaded.front()->getParam(QStringLiteral("live")) == QStringLiteral("2"));
    REQUIRE(loaded.front()->getParam(QStringLiteral("zoom")).toDouble() == Approx(180));
    REQUIRE(loaded.front()->getParam(QStringLiteral("tracking")) == QStringLiteral("0"));
    REQUIRE(loaded.front()->filter().get_in() == effectIn);
    REQUIRE(loaded.front()->filter().get_out() == effectOut);
    QList<QByteArray> after;
    for (int position : positions) after << studioFrameHash(live, position);
    loaded.front()->filter().set("disable", 1);
    QList<QByteArray> sourceAfter;
    for (int position : positions) sourceAfter << studioFrameHash(live, position);
    loaded.front()->filter().set("disable", 0);
    REQUIRE(sourceAfter == sourceBefore);
    REQUIRE(after == before);
}

static QByteArray studioExportedTextFrames(const QString &project)
{
    const QString video = project + QStringLiteral(".mkv");
    QProcess render;
    render.start(QStringLiteral("melt"), {project, QStringLiteral("in=0"), QStringLiteral("out=79"),
                                          QStringLiteral("-consumer"), QStringLiteral("avformat:") + video,
                                          QStringLiteral("vcodec=ffv1"), QStringLiteral("pix_fmt=bgra"),
                                          QStringLiteral("an=1"), QStringLiteral("real_time=-1")});
    const bool rendered = render.waitForFinished(120000);
    if (!rendered) { render.kill(); render.waitForFinished(1000); }
    INFO(render.readAllStandardError().toStdString());
    REQUIRE(rendered);
    REQUIRE(render.exitStatus() == QProcess::NormalExit);
    REQUIRE(render.exitCode() == 0);
    QByteArray frames;
    for (int position : {7, 40, 79}) {
        QProcess decode;
        decode.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), video,
                                                QStringLiteral("-vf"), QStringLiteral("select=eq(n\\,%1)").arg(position),
                                                QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-f"),
                                                QStringLiteral("rawvideo"), QStringLiteral("-pix_fmt"), QStringLiteral("rgba"),
                                                QStringLiteral("-")});
        const bool decoded = decode.waitForFinished(30000);
        if (!decoded) { decode.kill(); decode.waitForFinished(1000); }
        INFO(decode.readAllStandardError().toStdString());
        REQUIRE(decoded);
        REQUIRE(decode.exitStatus() == QProcess::NormalExit);
        REQUIRE(decode.exitCode() == 0);
        const QByteArray pixels = decode.readAllStandardOutput();
        REQUIRE(pixels.size() == pCore->getProjectProfile().width() * pCore->getProjectProfile().height() * 4);
        frames += pixels;
    }
    return frames;
}

TEST_CASE("Studio effect append undoes before clip creation and redoes after it", "[Studio][StudioEffectUndoOrder]")
{
    pCore->projectItemModel()->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const int track = timeline->getTrackIndexFromPosition(2);
    const QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", pCore->projectItemModel(), 30, false);
    Fun undo = [] { return true; }, redo = [] { return true; };
    int clipId = -1;
    REQUIRE(timeline->requestClipInsertion(binId, track, 0, clipId, false, false, false, undo, redo));
    auto stack = timeline->getClipEffectStack(clipId);
    REQUIRE(stack);
    const int initialEffects = stack->rowCount();
    const Fun clipUndo = undo, clipRedo = redo;
    bool ownerUndoRan = false, ownerRedoRan = false;
    undo = [&, clipUndo] {
        REQUIRE(timeline->isClip(clipId));
        REQUIRE(stack->rowCount() == initialEffects);
        ownerUndoRan = true;
        return clipUndo();
    };
    redo = [&, clipRedo] {
        REQUIRE_FALSE(timeline->isClip(clipId));
        REQUIRE(stack->rowCount() == initialEffects);
        const bool restored = clipRedo();
        REQUIRE(timeline->isClip(clipId));
        ownerRedoRan = true;
        return restored;
    };
    REQUIRE(stack->appendEffectWithUndo(QStringLiteral("brightness"), undo, redo).first);
    REQUIRE(stack->rowCount() == initialEffects + 1);
    REQUIRE(undo());
    REQUIRE(ownerUndoRan);
    REQUIRE_FALSE(timeline->isClip(clipId));
    REQUIRE(stack->rowCount() == initialEffects);
    REQUIRE(redo());
    REQUIRE(ownerRedoRan);
    REQUIRE(timeline->isClip(clipId));
    REQUIRE(stack->rowCount() == initialEffects + 1);
}

TEST_CASE("Studio Text native GUI save undo and redo preserve the title transaction", "[StudioTextUndoGUI][.gui]")
{
    REQUIRE_FALSE(pCore->window()); // Run alone in a fresh process with the real MainWindow.
    QTemporaryDir folder(QDir::temp().filePath(QStringLiteral("studio-text-undo-XXXXXX")));
    REQUIRE(folder.isValid());
    folder.setAutoRemove(false); // Retain the project even if the original Undo crash aborts the process.
    INFO("Text GUI evidence: " << folder.path().toStdString());
    qInfo().noquote() << "Text GUI evidence:" << folder.path();
    pCore->initGUI(QString(), QUrl());
    const auto closeDocument = qScopeGuard([] {
        if (pCore->currentDoc()) pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(studioWait([] { return pCore->currentDoc() && pCore->window()->getCurrentTimeline()
        && pCore->window()->getCurrentTimeline()->model(); }, 15000));
    const auto model = pCore->window()->getCurrentTimeline()->model();
    const auto videoTracks = model->getTracksIds(false);
    REQUIRE_FALSE(videoTracks.isEmpty());
    int topTrack = videoTracks.front();
    for (int track : videoTracks)
        if (model->getTrackPosition(track) > model->getTrackPosition(topTrack)) topTrack = track;
    const QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", pCore->projectItemModel(), 120, false);
    int baseClip = -1;
    REQUIRE(model->requestClipInsertion(binId, topTrack, 0, baseClip));
    REQUIRE(studioWait([] { return pCore->taskManager.backgroundIdle(); }, 15000));
    const auto &profile = pCore->getProjectProfile();
    const QString scene = folder.filePath(QStringLiteral("надпись.stxt"));
    QString error;
    REQUIRE(SunimoTextQt::save(scene, SunimoTextQt::compile({{QStringLiteral("text"), QStringLiteral("Первая строка\nВторая строка")},
        {QStringLiteral("duration"), 120.0 / pCore->getCurrentFps()}}, {profile.width(), profile.height()}), &error));
    const int beforeUndo = pCore->undoStack()->index(), beforeClips = model->getClipsCount(), beforeTracks = model->getTracksCount();
    struct TitleResult { bool finished = false, created = false; QString message; };
    auto result = std::make_shared<TitleResult>();
    REQUIRE(StudioText::createTitle(model, baseClip, 120, QStringLiteral("Две строки"), scene,
        [result](bool ok, const QString &message) { result->created = ok; result->message = message; result->finished = true; }));
    REQUIRE(studioWait([&] { return result->finished && pCore->taskManager.backgroundIdle(); }, 15000));
    INFO(result->message.toStdString());
    REQUIRE(result->created);
    REQUIRE(pCore->undoStack()->index() == beforeUndo + 1);
    REQUIRE(model->getClipsCount() == beforeClips + 1);
    REQUIRE(model->getTracksCount() == beforeTracks + 1);
    const auto selected = model->getCurrentSelection();
    REQUIRE(selected.size() == 1);
    const int titleId = *selected.begin();
    REQUIRE(model->isClip(titleId));
    REQUIRE(titleId != baseClip);
    const int titleTrack = model->getClipTrackId(titleId);
    REQUIRE(model->getTrackPosition(titleTrack) > model->getTrackPosition(topTrack));
    REQUIRE(effectsById(model->getClipEffectStack(titleId), QStringLiteral("sunimo_text_studio")).size() == 1);
    const QString project = folder.filePath(QStringLiteral("проверка.kdenlive"));
    // Native serialization and copied assets; save-copy preserves the live document and skips cachefiles KIO.
    REQUIRE(pCore->projectManager()->saveFileAs(project, true, true));
    REQUIRE(QFileInfo(project).isFile());
    REQUIRE(studioWait([] { return pCore->taskManager.backgroundIdle(); }, 15000));
    REQUIRE(pCore->undoStack()->index() == beforeUndo + 1);
    const auto savedEffects = effectsById(model->getClipEffectStack(titleId), QStringLiteral("sunimo_text_studio"));
    REQUIRE(savedEffects.size() == 1);
    const QString savedScene = savedEffects.front()->getParam(QStringLiteral("0"));
    REQUIRE(QFileInfo(savedScene).isFile());
    auto *undoAction = pCore->window()->actionCollection()->action(QStringLiteral("edit_undo"));
    auto *redoAction = pCore->window()->actionCollection()->action(QStringLiteral("edit_redo"));
    REQUIRE(undoAction); REQUIRE(redoAction); REQUIRE(undoAction->isEnabled());
    undoAction->trigger();
    REQUIRE(pCore->undoStack()->index() == beforeUndo);
    REQUIRE_FALSE(model->isClip(titleId));
    REQUIRE_FALSE(model->isTrack(titleTrack));
    REQUIRE(model->getClipsCount() == beforeClips);
    REQUIRE(model->getTracksCount() == beforeTracks);
    QApplication::processEvents(); // Deliver row updates queued before the selected stack was detached.
    REQUIRE(redoAction->isEnabled());
    redoAction->trigger();
    REQUIRE(pCore->undoStack()->index() == beforeUndo + 1);
    REQUIRE(model->isClip(titleId)); REQUIRE(model->isTrack(titleTrack));
    REQUIRE(model->getClipTrackId(titleId) == titleTrack);
    REQUIRE(model->getClipsCount() == beforeClips + 1);
    REQUIRE(model->getTracksCount() == beforeTracks + 1);
    const auto restored = effectsById(model->getClipEffectStack(titleId), QStringLiteral("sunimo_text_studio"));
    REQUIRE(restored.size() == 1);
    REQUIRE(restored.front()->getParam(QStringLiteral("0")) == savedScene);
    REQUIRE(QFileInfo(savedScene).isFile());
    QApplication::processEvents();
    folder.setAutoRemove(true);
}

TEST_CASE("Studio Text creates an independent undoable title clip", "[Studio]")
{
    const QString previousProfile = pCore->getCurrentProfilePath();
    const auto restoreProfile = qScopeGuard([previousProfile] { pCore->setCurrentProfile(previousProfile); });
    REQUIRE(pCore->setCurrentProfile(QStringLiteral("atsc_1080p_60")));
    const QByteArray previousPrefix = qgetenv("STUDIO_PREFIX");
    const QByteArray qaPrefix = qgetenv("STUDIO_QA_PREFIX");
    if (!qaPrefix.isEmpty()) qputenv("STUDIO_PREFIX", qaPrefix);
    const auto restorePrefix = qScopeGuard([previousPrefix] {
        if (previousPrefix.isNull()) qunsetenv("STUDIO_PREFIX"); else qputenv("STUDIO_PREFIX", previousPrefix);
    });
    pCore->projectItemModel()->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    const QString stage6Root = qEnvironmentVariable("STUDIO_QA_STAGE6_ROOT");
    QTemporaryDir folder(stage6Root.isEmpty() ? QString() : QDir(stage6Root).filePath(QStringLiteral("source-XXXXXX")));
    REQUIRE(folder.isValid());
    if (!stage6Root.isEmpty()) folder.setAutoRemove(false);
    const int previousLocation = KdenliveSettings::videotodefaultfolder();
    const QString previousVideoFolder = KdenliveSettings::videofolder();
    auto restoreSettings = qScopeGuard([previousLocation, previousVideoFolder] {
        KdenliveSettings::setVideotodefaultfolder(previousLocation);
        KdenliveSettings::setVideofolder(previousVideoFolder);
    });
    KdenliveSettings::setVideotodefaultfolder(KdenliveDoc::SaveToProjectFolder);
    document.m_sameProjectFolder = true;
    document.setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    if (!stage6Root.isEmpty()) {
        QFile idFile(QDir(stage6Root).filePath(QStringLiteral("documentid.txt")));
        REQUIRE(idFile.open(QIODevice::WriteOnly));
        REQUIRE(idFile.write(document.getDocumentProperty(QStringLiteral("documentid")).toUtf8()) > 0);
    }
    document.setProjectFolder(QUrl::fromLocalFile(folder.path()));
    const QString managedScenes = folder.filePath(QStringLiteral("studio-text"));
    REQUIRE(QDir().mkpath(managedScenes));
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([&document] {
        if (pCore->currentDoc() != &document) return;
        pCore->taskManager.slotCancelJobs();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const int baseTrack = timeline->getTrackIndexFromPosition(2);
    const auto &profile = pCore->getProjectProfile();
    // Source footage stays in a shared media folder; Save As relocates managed Text/ASS assets.
    const QString mediaFolder = stage6Root.isEmpty() ? folder.filePath(QStringLiteral("media")) : QDir(stage6Root).filePath(QStringLiteral("media"));
    REQUIRE(QDir().mkpath(mediaFolder));
    const QString video = QDir(mediaFolder).filePath(QStringLiteral("исходное видео.mkv"));
    QProcess encode;
    encode.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("testsrc2=size=%1x%2:rate=60").arg(profile.width()).arg(profile.height()),
        QStringLiteral("-frames:v"), QStringLiteral("120"), QStringLiteral("-c:v"), QStringLiteral("ffv1"), QStringLiteral("-an"), video});
    const bool encoded = encode.waitForFinished(30000);
    if (!encoded) { encode.kill(); encode.waitForFinished(1000); }
    INFO(encode.readAllStandardError().toStdString());
    REQUIRE(encoded);
    REQUIRE(encode.exitStatus() == QProcess::NormalExit);
    REQUIRE(encode.exitCode() == 0);
    auto videoProducer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), QFile::encodeName(video).constData());
    REQUIRE(videoProducer->is_valid());
    REQUIRE(videoProducer->get_length() == 120);
    const auto bin = pCore->projectItemModel();
    QString binId = QString::number(bin->getFreeClipId());
    {
        Fun binUndo = [] { return true; }, binRedo = [] { return true; };
        REQUIRE(bin->requestAddBinClip(binId, videoProducer, bin->getRootFolder()->clipId(), binUndo, binRedo));
    }
    REQUIRE(bin->getClipByBinID(binId)->hasVideo());
    REQUIRE(bin->getClipByBinID(binId)->clipType() == ClipType::Video);
    int clipId = -1;
    REQUIRE(timeline->requestClipInsertion(binId, baseTrack, 0, clipId));
    const auto before = frameHashes(timeline).at(1);
    REQUIRE(studioFrameHash(timeline, 7) != studioFrameHash(timeline, 40));
    const QString scene = QDir(managedScenes).filePath(QStringLiteral("привет.stxt"));
    QString sceneError;
    REQUIRE(SunimoTextQt::save(scene, SunimoTextQt::compile({{QStringLiteral("text"), QStringLiteral("Привет, монтаж!")},
                                                           {QStringLiteral("duration"), 4}}, {profile.width(), profile.height()}), &sceneError));
    const int beforeRejectedTitle = undoStack->index(), beforeRejectedTracks = timeline->getTracksCount();
    REQUIRE_FALSE(StudioText::createTitle(timeline, clipId, 100, QStringLiteral("Нет asset"), scene + QStringLiteral(".missing"),
                                          [](bool, const QString &) {}));
    REQUIRE(undoStack->index() == beforeRejectedTitle);
    REQUIRE(timeline->getClipsCount() == 1);
    REQUIRE(timeline->getTracksCount() == beforeRejectedTracks);
    timeline->setTrackLockedState(baseTrack, true);
    const int beforeLockedTitle = undoStack->index();
    REQUIRE_FALSE(StudioText::createTitle(timeline, clipId, 100, QStringLiteral("Заблокировано"), scene,
                                          [](bool, const QString &) {}));
    REQUIRE(undoStack->index() == beforeLockedTitle);
    REQUIRE(timeline->getClipsCount() == 1);
    REQUIRE(timeline->getTracksCount() == beforeRejectedTracks);
    timeline->setTrackLockedState(baseTrack, false);
    const int undoIndex = undoStack->index();
    bool completed = false, accepted = false;
    QString failure;
    REQUIRE(StudioText::createTitle(timeline, clipId, 100, QStringLiteral("Надпись: Привет, монтаж!"), scene,
                                    [&](bool success, const QString &message) { completed = true; accepted = success; failure = message; }));
    QElapsedTimer timer;
    timer.start();
    while (!completed && timer.elapsed() < 10000) QApplication::processEvents(QEventLoop::AllEvents, 50);
    INFO(failure.toStdString());
    REQUIRE(completed);
    REQUIRE(accepted);
    REQUIRE(timeline->getClipsCount() == 2);
    REQUIRE(undoStack->index() == undoIndex + 1);
    int titleId = -1;
    for (int track : timeline->getTracksIds(false)) {
        const int id = timeline->getClipByPosition(track, 0);
        if (id >= 0 && id != clipId) titleId = id;
    }
    REQUIRE(titleId >= 0);
    REQUIRE(timeline->getTrackPosition(timeline->getClipTrackId(titleId)) > timeline->getTrackPosition(baseTrack));
    REQUIRE(timeline->getCurrentSelection().size() == 1);
    REQUIRE(timeline->getCurrentSelection().count(titleId) == 1);
    const auto effects = effectsById(timeline->getClipEffectStack(titleId), QStringLiteral("sunimo_text_studio"));
    REQUIRE(effects.size() == 1);
    REQUIRE(QFileInfo::exists(effects.front()->getParam(QStringLiteral("0"))));
    REQUIRE(frameHashes(timeline).at(1) != before);
    undoStack->undo();
    REQUIRE(timeline->getClipsCount() == 1);
    undoStack->redo();
    REQUIRE(timeline->getClipsCount() == 2);
    const int tracksBeforeSecond = timeline->getTracksCount();
    const QString secondScene = QDir(managedScenes).filePath(QStringLiteral("вторая.stxt"));
    REQUIRE(QFile::copy(scene, secondScene));
    bool secondCompleted = false, secondAccepted = false;
    REQUIRE(StudioText::createTitle(timeline, clipId, 60, QStringLiteral("Вторая надпись"), secondScene,
                                    [&](bool success, const QString &) { secondCompleted = true; secondAccepted = success; }));
    timer.restart();
    while (!secondCompleted && timer.elapsed() < 10000) QApplication::processEvents(QEventLoop::AllEvents, 50);
    REQUIRE(secondCompleted);
    REQUIRE(secondAccepted);
    REQUIRE(timeline->getClipsCount() == 3);
    REQUIRE(timeline->getTracksCount() == tracksBeforeSecond + 1);
    const auto secondSelection = timeline->getCurrentSelection();
    REQUIRE(secondSelection.size() == 1);
    const int secondTitle = *secondSelection.begin();
    REQUIRE(timeline->getItemPosition(secondTitle) == 0);
    undoStack->undo();
    REQUIRE(timeline->getClipsCount() == 2);
    REQUIRE(timeline->getTracksCount() == tracksBeforeSecond);
    const auto titleFrames = frameHashes(timeline);
    REQUIRE(titleFrames.at(1) != before);
    const QString revisedScene = QDir(managedScenes).filePath(QStringLiteral("правка.stxt"));
    REQUIRE(SunimoTextQt::save(revisedScene, SunimoTextQt::compile({{QStringLiteral("text"), QStringLiteral("Новая надпись")},
                                                                   {QStringLiteral("outlineEnabled"), true},
                                                                   {QStringLiteral("outlineWidth"), 10},
                                                                   {QStringLiteral("outlineColor"), QStringLiteral("#aaff0000")},
                                                                   {QStringLiteral("backgroundEnabled"), true},
                                                                   {QStringLiteral("backgroundColor"), QStringLiteral("#800000ff")},
                                                                   {QStringLiteral("group"), 0},
                                                                   {QStringLiteral("styleId"), 7},
                                                                   {QStringLiteral("inPreset"), 72},
                                                                   {QStringLiteral("outPreset"), 1},
                                                                   {QStringLiteral("order"), 2},
                                                                   {QStringLiteral("lag"), .4},
                                                                   {QStringLiteral("lifeSource"), 72},
                                                                   {QStringLiteral("lifeAmount"), .85},
                                                                   {QStringLiteral("lifeSpeed"), 1.6},
                                                                   {QStringLiteral("duration"), 120.0 / pCore->getCurrentFps()}},
                                                                  {profile.width(), profile.height()}), &sceneError));
    const auto activeEffect = effectsById(timeline->getClipEffectStack(titleId), QStringLiteral("sunimo_text_studio"));
    REQUIRE(activeEffect.size() == 1);
    const int beforeRejectedUpdate = undoStack->index(), beforeRejectedUpdateTracks = timeline->getTracksCount();
    REQUIRE_FALSE(StudioText::updateTitle(timeline, titleId, activeEffect.front(), 100, revisedScene + QStringLiteral(".missing")));
    REQUIRE(undoStack->index() == beforeRejectedUpdate);
    REQUIRE(timeline->getClipPlaytime(titleId) == 100);
    REQUIRE(activeEffect.front()->getParam(QStringLiteral("0")) == scene);
    timeline->setTrackLockedState(timeline->getClipTrackId(titleId), true);
    const int beforeLockedUpdate = undoStack->index();
    REQUIRE_FALSE(StudioText::updateTitle(timeline, titleId, activeEffect.front(), 100, revisedScene));
    REQUIRE(undoStack->index() == beforeLockedUpdate);
    REQUIRE(timeline->getClipsCount() == 2);
    REQUIRE(timeline->getTracksCount() == beforeRejectedUpdateTracks);
    REQUIRE(timeline->getClipPlaytime(titleId) == 100);
    REQUIRE(activeEffect.front()->getParam(QStringLiteral("0")) == scene);
    timeline->setTrackLockedState(timeline->getClipTrackId(titleId), false);
    REQUIRE(StudioText::updateTitle(timeline, titleId, activeEffect.front(), 120, revisedScene));
    REQUIRE(timeline->getClipPlaytime(titleId) == 120);
    const auto revisedFrames = frameHashes(timeline);
    REQUIRE(revisedFrames.at(1) != titleFrames.at(1));
    const auto requireTitleExit = [&](int id) {
        const auto found = effectsById(timeline->getClipEffectStack(id), QStringLiteral("sunimo_text_studio"));
        REQUIRE(found.size() == 1);
        const int start = timeline->getItemPosition(id), length = timeline->getClipPlaytime(id);
        QList<int> contrast;
        for (int local : {length / 2, length - 1}) {
            const QByteArray visible = studioFrameAt(timeline, start + local);
            const int disabled = found.front()->filter().get_int("disable");
            found.front()->filter().set("disable", 1);
            const auto restore = qScopeGuard([&found, disabled] { found.front()->filter().set("disable", disabled); });
            const QByteArray background = studioFrameAt(timeline, start + local);
            REQUIRE(visible.size() == background.size());
            int difference = 0;
            for (qsizetype pixel = 0; pixel < visible.size(); pixel += 4)
                for (int channel = 0; channel < 3; ++channel)
                    difference += std::abs(int(quint8(visible[pixel + channel])) - int(quint8(background[pixel + channel])));
            contrast << difference;
        }
        INFO("title=" << id << " start=" << start << " length=" << length
             << " middle contrast=" << contrast[0] << " final contrast=" << contrast[1]);
        REQUIRE(contrast[0] > 1000);
        REQUIRE(contrast[1] < contrast[0] / 10);
    };
    requireTitleExit(titleId);
    undoStack->undo();
    REQUIRE(timeline->getClipPlaytime(titleId) == 100);
    REQUIRE(activeEffect.front()->getParam(QStringLiteral("0")) == scene);
    REQUIRE(frameHashes(timeline) == titleFrames);
    undoStack->redo();
    REQUIRE(timeline->getClipPlaytime(titleId) == 120);
    REQUIRE(activeEffect.front()->getParam(QStringLiteral("0")) == revisedScene);
    REQUIRE(frameHashes(timeline) == revisedFrames);
    const QString previousLength = activeEffect.front()->getParam(QStringLiteral("1"));
    const int beforeTrim = undoStack->index();
    REQUIRE(timeline->requestItemResize(titleId, 80, true, true) == 80);
    REQUIRE(undoStack->index() == beforeTrim + 1);
    REQUIRE(activeEffect.front()->getParam(QStringLiteral("1")).toDouble() == Approx(80.0 / (pCore->getCurrentFps() * 120.0)));
    undoStack->undo();
    REQUIRE(timeline->getClipPlaytime(titleId) == 120);
    REQUIRE(activeEffect.front()->getParam(QStringLiteral("1")) == previousLength);
    undoStack->redo();
    REQUIRE(timeline->getClipPlaytime(titleId) == 80);
    requireTitleExit(titleId);
    const int beforeLeftTrim = undoStack->index();
    REQUIRE(timeline->requestItemResize(titleId, 60, false, true) == 60);
    REQUIRE(undoStack->index() == beforeLeftTrim + 1);
    REQUIRE(timeline->getItemPosition(titleId) == 20);
    // Native title clips rebase their source range after left trim; the filter clock starts at zero.
    REQUIRE(timeline->getClipIn(titleId) == 0);
    requireTitleExit(titleId);
    REQUIRE(activeEffect.front()->getParam(QStringLiteral("1")).toDouble() == Approx(60.0 / (pCore->getCurrentFps() * 120.0)));
    undoStack->undo();
    REQUIRE(timeline->getItemPosition(titleId) == 0);
    REQUIRE(timeline->getClipPlaytime(titleId) == 80);
    const int beforeCut = undoStack->index();
    REQUIRE(TimelineFunctions::requestClipCut(timeline, titleId, 40));
    REQUIRE(undoStack->index() == beforeCut + 1);
    const int rightTitle = timeline->getClipByPosition(timeline->getClipTrackId(titleId), 40);
    REQUIRE(rightTitle >= 0);
    REQUIRE(rightTitle != titleId);
    REQUIRE(timeline->getClipsCount() == 3);
    const auto rightEffects = effectsById(timeline->getClipEffectStack(rightTitle), QStringLiteral("sunimo_text_studio"));
    REQUIRE(rightEffects.size() == 1);
    REQUIRE(rightEffects.front()->getParam(QStringLiteral("0")) == revisedScene);
    REQUIRE(activeEffect.front()->getParam(QStringLiteral("1")).toDouble() == Approx(40.0 / (pCore->getCurrentFps() * 120.0)));
    REQUIRE(rightEffects.front()->getParam(QStringLiteral("1")).toDouble() == Approx(40.0 / (pCore->getCurrentFps() * 120.0)));
    requireTitleExit(titleId);
    requireTitleExit(rightTitle);
    const QString copiedXml = TimelineFunctions::copyClips(timeline, {rightTitle}, rightTitle);
    REQUIRE_FALSE(copiedXml.isEmpty());
    REQUIRE(TimelineFunctions::pasteClips(timeline, copiedXml, timeline->getClipTrackId(rightTitle), 80));
    const int pastedTitle = timeline->getClipByPosition(timeline->getClipTrackId(rightTitle), 80);
    REQUIRE(pastedTitle >= 0);
    REQUIRE(timeline->getClipPlaytime(pastedTitle) == 40);
    const auto pastedEffects = effectsById(timeline->getClipEffectStack(pastedTitle), QStringLiteral("sunimo_text_studio"));
    REQUIRE(pastedEffects.size() == 1);
    REQUIRE(pastedEffects.front()->getParam(QStringLiteral("0")) == revisedScene);
    REQUIRE(pastedEffects.front()->getParam(QStringLiteral("1")).toDouble() == Approx(40.0 / (pCore->getCurrentFps() * 120.0)));
    requireTitleExit(pastedTitle);
    undoStack->undo();
    REQUIRE_FALSE(timeline->isClip(pastedTitle));
    undoStack->redo();
    REQUIRE(timeline->isClip(pastedTitle));
    undoStack->undo();
    undoStack->undo();
    REQUIRE(timeline->getClipsCount() == 2);
    REQUIRE(timeline->getClipPlaytime(titleId) == 80);
    REQUIRE(activeEffect.front()->getParam(QStringLiteral("1")).toDouble() == Approx(80.0 / (pCore->getCurrentFps() * 120.0)));
    const QByteArray withoutSubtitles = studioFrameHash(timeline, 7);
    auto subtitles = timeline->createSubtitleModel();
    REQUIRE(subtitles);
    const QString subtitleSource = folder.filePath(QStringLiteral("черновик.ass"));
    QFile subtitleFile(subtitleSource);
    REQUIRE(subtitleFile.open(QIODevice::WriteOnly));
    const QByteArray subtitleContents = QStringLiteral(R"ASS([Script Info]
ScriptType: v4.00+
PlayResX: 1920
PlayResY: 1080

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Stage6-before,DejaVu Sans,64,&H00FFFFFF,&H0000FFFF,&H00000000,&H66000000,1,0,0,0,100,100,0,0,1,3,0,2,80,80,88,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.10,0:00:00.35,Stage6-before,,0,0,0,,Черновик
)ASS").toUtf8();
    REQUIRE(subtitleFile.write(subtitleContents) == subtitleContents.size());
    subtitleFile.close();
    subtitles->importSubtitle(subtitleSource, 0, true);
    REQUIRE(subtitles->rowCount() == 1);
    const int firstSubtitle = *subtitles->getAllSubIds().begin();
    subtitles->editSubtitle(firstSubtitle, QStringLiteral("Исправленный субтитр"), QStringLiteral("Черновик"));
    REQUIRE(subtitles->getText(firstSubtitle) == QStringLiteral("Исправленный субтитр"));
    const auto activeSequence = pCore->projectItemModel()->getAllSequenceClips().value(document.uuid());
    REQUIRE_FALSE(activeSequence.isEmpty());
    pCore->projectManager()->openTimeline(activeSequence, -1, document.uuid());
    QString styledSubtitle;
    {
        StudioSubtitlePage subtitlePage;
        auto restyle = subtitlePage.findChild<QPushButton *>(QStringLiteral("studioSubtitleRestyle"));
        auto background = subtitlePage.findChild<QComboBox *>(QStringLiteral("studioSubtitle_background"));
        auto animation = subtitlePage.findChild<QComboBox *>(QStringLiteral("studioSubtitle_animation"));
        auto status = subtitlePage.findChild<QLabel *>(QStringLiteral("studioSubtitleStatus"));
        REQUIRE(restyle); REQUIRE(background); REQUIRE(animation); REQUIRE(status);
        background->setCurrentIndex(0);
        animation->setCurrentIndex(7);
        timeline->requestSetSelection({firstSubtitle});
        subtitlePage.activate();
        REQUIRE(restyle->isEnabled());
        const int beforeStyle = undoStack->index();
        restyle->click();
        REQUIRE(studioWait([&] { return status->text().contains(QStringLiteral("Переоформлено блоков: 1")); }, 30000));
        REQUIRE(undoStack->index() == beforeStyle + 1);
        REQUIRE(subtitles->rowCount() == 1);
        styledSubtitle = subtitles->getStyleName(*subtitles->getAllSubIds().begin());
        REQUIRE(styledSubtitle != QStringLiteral("Stage6-before"));
        REQUIRE(subtitles->getText(*subtitles->getAllSubIds().begin()).contains(QStringLiteral("Исправленный субтитр")));
        undoStack->undo();
        REQUIRE(subtitles->getStyleName(firstSubtitle) == QStringLiteral("Stage6-before"));
        undoStack->redo();
        REQUIRE(subtitles->getStyleName(*subtitles->getAllSubIds().begin()) == styledSubtitle);

        timeline->requestSetSelection({*subtitles->getAllSubIds().begin()});
        subtitlePage.refreshSelection();
        REQUIRE(restyle->isEnabled());
        restyle->click();
        REQUIRE(studioWait([&] { return status->text().contains(QStringLiteral("Переоформлено блоков: 1")) && restyle->isEnabled(); }, 30000));
        REQUIRE(subtitles->rowCount() == 1);
        timeline->requestSetSelection({*subtitles->getAllSubIds().begin()});
        subtitlePage.refreshSelection();
        REQUIRE(restyle->isEnabled());
        const int stableUndo = undoStack->index();
        restyle->click();
        INFO(status->text().toStdString());
        REQUIRE(status->text().contains(QStringLiteral("Меняю оформление")));
        auto alternateUndo = std::make_shared<DocUndoStack>(nullptr);
        KdenliveDoc alternate(alternateUndo);
        pCore->projectManager()->testSetDocument(&alternate);
        const auto restoreDocument = qScopeGuard([&document] { pCore->projectManager()->testSetDocument(&document); });
        const bool rejected = studioWait([&] { return !status->text().contains(QStringLiteral("Меняю оформление")); }, 30000);
        INFO(status->text().toStdString());
        REQUIRE(rejected);
        REQUIRE(status->text().contains(QStringLiteral("Проект не изменён")));
        REQUIRE(undoStack->index() == stableUndo);
        REQUIRE(subtitles->rowCount() == 1);
        REQUIRE(subtitles->getStyleName(*subtitles->getAllSubIds().begin()) == styledSubtitle);
        subtitlePage.deactivate();
    }
    const int renderWidth = profile.width(), renderHeight = profile.height();
    REQUIRE(studioFrameHash(timeline, 7) != withoutSubtitles);
    QByteArray expectedExport;
    for (int position : {7, 40, 79}) expectedExport += studioFrameAt(timeline, position, renderWidth, renderHeight);
    const QString saved = folder.filePath(QStringLiteral("Надпись тест.kdenlive"));
    // This headless helper serializes the real model. The GUI Save As flow remains a separate acceptance check.
    subtitles->copySubtitle(saved + QStringLiteral(".ass"), 0, false, true);
    REQUIRE(pCore->projectManager()->testSaveFileAs(saved));
    document.setUrl(QUrl::fromLocalFile(saved));
    REQUIRE(document.extractExternalEffectFiles().contains(revisedScene));
    {
        const QString lastEditScene = QDir(managedScenes).filePath(QStringLiteral("последняя правка.stxt"));
        auto lastEditSettings = SunimoTextQt::metadata(revisedScene);
        lastEditSettings.insert(QStringLiteral("text"), QStringLiteral("Undo и Redo после Save As"));
        REQUIRE(SunimoTextQt::save(lastEditScene, SunimoTextQt::compile(lastEditSettings, {renderWidth, renderHeight}), &sceneError));
        const int beforeSaveEdit = undoStack->index();
        const QUrl previousUrl = document.url();
        const QVariant previousSaveRoot = document.property("_studioTextSaveRoot");
        const QVariant previousAssetPaths = document.property("_studioTextAssetPaths");
        const QString hiddenA = revisedScene + QStringLiteral(".undo-hidden"), hiddenB = lastEditScene + QStringLiteral(".undo-hidden");
        const auto restoreSaveAs = qScopeGuard([&] {
            if (QFileInfo::exists(hiddenA)) QFile::rename(hiddenA, revisedScene);
            if (QFileInfo::exists(hiddenB)) QFile::rename(hiddenB, lastEditScene);
            undoStack->setIndex(beforeSaveEdit);
            document.setProperty("_studioTextSaveRoot", previousSaveRoot);
            document.setProperty("_studioTextAssetPaths", previousAssetPaths);
            activeEffect.front()->setParameter(QStringLiteral("0"), revisedScene, true);
            document.setUrl(previousUrl);
        });
        const QByteArray frameA = studioFrameAt(timeline, 40, renderWidth, renderHeight);
        REQUIRE(frameA.size() == renderWidth * renderHeight * 4);
        REQUIRE(StudioText::updateTitle(timeline, titleId, activeEffect.front(), 80, lastEditScene));
        REQUIRE(undoStack->index() == beforeSaveEdit + 1);
        const QByteArray frameB = studioFrameAt(timeline, 40, renderWidth, renderHeight);
        REQUIRE(frameB != frameA);
        const QString undoCopy = folder.filePath(QStringLiteral("Undo копия/Надпись.kdenlive"));
        const QString undoCopyRoot = QFileInfo(undoCopy).dir().filePath(QStringLiteral("studio-text"));
        const QString copiedA = QDir(undoCopyRoot).filePath(QFileInfo(revisedScene).fileName());
        const QString copiedB = QDir(undoCopyRoot).filePath(QFileInfo(lastEditScene).fileName());
        REQUIRE(document.copyStudioAssetsForSave(undoCopy));
        REQUIRE(pCore->projectManager()->testSaveFileAs(undoCopy));
        REQUIRE(QFileInfo::exists(copiedA));
        REQUIRE(QFileInfo::exists(copiedB));
        // Mirror the successful GUI caller without reopening: its mapping contains only the current scene B.
        document.setProperty("_studioTextSaveRoot", undoCopyRoot);
        document.updateStudioEffectAssetPaths({{lastEditScene, copiedB}});
        document.setUrl(QUrl::fromLocalFile(undoCopy));
        undoStack->setClean();
        REQUIRE(undoStack->index() == beforeSaveEdit + 1);
        REQUIRE(activeEffect.front()->getParam(QStringLiteral("0")) == copiedB);
        REQUIRE(QFile::rename(revisedScene, hiddenA));
        REQUIRE(QFile::rename(lastEditScene, hiddenB));
        REQUIRE_FALSE(QFileInfo::exists(revisedScene));
        REQUIRE_FALSE(QFileInfo::exists(lastEditScene));
        undoStack->undo();
        REQUIRE(undoStack->index() == beforeSaveEdit);
        REQUIRE(activeEffect.front()->getParam(QStringLiteral("0")) == copiedA);
        REQUIRE(studioFrameAt(timeline, 40, renderWidth, renderHeight) == frameA);
        undoStack->redo();
        REQUIRE(undoStack->index() == beforeSaveEdit + 1);
        REQUIRE(activeEffect.front()->getParam(QStringLiteral("0")) == copiedB);
        REQUIRE(studioFrameAt(timeline, 40, renderWidth, renderHeight) == frameB);
        {
            const QString missingHistorical = copiedA + QStringLiteral(".historical-hidden");
            const auto restoreHistorical = qScopeGuard([&] {
                if (QFileInfo::exists(missingHistorical)) QFile::rename(missingHistorical, copiedA);
            });
            REQUIRE(document.property("_studioTextAssetPaths").toMap().value(revisedScene).toString() == copiedA);
            REQUIRE(QFile::rename(copiedA, missingHistorical));
            REQUIRE_FALSE(document.copyStudioAssetsForSave(undoCopy));
            REQUIRE(activeEffect.front()->getParam(QStringLiteral("0")) == copiedB);
            REQUIRE(undoStack->index() == beforeSaveEdit + 1);
            REQUIRE(QFileInfo::exists(copiedB));
            REQUIRE(QFile::rename(missingHistorical, copiedA));
            REQUIRE(document.copyStudioAssetsForSave(undoCopy));
        }
        const auto titleStack = timeline->getClipEffectStack(titleId);
        titleStack->removeEffect(activeEffect.front()); // Native wrapper for removeEffectWithUndo and PUSH_UNDO.
        REQUIRE(undoStack->index() == beforeSaveEdit + 2);
        REQUIRE(effectsById(titleStack, QStringLiteral("sunimo_text_studio")).isEmpty());
        REQUIRE(studioFrameAt(timeline, 40, renderWidth, renderHeight) != frameB);
        const QString deletedCopy = folder.filePath(QStringLiteral("Undo удаления/Надпись.kdenlive"));
        const QString deletedCopyRoot = QFileInfo(deletedCopy).dir().filePath(QStringLiteral("studio-text"));
        const QString twiceCopiedB = QDir(deletedCopyRoot).filePath(QFileInfo(lastEditScene).fileName());
        REQUIRE(document.copyStudioAssetsForSave(deletedCopy));
        REQUIRE(pCore->projectManager()->testSaveFileAs(deletedCopy));
        QFile deletedProject(deletedCopy);
        REQUIRE(deletedProject.open(QIODevice::ReadOnly));
        REQUIRE_FALSE(deletedProject.readAll().contains("frei0r.sunimo_text_studio"));
        REQUIRE(QFileInfo::exists(twiceCopiedB));
        document.setProperty("_studioTextSaveRoot", deletedCopyRoot);
        document.updateStudioEffectAssetPaths({}); // No live Text filter supplies a serialized asset mapping.
        document.setUrl(QUrl::fromLocalFile(deletedCopy));
        undoStack->setClean();
        const QString hiddenCopyA = copiedA + QStringLiteral(".delete-hidden"), hiddenCopyB = copiedB + QStringLiteral(".delete-hidden");
        const auto restoreFirstCopy = qScopeGuard([&] {
            if (QFileInfo::exists(hiddenCopyA)) QFile::rename(hiddenCopyA, copiedA);
            if (QFileInfo::exists(hiddenCopyB)) QFile::rename(hiddenCopyB, copiedB);
        });
        REQUIRE(QFile::rename(copiedA, hiddenCopyA));
        REQUIRE(QFile::rename(copiedB, hiddenCopyB));
        REQUIRE_FALSE(QFileInfo::exists(copiedA));
        REQUIRE_FALSE(QFileInfo::exists(copiedB));
        undoStack->undo();
        REQUIRE(undoStack->index() == beforeSaveEdit + 1);
        const auto restoredText = effectsById(titleStack, QStringLiteral("sunimo_text_studio"));
        REQUIRE(restoredText.size() == 1);
        REQUIRE(restoredText.front() == activeEffect.front());
        REQUIRE(restoredText.front()->getParam(QStringLiteral("0")) == twiceCopiedB);
        REQUIRE(studioFrameAt(timeline, 40, renderWidth, renderHeight) == frameB);
        REQUIRE(timeline->requestItemDeletion(titleId, true));
        REQUIRE(undoStack->index() == beforeSaveEdit + 2);
        REQUIRE_FALSE(timeline->isClip(titleId));
        const QByteArray withoutTitle = studioFrameAt(timeline, 40, renderWidth, renderHeight);
        REQUIRE(withoutTitle != frameB);
        const QString clipDeletedCopy = folder.filePath(QStringLiteral("Undo удаления клипа/Надпись.kdenlive"));
        const QString clipDeletedRoot = QFileInfo(clipDeletedCopy).dir().filePath(QStringLiteral("studio-text"));
        const QString thriceCopiedB = QDir(clipDeletedRoot).filePath(QFileInfo(lastEditScene).fileName());
        REQUIRE(document.copyStudioAssetsForSave(clipDeletedCopy));
        REQUIRE(pCore->projectManager()->testSaveFileAs(clipDeletedCopy));
        QFile clipDeletedProject(clipDeletedCopy);
        REQUIRE(clipDeletedProject.open(QIODevice::ReadOnly));
        REQUIRE_FALSE(clipDeletedProject.readAll().contains("frei0r.sunimo_text_studio"));
        REQUIRE(QFileInfo::exists(thriceCopiedB));
        document.setProperty("_studioTextSaveRoot", clipDeletedRoot);
        document.updateStudioEffectAssetPaths({});
        document.setUrl(QUrl::fromLocalFile(clipDeletedCopy));
        undoStack->setClean();
        const QString hiddenSecondCopy = twiceCopiedB + QStringLiteral(".clip-hidden");
        const auto restoreSecondCopy = qScopeGuard([&] {
            if (QFileInfo::exists(hiddenSecondCopy)) QFile::rename(hiddenSecondCopy, twiceCopiedB);
        });
        REQUIRE(QFile::rename(twiceCopiedB, hiddenSecondCopy));
        REQUIRE_FALSE(QFileInfo::exists(twiceCopiedB));
        undoStack->undo();
        REQUIRE(undoStack->index() == beforeSaveEdit + 1);
        REQUIRE(timeline->isClip(titleId));
        const auto restoredClipText = effectsById(timeline->getClipEffectStack(titleId), QStringLiteral("sunimo_text_studio"));
        REQUIRE(restoredClipText.size() == 1);
        REQUIRE(restoredClipText.front()->getParam(QStringLiteral("0")) == thriceCopiedB);
        REQUIRE(studioFrameAt(timeline, 40, renderWidth, renderHeight) == frameB);
        undoStack->redo();
        REQUIRE(undoStack->index() == beforeSaveEdit + 2);
        REQUIRE_FALSE(timeline->isClip(titleId));
        REQUIRE(activeEffect.front()->getParam(QStringLiteral("0")) == thriceCopiedB);
        REQUIRE(studioFrameAt(timeline, 40, renderWidth, renderHeight) == withoutTitle);
    }
    REQUIRE(QFileInfo::exists(revisedScene));
    REQUIRE(document.url() == QUrl::fromLocalFile(saved));
    REQUIRE(activeEffect.front()->getParam(QStringLiteral("0")) == revisedScene);
    REQUIRE(studioFrameAt(timeline, 40, renderWidth, renderHeight) == expectedExport.mid(renderWidth * renderHeight * 4, renderWidth * renderHeight * 4));
    REQUIRE(studioWait([] { return pCore->taskManager.backgroundIdle(); }));
    // ClipLoadTask's deferred QObject deletion releases the completed title callback and its timeline.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    pCore->projectManager()->closeCurrentDocument(false, false);
    QUndoGroup reopenedUndo;
    auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(saved), folder.path(), &reopenedUndo, false, nullptr);
    REQUIRE(opened.isSuccessful());
    auto reopened = opened.getDocument();
    pCore->projectManager()->testSetDocument(reopened.get());
    const auto closeReopened = qScopeGuard([&reopened] {
        if (pCore->currentDoc() == reopened.get())
            pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(saved).lastModified(), 0));
    const auto sequence = pCore->projectItemModel()->getAllSequenceClips().value(reopened->uuid());
    pCore->projectManager()->openTimeline(sequence, -1, reopened->uuid());
    auto reopenedTimeline = reopened->getTimeline(reopened->uuid());
    pCore->projectManager()->testSetActiveTimeline(reopenedTimeline);
    REQUIRE(reopenedTimeline->getClipsCount() == 2);
    auto reopenedSubtitles = reopenedTimeline->getSubtitleModel();
    REQUIRE(reopenedSubtitles);
    REQUIRE(reopenedSubtitles->rowCount() == 1);
    REQUIRE(reopenedSubtitles->getText(*reopenedSubtitles->getAllSubIds().begin()).contains(QStringLiteral("Исправленный субтитр")));
    REQUIRE(reopenedSubtitles->getStyleName(*reopenedSubtitles->getAllSubIds().begin()) == styledSubtitle);
    QString reopenedScene;
    int reopenedTitle = -1;
    for (int track : reopenedTimeline->getTracksIds(false)) {
        const int id = reopenedTimeline->getClipByPosition(track, 0);
        if (id < 0) continue;
        const auto found = effectsById(reopenedTimeline->getClipEffectStack(id), QStringLiteral("sunimo_text_studio"));
        if (!found.isEmpty()) { reopenedScene = found.front()->getParam(QStringLiteral("0")); reopenedTitle = id; }
    }
    REQUIRE(reopenedScene == revisedScene);
    REQUIRE(QFileInfo::exists(reopenedScene));
    const auto reopenedSettings = SunimoTextQt::metadata(reopenedScene);
    REQUIRE(reopenedSettings.value(QStringLiteral("outlineEnabled")).toBool());
    REQUIRE(reopenedSettings.value(QStringLiteral("outlineWidth")).toInt() == 10);
    REQUIRE(reopenedSettings.value(QStringLiteral("outlineColor")).toString() == QStringLiteral("#aaff0000"));
    REQUIRE(reopenedSettings.value(QStringLiteral("text")).toString() == QStringLiteral("Новая надпись"));
    REQUIRE(reopenedSettings.value(QStringLiteral("inPreset")).toInt() == 72);
    REQUIRE(reopenedSettings.value(QStringLiteral("outPreset")).toInt() == 1);
    REQUIRE(reopenedSettings.value(QStringLiteral("backgroundEnabled")).toBool());
    REQUIRE(reopenedSettings.value(QStringLiteral("backgroundColor")).toString() == QStringLiteral("#800000ff"));
    REQUIRE(reopenedSettings.value(QStringLiteral("group")).toInt() == 0);
    REQUIRE(reopenedSettings.value(QStringLiteral("lifeSource")).toInt() == 72);
    REQUIRE(reopenedSettings.value(QStringLiteral("styleId")).toInt() == 7);
    REQUIRE(reopenedSettings.value(QStringLiteral("lifeAmount")).toDouble() == Approx(.85));
    REQUIRE(reopenedSettings.value(QStringLiteral("lifeSpeed")).toDouble() == Approx(1.6));
    REQUIRE(reopenedTitle >= 0);
    REQUIRE(reopenedTimeline->requestSetSelection({reopenedTitle}));
    {
        QHash<QString, QJsonObject> restoredDrafts;
        StudioTextPage restoredPage(&restoredDrafts);
        auto restoredStyle = restoredPage.findChild<QComboBox *>(QStringLiteral("studioTextStyle"));
        auto restoredAmount = restoredPage.findChild<QSpinBox *>(QStringLiteral("studioTextMotionAmount"));
        auto restoredSpeed = restoredPage.findChild<QDoubleSpinBox *>(QStringLiteral("studioTextMotionSpeed"));
        REQUIRE(restoredStyle); REQUIRE(restoredAmount); REQUIRE(restoredSpeed);
        REQUIRE(restoredStyle->currentData().toInt() == 7);
        REQUIRE(restoredAmount->value() == 85);
        REQUIRE(restoredSpeed->value() == Approx(1.6));
    }
    const QString savedAgain = folder.filePath(QStringLiteral("Надпись повторно.kdenlive"));
    reopenedSubtitles->copySubtitle(savedAgain + QStringLiteral(".ass"), 0, false, true);
    REQUIRE(pCore->projectManager()->testSaveFileAs(savedAgain));
    const QString copy = QDir(stage6Root.isEmpty() ? folder.path() : stage6Root).filePath(QStringLiteral("копия проекта/Надпись.kdenlive"));
    const QString temporarilyMissing = revisedScene + QStringLiteral(".missing");
    REQUIRE(QFile::rename(revisedScene, temporarilyMissing));
    REQUIRE_FALSE(reopened->copyStudioAssetsForSave(copy));
    REQUIRE(QFile::rename(temporarilyMissing, revisedScene));
    REQUIRE(reopened->copyStudioAssetsForSave(copy));
    const QString copiedScene = QFileInfo(copy).dir().filePath(QStringLiteral("studio-text/правка.stxt"));
    REQUIRE(QFileInfo::exists(copiedScene));
    QFile conflict(copiedScene);
    REQUIRE(conflict.open(QIODevice::WriteOnly | QIODevice::Truncate));
    REQUIRE(conflict.write("different-scene") == 15);
    conflict.close();
    REQUIRE_FALSE(reopened->copyStudioAssetsForSave(copy));
    REQUIRE(QFile::remove(copiedScene));
    REQUIRE(reopened->copyStudioAssetsForSave(copy));
    reopenedSubtitles->copySubtitle(copy + QStringLiteral(".ass"), 0, false, true);
    REQUIRE(pCore->projectManager()->testSaveFileAs(copy));
    QFile copiedProject(copy);
    REQUIRE(copiedProject.open(QIODevice::ReadOnly));
    const QByteArray copiedXmlContents = copiedProject.readAll();
    REQUIRE(copiedXmlContents.contains(copiedScene.toUtf8()));
    REQUIRE_FALSE(copiedXmlContents.contains(revisedScene.toUtf8()));
    pCore->projectManager()->closeCurrentDocument(false, false);
    const QByteArray firstExport = studioExportedTextFrames(saved);
    REQUIRE(firstExport.size() == renderWidth * renderHeight * 4 * 3);
    REQUIRE(firstExport == expectedExport);
    const QByteArray secondExport = studioExportedTextFrames(savedAgain);
    REQUIRE(firstExport == secondExport);
    REQUIRE(QFile::remove(revisedScene));
    REQUIRE(studioExportedTextFrames(copy) == firstExport);
    if (!stage6Root.isEmpty()) {
        QFile expected(copy + QStringLiteral(".sha256"));
        REQUIRE(expected.open(QIODevice::WriteOnly));
        REQUIRE(expected.write(QCryptographicHash::hash(firstExport, QCryptographicHash::Sha256).toHex()) == 64);
    }
    QDomDocument legacyRelativeProject;
    REQUIRE(legacyRelativeProject.setContent(copiedXmlContents));
    legacyRelativeProject.documentElement().setAttribute(QStringLiteral("root"), QFileInfo(copy).absolutePath());
    const QString legacyScene = QFileInfo(copy).dir().relativeFilePath(copiedScene);
    REQUIRE(QDir::isRelativePath(legacyScene));
    const auto legacyFilters = legacyRelativeProject.elementsByTagName(QStringLiteral("filter"));
    bool relativeReferenceWritten = false;
    for (int i = 0; i < legacyFilters.count(); ++i) {
        auto filter = legacyFilters.at(i).toElement();
        if (Xml::getXmlProperty(filter, QStringLiteral("mlt_service")) != QStringLiteral("frei0r.sunimo_text_studio")
            || Xml::getXmlProperty(filter, QStringLiteral("0")) != copiedScene) continue;
        Xml::setXmlProperty(filter, QStringLiteral("0"), legacyScene);
        relativeReferenceWritten = true;
    }
    REQUIRE(relativeReferenceWritten);
    copiedProject.close();
    REQUIRE(copiedProject.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray legacyXml = legacyRelativeProject.toByteArray();
    REQUIRE(copiedProject.write(legacyXml) == legacyXml.size());
    copiedProject.close();
    QUndoGroup copiedUndo;
    auto copiedOpen = KdenliveDoc::Open(QUrl::fromLocalFile(copy), QFileInfo(copy).absolutePath(), &copiedUndo, false, nullptr);
    REQUIRE(copiedOpen.isSuccessful());
    auto copiedDoc = copiedOpen.getDocument();
    pCore->projectManager()->testSetDocument(copiedDoc.get());
    const auto closeCopied = qScopeGuard([&copiedDoc] {
        if (pCore->currentDoc() == copiedDoc.get())
            pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(copy).lastModified(), 0));
    const auto copiedSequence = pCore->projectItemModel()->getAllSequenceClips().value(copiedDoc->uuid());
    pCore->projectManager()->openTimeline(copiedSequence, -1, copiedDoc->uuid());
    const auto copiedTimeline = copiedDoc->getTimeline(copiedDoc->uuid());
    pCore->projectManager()->testSetActiveTimeline(copiedTimeline);
    REQUIRE(pCore->projectManager()->getTimeline() == copiedTimeline);
    const auto copiedSubtitles = copiedTimeline->getSubtitleModel();
    REQUIRE(copiedSubtitles);
    REQUIRE(copiedSubtitles->rowCount() == 1);
    REQUIRE(copiedSubtitles->getText(*copiedSubtitles->getAllSubIds().begin()).contains(QStringLiteral("Исправленный субтитр")));
    REQUIRE(copiedSubtitles->getStyleName(*copiedSubtitles->getAllSubIds().begin()) == styledSubtitle);
    int copiedTitle = -1;
    for (int track : copiedTimeline->getTracksIds(false)) {
        const int id = copiedTimeline->getClipByPosition(track, 0);
        if (id >= 0 && !effectsById(copiedTimeline->getClipEffectStack(id), QStringLiteral("sunimo_text_studio")).isEmpty()) copiedTitle = id;
    }
    REQUIRE(copiedTitle >= 0);
    REQUIRE(effectsById(copiedTimeline->getClipEffectStack(copiedTitle), QStringLiteral("sunimo_text_studio")).front()->getParam(QStringLiteral("0")) == copiedScene);
    REQUIRE(copiedDoc->property("_studioTextAssetPaths").toMap().value(copiedScene).toString() == copiedScene);
    // The headless save helper does not move GUI cache data; prepare independent native storage explicitly.
    copiedDoc->setProjectFolder(QUrl::fromLocalFile(QFileInfo(copy).dir().filePath(QStringLiteral("cachefiles"))));
    bool cacheOk = false;
    const auto cache = copiedDoc->getCacheDir(CacheBase, &cacheOk);
    REQUIRE((cacheOk && cache.exists()));
    REQUIRE(pCore->projectManager()->testSaveFileAs(copy));
    const QString externalScene = folder.filePath(QStringLiteral("внешняя сцена.stxt"));
    REQUIRE(QFileInfo(externalScene).absolutePath() != QDir(copiedDoc->projectDataFolder()).absoluteFilePath(QStringLiteral("studio-text")));
    REQUIRE(QFileInfo(externalScene).absolutePath() != QDir(copiedDoc->documentRoot()).absoluteFilePath(QStringLiteral("others")));
    REQUIRE(QFile::copy(copiedScene, externalScene));
    const auto copiedTextEffect = effectsById(copiedTimeline->getClipEffectStack(copiedTitle), QStringLiteral("sunimo_text_studio")).front();
    REQUIRE(StudioText::updateTitle(copiedTimeline, copiedTitle, copiedTextEffect, 80, externalScene));
    REQUIRE(copiedTextEffect->getParam(QStringLiteral("0")) == externalScene);
    REQUIRE(copiedDoc->extractExternalEffectFiles().contains(externalScene));
    const QString externalCopy = QDir(stage6Root.isEmpty() ? folder.path() : stage6Root).filePath(QStringLiteral("копия внешней сцены/Надпись.kdenlive"));
    const QString relocatedScene = QFileInfo(externalCopy).dir().filePath(QStringLiteral("studio-text/внешняя сцена.stxt"));
    REQUIRE(copiedDoc->copyStudioAssetsForSave(externalCopy));
    copiedSubtitles->copySubtitle(externalCopy + QStringLiteral(".ass"), 0, false, true);
    REQUIRE(pCore->projectManager()->testSaveFileAs(externalCopy));
    REQUIRE(QFileInfo::exists(relocatedScene));
    QFile externalProject(externalCopy);
    REQUIRE(externalProject.open(QIODevice::ReadOnly));
    const QByteArray externalXml = externalProject.readAll();
    REQUIRE(externalXml.contains(relocatedScene.toUtf8()));
    REQUIRE_FALSE(externalXml.contains(externalScene.toUtf8()));
    externalProject.close();
    const QString relativeScene = QDir(copiedDoc->documentRoot()).relativeFilePath(externalScene);
    REQUIRE(QDir::isRelativePath(relativeScene));
    const QString customStorage = folder.filePath(QStringLiteral("отдельное хранилище"));
    REQUIRE(QDir().mkpath(customStorage));
    KdenliveSettings::setVideotodefaultfolder(KdenliveDoc::SaveToCustomFolder);
    KdenliveSettings::setVideofolder(customStorage);
    REQUIRE(copiedDoc->projectDataFolder(QFileInfo(externalCopy).absolutePath()) == customStorage);
    REQUIRE(QFileInfo(externalCopy).dir().relativeFilePath(customStorage).startsWith(QStringLiteral("../")));
    REQUIRE(QFile::remove(relocatedScene));
    REQUIRE(copiedDoc->copyStudioAssetsForSave(externalCopy));
    REQUIRE(QFileInfo::exists(relocatedScene));
    REQUIRE(pCore->projectManager()->testSaveFileAs(externalCopy));
    REQUIRE(externalProject.open(QIODevice::ReadOnly));
    const QByteArray customXml = externalProject.readAll();
    REQUIRE(customXml.contains(relocatedScene.toUtf8()));
    REQUIRE_FALSE(customXml.contains(relativeScene.toUtf8()));
    REQUIRE_FALSE(customXml.contains(externalScene.toUtf8()));
    {
        const QString unusedBinId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", pCore->projectItemModel(), 80, false);
        const auto unusedBin = pCore->projectItemModel()->getClipByBinID(unusedBinId);
        REQUIRE(unusedBin);
        REQUIRE_FALSE(unusedBin->isIncludedInTimeline());
        const auto binStack = unusedBin->getEffectStack();
        REQUIRE(binStack->appendEffect(QStringLiteral("sunimo_text_studio")));
        const auto binTextEffects = effectsById(binStack, QStringLiteral("sunimo_text_studio"));
        REQUIRE(binTextEffects.size() == 1);
        binTextEffects.front()->setParameter(QStringLiteral("0"), externalScene, true);
        REQUIRE(binTextEffects.front()->getParam(QStringLiteral("0")) == externalScene);
        {
            const auto binModel = pCore->projectItemModel();
            const auto binUndoStack = copiedDoc->commandStack();
            const int beforeBinDelete = binUndoStack->index();
            const QUrl previousBinUrl = copiedDoc->url();
            const QVariant previousBinRoot = copiedDoc->property("_studioTextSaveRoot");
            const QVariant previousBinPaths = copiedDoc->property("_studioTextAssetPaths");
            const bool previousBinEnabled = binStack->isStackEnabled();
            const QString hiddenBinSource = externalScene + QStringLiteral(".bin-hidden");
            Fun binDeleteUndo = [] { return true; }, binDeleteRedo = [] { return true; };
            const auto restoreBinSaveAs = qScopeGuard([&] {
                if (QFileInfo::exists(hiddenBinSource)) QFile::rename(hiddenBinSource, externalScene);
                binUndoStack->setIndex(beforeBinDelete);
                copiedDoc->setProperty("_studioTextSaveRoot", previousBinRoot);
                copiedDoc->setProperty("_studioTextAssetPaths", previousBinPaths);
                copiedTextEffect->setParameter(QStringLiteral("0"), externalScene, true);
                binTextEffects.front()->setParameter(QStringLiteral("0"), externalScene, true);
                binStack->setEffectStackEnabled(previousBinEnabled);
                copiedDoc->setUrl(previousBinUrl);
            });
            binStack->setEffectStackEnabled(false);
            QFile binSource(externalScene);
            REQUIRE(binSource.open(QIODevice::ReadOnly));
            const auto binSourceHash = QCryptographicHash::hash(binSource.readAll(), QCryptographicHash::Sha256);
            binSource.close();
            REQUIRE(binModel->requestBinClipDeletion(unusedBin, binDeleteUndo, binDeleteRedo));
            pCore->pushUndo(binDeleteUndo, binDeleteRedo, QStringLiteral("Удалить bin-клип"));
            REQUIRE(binUndoStack->index() == beforeBinDelete + 1);
            REQUIRE_FALSE(binModel->getClipByBinID(unusedBinId));
            const QString binCopy = folder.filePath(QStringLiteral("Undo удаления bin/Надпись.kdenlive"));
            const QString binCopyRoot = QFileInfo(binCopy).dir().filePath(QStringLiteral("studio-text"));
            const QString binCopiedScene = QDir(binCopyRoot).filePath(QFileInfo(externalScene).fileName());
            REQUIRE(copiedDoc->copyStudioAssetsForSave(binCopy));
            REQUIRE(pCore->projectManager()->testSaveFileAs(binCopy));
            QFile binAssetCopy(binCopiedScene);
            REQUIRE(binAssetCopy.open(QIODevice::ReadOnly));
            REQUIRE(QCryptographicHash::hash(binAssetCopy.readAll(), QCryptographicHash::Sha256) == binSourceHash);
            copiedDoc->setProperty("_studioTextSaveRoot", binCopyRoot);
            copiedDoc->updateStudioEffectAssetPaths({{externalScene, binCopiedScene}});
            copiedDoc->setUrl(QUrl::fromLocalFile(binCopy));
            binUndoStack->setClean();
            REQUIRE(QFile::rename(externalScene, hiddenBinSource));
            REQUIRE_FALSE(QFileInfo::exists(externalScene));
            binUndoStack->undo();
            REQUIRE(binUndoStack->index() == beforeBinDelete);
            REQUIRE(binModel->getClipByBinID(unusedBinId) == unusedBin);
            REQUIRE(binTextEffects.front()->getParam(QStringLiteral("0")) == binCopiedScene);
            REQUIRE_FALSE(binStack->isStackEnabled());
        }
        const QString cameraSource = QDir(customStorage).filePath(QStringLiteral("studio-tracks/голова тест.scam"));
        REQUIRE(QDir().mkpath(QFileInfo(cameraSource).absolutePath()));
        QFile cameraTrack(cameraSource);
        REQUIRE(cameraTrack.open(QIODevice::WriteOnly));
        const QByteArray cameraContents("SUNIMO_CAMERA_TRACK_V1\n0,0.5,0.5,1\n");
        REQUIRE(cameraTrack.write(cameraContents) == cameraContents.size());
        cameraTrack.close();
        REQUIRE(binStack->appendEffect(QStringLiteral("studio_camera")));
        const auto binCameraEffects = effectsById(binStack, QStringLiteral("studio_camera"));
        REQUIRE(binCameraEffects.size() == 1);
        binCameraEffects.front()->setParameter(QStringLiteral("tracking"), QStringLiteral("1"), true);
        binCameraEffects.front()->setParameter(QStringLiteral("track_path"), cameraSource, true);
        REQUIRE(copiedDoc->documentRoot() != customStorage);
        REQUIRE(copiedTextEffect->getParam(QStringLiteral("0")) == externalScene);
        REQUIRE(binTextEffects.front()->getParam(QStringLiteral("0")) == externalScene);
        const QString cameraOutput = QDir(customStorage).filePath(QStringLiteral("Проверка камеры.kdenlive"));
        REQUIRE(pCore->projectManager()->testSaveFileAs(cameraOutput));
        QFile cameraProject(cameraOutput);
        REQUIRE(cameraProject.open(QIODevice::ReadOnly));
        REQUIRE(cameraProject.readAll().contains(cameraSource.toUtf8()));
        // Contract setup only: native project Open normalizes relative references before serialization.
        copiedTextEffect->setParameter(QStringLiteral("0"), relativeScene, true);
        REQUIRE(copiedTextEffect->getParam(QStringLiteral("0")) == relativeScene);
        copiedDoc->updateStudioEffectAssetPaths({{QDir(copiedDoc->documentRoot()).absoluteFilePath(relativeScene), relocatedScene},
                                                 {externalScene, relocatedScene}});
        REQUIRE(binTextEffects.front()->getParam(QStringLiteral("0")) == relocatedScene);
        REQUIRE(copiedTimeline->getClipsCount() == 2);
    }
    REQUIRE(copiedTextEffect->getParam(QStringLiteral("0")) == relocatedScene);
    copiedDoc->setUrl(QUrl::fromLocalFile(externalCopy));
    pCore->projectManager()->closeCurrentDocument(false, false);
    REQUIRE(QFile::remove(externalScene));
    REQUIRE(studioExportedTextFrames(externalCopy) == firstExport);
}

TEST_CASE("Studio stage 6 copied project reopens after restart", "[StudioStage6][.persistent]")
{
    const QString project = qEnvironmentVariable("STUDIO_QA_STAGE6_PROJECT");
    INFO("This child case requires STUDIO_QA_STAGE6_PROJECT; use the parent restart case.");
    REQUIRE_FALSE(project.isEmpty());
    const QString previousProfile = pCore->getCurrentProfilePath();
    const auto restoreProfile = qScopeGuard([previousProfile] { pCore->setCurrentProfile(previousProfile); });
    REQUIRE(pCore->setCurrentProfile(QStringLiteral("atsc_1080p_60")));
    REQUIRE(QFileInfo::exists(project));
    pCore->projectItemModel()->clean();
    QUndoGroup undo;
    auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(project), QFileInfo(project).absolutePath(), &undo, false, nullptr);
    REQUIRE(opened.isSuccessful());
    auto document = opened.getDocument();
    pCore->projectManager()->testSetDocument(document.get());
    const auto closeDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(project).lastModified(), 0));
    const auto sequence = pCore->projectItemModel()->getAllSequenceClips().value(document->uuid());
    pCore->projectManager()->openTimeline(sequence, -1, document->uuid());
    const auto timeline = document->getTimeline(document->uuid());
    REQUIRE(timeline->getClipsCount() == 2);
    auto subtitles = timeline->getSubtitleModel();
    REQUIRE(subtitles);
    REQUIRE(subtitles->rowCount() == 1);
    REQUIRE(subtitles->getText(*subtitles->getAllSubIds().begin()).contains(QStringLiteral("Исправленный субтитр")));
    REQUIRE(subtitles->getStyleName(*subtitles->getAllSubIds().begin()) != QStringLiteral("Stage6-before"));
    const QString finalSubtitle = document->subTitlePath(document->uuid(), 0, true);
    REQUIRE(finalSubtitle == project + QStringLiteral(".ass"));
    QFile subtitleFile(finalSubtitle);
    REQUIRE(subtitleFile.open(QIODevice::ReadOnly));
    REQUIRE(subtitleFile.readAll().contains(QStringLiteral("Исправленный субтитр").toUtf8()));
    int titleId = -1;
    for (int track : timeline->getTracksIds(false)) {
        const int id = timeline->getClipByPosition(track, 0);
        if (id >= 0 && !effectsById(timeline->getClipEffectStack(id), QStringLiteral("sunimo_text_studio")).isEmpty()) titleId = id;
    }
    REQUIRE(titleId >= 0);
    const auto textEffects = effectsById(timeline->getClipEffectStack(titleId), QStringLiteral("sunimo_text_studio"));
    REQUIRE(textEffects.size() == 1);
    const QString scene = textEffects.front()->getParam(QStringLiteral("0"));
    REQUIRE(QFileInfo::exists(scene));
    REQUIRE(QDir(QFileInfo(project).absolutePath()).relativeFilePath(scene).startsWith(QStringLiteral("studio-text/")));
    const auto metadata = SunimoTextQt::metadata(scene);
    REQUIRE(metadata.value(QStringLiteral("text")).toString() == QStringLiteral("Новая надпись"));
    REQUIRE(metadata.value(QStringLiteral("outlineEnabled")).toBool());
    REQUIRE(metadata.value(QStringLiteral("outlineWidth")).toInt() == 10);
    REQUIRE(metadata.value(QStringLiteral("outlineColor")).toString() == QStringLiteral("#aaff0000"));
    REQUIRE(metadata.value(QStringLiteral("styleId")).toInt() == 7);
    REQUIRE(metadata.value(QStringLiteral("lifeAmount")).toDouble() == Approx(.85));
    REQUIRE(metadata.value(QStringLiteral("lifeSpeed")).toDouble() == Approx(1.6));
    REQUIRE(metadata.value(QStringLiteral("inPreset")).toInt() == 72);
    REQUIRE(metadata.value(QStringLiteral("outPreset")).toInt() == 1);
    REQUIRE(timeline->getClipPlaytime(titleId) == 80);
    REQUIRE(textEffects.front()->getParam(QStringLiteral("1")).toDouble() == Approx(80.0 / (pCore->getCurrentFps() * 120.0)));
    // Native export serializes the reopened model with this session's working ASS.
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QString renderScene = project + QStringLiteral(".render.mlt");
    QDomDocument snapshot;
    REQUIRE(snapshot.setContent(pCore->projectManager()->projectSceneList(QFileInfo(project).absolutePath()).first));
    const auto filters = snapshot.elementsByTagName(QStringLiteral("filter"));
    bool currentSubtitle = false;
    for (int i = 0; i < filters.size(); ++i) {
        const auto filter = filters.at(i).toElement();
        if (Xml::getXmlProperty(filter, QStringLiteral("mlt_service")) == QStringLiteral("avfilter.subtitles"))
            currentSubtitle = Xml::getXmlProperty(filter, QStringLiteral("av.filename")) == subtitles->getUrl();
    }
    REQUIRE(currentSubtitle);
    REQUIRE(QFileInfo::exists(subtitles->getUrl()));
    REQUIRE(Xml::docContentToFile(snapshot, renderScene));
    QFile expected(project + QStringLiteral(".sha256"));
    REQUIRE(expected.open(QIODevice::ReadOnly));
    REQUIRE(QCryptographicHash::hash(studioExportedTextFrames(renderScene), QCryptographicHash::Sha256).toHex() == expected.readAll());
    pCore->projectManager()->closeCurrentDocument(false, false);
}

TEST_CASE("Studio stage 6 Text and subtitles survive restart without original assets", "[StudioStage6][.external]")
{
    QTemporaryDir root(QDir(QDir::tempPath()).filePath(QStringLiteral("Этап 6 с пробелами-XXXXXX")));
    REQUIRE(root.isValid());
    root.setAutoRemove(false);
    INFO("Preserved stage 6 restart input: " << root.path().toStdString());
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("STUDIO_QA_STAGE6_ROOT"), root.path());
    QProcess first;
    first.setProcessEnvironment(environment);
    first.start(QCoreApplication::applicationFilePath(), {QStringLiteral("Studio Text creates an independent undoable title clip")});
    const bool firstFinished = first.waitForFinished(360000);
    if (!firstFinished) { first.kill(); first.waitForFinished(1000); }
    INFO(first.readAllStandardError().toStdString());
    INFO(first.readAllStandardOutput().toStdString());
    REQUIRE(firstFinished);
    REQUIRE(first.exitStatus() == QProcess::NormalExit);
    REQUIRE(first.exitCode() == 0);
    const QStringList folders = QDir(root.path()).entryList({QStringLiteral("source-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    REQUIRE(folders.size() == 1);
    const QString source = QDir(root.path()).filePath(folders.front());
    const QString copy = QDir(root.path()).filePath(QStringLiteral("копия проекта/Надпись.kdenlive"));
    REQUIRE(QFileInfo::exists(copy));
    REQUIRE(QDir(root.path()).rename(folders.front(), folders.front() + QStringLiteral("-unavailable")));
    REQUIRE_FALSE(QFileInfo::exists(source));
    QFile idFile(QDir(root.path()).filePath(QStringLiteral("documentid.txt")));
    REQUIRE(idFile.open(QIODevice::ReadOnly));
    const QString documentId = QString::fromUtf8(idFile.readAll()).trimmed();
    REQUIRE_FALSE(documentId.isEmpty());
    QStringList hiddenTemporarySubtitles;
    for (const QString &name : QDir(QDir::tempPath()).entryList({documentId + QStringLiteral("-*.ass")}, QDir::Files)) {
        const QString path = QDir(QDir::tempPath()).filePath(name);
        REQUIRE(QFile::rename(path, path + QStringLiteral(".unavailable")));
        hiddenTemporarySubtitles << path;
    }
    const auto restoreTemporarySubtitles = qScopeGuard([&hiddenTemporarySubtitles] {
        for (const QString &path : hiddenTemporarySubtitles) QFile::rename(path + QStringLiteral(".unavailable"), path);
    });
    environment.remove(QStringLiteral("STUDIO_QA_STAGE6_ROOT"));
    environment.insert(QStringLiteral("STUDIO_QA_STAGE6_PROJECT"), copy);
    QProcess reopened;
    reopened.setProcessEnvironment(environment);
    reopened.start(QCoreApplication::applicationFilePath(), {QStringLiteral("Studio stage 6 copied project reopens after restart")});
    const bool reopenedFinished = reopened.waitForFinished(180000);
    if (!reopenedFinished) { reopened.kill(); reopened.waitForFinished(1000); }
    INFO(reopened.readAllStandardError().toStdString());
    const QByteArray reopenedOutput = reopened.readAllStandardOutput();
    INFO(reopenedOutput.toStdString());
    REQUIRE(reopenedFinished);
    REQUIRE(reopened.exitStatus() == QProcess::NormalExit);
    REQUIRE(reopened.exitCode() == 0);
    REQUIRE(reopenedOutput.contains("All tests passed"));
    root.setAutoRemove(true);
}

TEST_CASE("Studio Text page keeps each clip draft and applies visible controls", "[StudioUI]")
{
    const QByteArray previousNative = qgetenv("STUDIO_TEXT_LIBRARY");
    if (!qgetenv("STUDIO_QA_TEXT_LIBRARY").isEmpty()) qputenv("STUDIO_TEXT_LIBRARY", qgetenv("STUDIO_QA_TEXT_LIBRARY"));
    const auto restoreNative = qScopeGuard([previousNative] {
        if (previousNative.isNull()) qunsetenv("STUDIO_TEXT_LIBRARY"); else qputenv("STUDIO_TEXT_LIBRARY", previousNative);
    });
    pCore->projectItemModel()->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const int previousLocation = KdenliveSettings::videotodefaultfolder();
    auto restoreSettings = qScopeGuard([previousLocation] { KdenliveSettings::setVideotodefaultfolder(previousLocation); });
    KdenliveSettings::setVideotodefaultfolder(KdenliveDoc::SaveToProjectFolder);
    document.m_sameProjectFolder = true;
    document.setProjectFolder(QUrl::fromLocalFile(folder.path()));
    document.setUrl(QUrl::fromLocalFile(folder.filePath(QStringLiteral("text-ui.kdenlive"))));
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([&document] {
        if (pCore->currentDoc() != &document) return;
        pCore->taskManager.slotCancelJobs();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const int track = timeline->getTrackIndexFromPosition(2);
    const QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", pCore->projectItemModel(), 120, false);
    int firstClip = -1, secondClip = -1;
    REQUIRE(timeline->requestClipInsertion(binId, track, 0, firstClip));
    REQUIRE(timeline->requestClipInsertion(binId, track, 130, secondClip));
    REQUIRE(timeline->requestSetSelection({firstClip}));

    QHash<QString, QJsonObject> drafts;
    StudioTextPage page(&drafts);
    page.show();
    auto input = page.findChild<QPlainTextEdit *>(QStringLiteral("studioTextInput"));
    auto apply = page.findChild<QPushButton *>(QStringLiteral("studioTextApply"));
    auto font = page.findChild<QFontComboBox *>(QStringLiteral("studioTextFont"));
    auto bold = page.findChild<QCheckBox *>(QStringLiteral("studioTextBold"));
    auto italic = page.findChild<QCheckBox *>(QStringLiteral("studioTextItalic"));
    auto outlineEnabled = page.findChild<QCheckBox *>(QStringLiteral("studioTextOutlineEnabled"));
    auto outlineWidth = page.findChild<QSpinBox *>(QStringLiteral("studioTextOutlineWidth"));
    auto opacity = page.findChild<QSpinBox *>(QStringLiteral("studioTextOpacity"));
    auto backgroundEnabled = page.findChild<QCheckBox *>(QStringLiteral("studioTextBackgroundEnabled"));
    auto letterSpacing = page.findChild<QSpinBox *>(QStringLiteral("studioTextLetterSpacing"));
    auto size = page.findChild<QSpinBox *>(QStringLiteral("studioTextSize"));
    auto width = page.findChild<QSpinBox *>(QStringLiteral("studioTextWidth"));
    auto x = page.findChild<QSpinBox *>(QStringLiteral("studioTextX"));
    auto y = page.findChild<QSpinBox *>(QStringLiteral("studioTextY"));
    auto group = page.findChild<QComboBox *>(QStringLiteral("studioTextGroup"));
    auto order = page.findChild<QComboBox *>(QStringLiteral("studioTextOrder"));
    auto style = page.findChild<QComboBox *>(QStringLiteral("studioTextStyle"));
    auto entrance = page.findChild<QComboBox *>(QStringLiteral("studioTextEntrance"));
    auto life = page.findChild<QComboBox *>(QStringLiteral("studioTextLife"));
    auto exit = page.findChild<QComboBox *>(QStringLiteral("studioTextExit"));
    auto lag = page.findChild<QSpinBox *>(QStringLiteral("studioTextLag"));
    auto amount = page.findChild<QSpinBox *>(QStringLiteral("studioTextMotionAmount"));
    auto speed = page.findChild<QDoubleSpinBox *>(QStringLiteral("studioTextMotionSpeed"));
    REQUIRE(input); REQUIRE(apply); REQUIRE(font); REQUIRE(bold); REQUIRE(italic); REQUIRE(outlineEnabled); REQUIRE(opacity);
    REQUIRE(outlineWidth); REQUIRE(backgroundEnabled); REQUIRE(letterSpacing); REQUIRE(size); REQUIRE(width); REQUIRE(x); REQUIRE(y);
    REQUIRE(group); REQUIRE(order); REQUIRE(lag); REQUIRE(amount); REQUIRE(speed);
    REQUIRE(style); REQUIRE(entrance); REQUIRE(life); REQUIRE(exit);
    REQUIRE(style->count() == 20);
    REQUIRE(entrance->findData(0) >= 0);
    REQUIRE(life->findData(-1) >= 0);
    REQUIRE(exit->findData(0) >= 0);
    REQUIRE_FALSE(apply->isEnabled());
    input->setPlainText(QStringLiteral("Проверка надписи"));
    style->setCurrentIndex(style->findData(-1));
    REQUIRE(entrance->currentData().toInt() == 0);
    REQUIRE(exit->currentData().toInt() == 0);
    REQUIRE(life->currentData().toInt() == 72);
    style->setCurrentIndex(style->findData(7));
    REQUIRE(entrance->currentData().toInt() == 72);
    REQUIRE(exit->currentData().toInt() == -1);
    REQUIRE(studioWait([&] { return !page.previewImage().isNull(); }, 10000));
    style->setCurrentIndex(style->findData(6));
    REQUIRE(page.previewImage().isNull());
    style->setCurrentIndex(style->findData(7));
    bold->setChecked(false); italic->setChecked(true); outlineEnabled->setChecked(true); outlineWidth->setValue(10);
    opacity->setValue(65);
    backgroundEnabled->setChecked(true); letterSpacing->setValue(3);
    size->setValue(50); width->setValue(70); x->setValue(20); y->setValue(30);
    group->setCurrentIndex(group->findData(0)); order->setCurrentIndex(2); lag->setValue(40);
    amount->setValue(85); speed->setValue(1.6);
    const QString chosenFont = font->currentFont().family();
    REQUIRE(apply->isEnabled());
    REQUIRE(timeline->requestSetSelection({secondClip}));
    page.refreshSelection();
    REQUIRE(input->toPlainText().isEmpty());
    REQUIRE(page.previewImage().isNull());
    REQUIRE_FALSE(apply->isEnabled());
    REQUIRE(timeline->requestSetSelection({firstClip}));
    page.refreshSelection();
    REQUIRE(input->toPlainText() == QStringLiteral("Проверка надписи"));
    REQUIRE_FALSE(bold->isChecked());
    REQUIRE(italic->isChecked());
    REQUIRE(outlineEnabled->isChecked());
    REQUIRE(outlineWidth->value() == 10);
    REQUIRE(backgroundEnabled->isChecked());
    REQUIRE(letterSpacing->value() == 3);
    REQUIRE(size->value() == 50);
    REQUIRE(width->value() == 70);
    REQUIRE(x->value() == 20);
    REQUIRE(y->value() == 30);
    REQUIRE(group->currentData().toInt() == 0);
    REQUIRE(order->currentIndex() == 2);
    REQUIRE(lag->value() == 40);
    REQUIRE(amount->value() == 85);
    REQUIRE(speed->value() == Approx(1.6));
    REQUIRE(style->currentData().toInt() == 7);

    auto seek = page.findChild<QSlider *>(QStringLiteral("studioTextSeek"));
    auto previewLabel = page.findChild<QLabel *>(QStringLiteral("studioTextPreview"));
    auto play = page.findChild<QPushButton *>(QStringLiteral("studioTextPlay"));
    REQUIRE(seek); REQUIRE(previewLabel); REQUIRE(play);
    const int draftUndo = undoStack->index();
    seek->setValue(500);
    REQUIRE(studioWait([&] { return !page.previewImage().isNull(); }, 10000));
    const QImage previewAtMiddle = page.previewImage();
    REQUIRE(previewAtMiddle.width() > 100);
    bool translucent = false;
    for (int y = 0; y < previewAtMiddle.height() && !translucent; ++y)
        for (int x = 0; x < previewAtMiddle.width(); ++x)
            translucent |= qAlpha(previewAtMiddle.pixel(x, y)) > 20 && qAlpha(previewAtMiddle.pixel(x, y)) < 250;
    REQUIRE(translucent);
    REQUIRE(undoStack->index() == draftUndo);
    play->click();
    REQUIRE(studioWait([&] { return previewLabel->property("studioRenderedFrames").toInt() > 1; }));
    page.hide();
    const int hiddenCount = previewLabel->property("studioRenderedFrames").toInt();
    QThread::msleep(220); QApplication::processEvents();
    REQUIRE(previewLabel->property("studioRenderedFrames").toInt() == hiddenCount);
    page.show(); seek->setValue(500);
    REQUIRE(studioWait([&] { return !page.previewImage().isNull(); }, 10000));

    QSettings savedStyles;
    const auto oldStyles = savedStyles.value(QStringLiteral("StudioText/customStyles"));
    const auto restoreStyles = qScopeGuard([oldStyles] {
        QSettings settings;
        if (oldStyles.isValid()) settings.setValue(QStringLiteral("StudioText/customStyles"), oldStyles);
        else settings.remove(QStringLiteral("StudioText/customStyles"));
    });
    auto styleTabs = page.findChild<QTabBar *>();
    auto saveStyle = page.findChild<QPushButton *>(QStringLiteral("studioTextSaveStyle"));
    auto customStyles = page.findChild<QComboBox *>(QStringLiteral("studioTextCustomStyles"));
    REQUIRE(styleTabs); REQUIRE(saveStyle); REQUIRE(customStyles);
    styleTabs->setCurrentIndex(1);
    QTimer::singleShot(0, [] {
        if (auto dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("Стиль проверки")); dialog->accept();
        }
    });
    saveStyle->click();
    REQUIRE(customStyles->findText(QStringLiteral("Стиль проверки")) >= 0);
    REQUIRE_FALSE(QSettings().value(QStringLiteral("StudioText/customStyles")).toByteArray().contains("Проверка надписи"));
    REQUIRE_FALSE(QSettings().value(QStringLiteral("StudioText/customStyles")).toByteArray().contains(folder.path().toUtf8()));
    QSettings().sync();
    QProcess restarted;
    restarted.start(QCoreApplication::applicationFilePath(), {QStringLiteral("Studio persisted text style is available")});
    if (!restarted.waitForFinished(30000)) { restarted.kill(); restarted.waitForFinished(1000); }
    INFO(restarted.readAllStandardError().toStdString());
    REQUIRE(restarted.exitStatus() == QProcess::NormalExit);
    REQUIRE(restarted.exitCode() == 0);
    {
        QHash<QString, QJsonObject> reopenedDrafts;
        StudioTextPage reopenedStyles(&reopenedDrafts);
        REQUIRE(reopenedStyles.findChild<QComboBox *>(QStringLiteral("studioTextCustomStyles"))
                ->findText(QStringLiteral("Стиль проверки")) >= 0);
    }

    const int beforeUndo = undoStack->index();
    apply->click();
    QElapsedTimer timer;
    timer.start();
    while (timeline->getClipsCount() < 3 && timer.elapsed() < 10000) QApplication::processEvents(QEventLoop::AllEvents, 50);
    REQUIRE(timeline->getClipsCount() == 3);
    REQUIRE(undoStack->index() == beforeUndo + 1);
    int titleId = -1;
    for (int id : timeline->getCurrentSelection()) if (timeline->isClip(id)) titleId = id;
    REQUIRE(titleId >= 0);
    const auto effects = effectsById(timeline->getClipEffectStack(titleId), QStringLiteral("sunimo_text_studio"));
    REQUIRE(effects.size() == 1);
    const auto values = SunimoTextQt::metadata(effects.front()->getParam(QStringLiteral("0")));
    REQUIRE(values.value(QStringLiteral("text")).toString() == QStringLiteral("Проверка надписи"));
    REQUIRE(values.value(QStringLiteral("font")).toString() == chosenFont);
    REQUIRE_FALSE(values.value(QStringLiteral("bold")).toBool());
    REQUIRE(values.value(QStringLiteral("italic")).toBool());
    REQUIRE(values.value(QStringLiteral("outlineEnabled")).toBool());
    REQUIRE(values.value(QStringLiteral("outlineWidth")).toInt() == 10);
    REQUIRE(values.value(QStringLiteral("opacity")).toInt() == 65);
    REQUIRE(values.value(QStringLiteral("backgroundEnabled")).toBool());
    REQUIRE(values.value(QStringLiteral("letterSpacing")).toInt() == 3);
    REQUIRE(values.value(QStringLiteral("size")).toInt() == 50);
    REQUIRE(values.value(QStringLiteral("width")).toInt() == 70);
    REQUIRE(values.value(QStringLiteral("x")).toInt() == 20);
    REQUIRE(values.value(QStringLiteral("y")).toInt() == 30);
    REQUIRE(values.value(QStringLiteral("group")).toInt() == 0);
    REQUIRE(values.value(QStringLiteral("order")).toInt() == 2);
    REQUIRE(values.value(QStringLiteral("lag")).toDouble() == Approx(.4));
    REQUIRE(values.value(QStringLiteral("lifeAmount")).toDouble() == Approx(.85));
    REQUIRE(values.value(QStringLiteral("lifeSpeed")).toDouble() == Approx(1.6));
    REQUIRE(values.value(QStringLiteral("styleId")).toInt() == 7);
    {
        const auto previousDrafts = drafts;
        const QString scenePath = effects.front()->getParam(QStringLiteral("0"));
        const QString offlinePath = scenePath + QStringLiteral(".offline");
        const int unchangedUndo = undoStack->index();
        const auto restoreScene = qScopeGuard([&] {
            if (QFile::exists(offlinePath)) { QFile::remove(scenePath); QFile::rename(offlinePath, scenePath); }
        });
        const auto hasError = [](const StudioTextPage &target, const QString &message) {
            const auto labels = target.findChildren<QLabel *>();
            return std::any_of(labels.cbegin(), labels.cend(), [&](QLabel *label) { return label->text().contains(message); });
        };
        REQUIRE(QFile::rename(scenePath, offlinePath));
        page.refreshSelection();
        REQUIRE(hasError(page, QStringLiteral("Файл надписи недоступен")));
        REQUIRE_FALSE(apply->isEnabled());
        QHash<QString, QJsonObject> failedDrafts;
        {
            StudioTextPage coldMissing(&failedDrafts);
            REQUIRE(hasError(coldMissing, QStringLiteral("Файл надписи недоступен")));
            coldMissing.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral("Не подменять пропавший файл"));
            REQUIRE_FALSE(coldMissing.findChild<QPushButton *>(QStringLiteral("studioTextApply"))->isEnabled());
            REQUIRE(hasError(coldMissing, QStringLiteral("Файл надписи недоступен")));
        }
        REQUIRE(failedDrafts.isEmpty());
        QFile corrupt(scenePath);
        REQUIRE(corrupt.open(QIODevice::WriteOnly));
        REQUIRE(corrupt.write("not an STXT scene") == 17);
        corrupt.close();
        page.refreshSelection();
        REQUIRE(hasError(page, QStringLiteral("не содержит читаемых настроек")));
        REQUIRE_FALSE(apply->isEnabled());
        REQUIRE(QFile::remove(scenePath));
        REQUIRE(QFile::rename(offlinePath, scenePath));
        page.refreshSelection();
        REQUIRE(input->toPlainText() == QStringLiteral("Проверка надписи"));
        REQUIRE(apply->isEnabled());
        input->setPlainText(QStringLiteral("Сохранить настоящий черновик"));
        REQUIRE(QFile::rename(scenePath, offlinePath));
        apply->click(); // File disappeared after selection, before refresh.
        REQUIRE(hasError(page, QStringLiteral("Файл надписи недоступен")));
        REQUIRE_FALSE(apply->isEnabled());
        REQUIRE(undoStack->index() == unchangedUndo);
        REQUIRE(effects.front()->getParam(QStringLiteral("0")) == scenePath);
        REQUIRE_FALSE(QFile::exists(scenePath));
        REQUIRE(QFile::rename(offlinePath, scenePath));
        page.refreshSelection();
        REQUIRE(input->toPlainText() == QStringLiteral("Сохранить настоящий черновик"));
        REQUIRE(apply->isEnabled());
        REQUIRE(undoStack->index() == unchangedUndo);
        input->setPlainText(QStringLiteral("Проверка надписи"));
        drafts = previousDrafts;
    }
    const QString nativePath = qEnvironmentVariable("STUDIO_QA_TEXT_LIBRARY", QStringLiteral("/app/lib/frei0r-1/sunimo_text_studio.so"));
    QLibrary native(nativePath);
    REQUIRE(native.load());
    auto createNative = reinterpret_cast<void *(*)()>(native.resolve("smt_create"));
    auto destroyNative = reinterpret_cast<void (*)(void *)>(native.resolve("smt_destroy"));
    auto loadNative = reinterpret_cast<int (*)(void *, const void *, size_t)>(native.resolve("smt_load"));
    auto renderNative = reinterpret_cast<int (*)(void *, double, unsigned, unsigned, const void *, void *)>(native.resolve("smt_render"));
    REQUIRE(createNative); REQUIRE(destroyNative); REQUIRE(loadNative); REQUIRE(renderNative);
    QFile sceneFile(effects.front()->getParam(QStringLiteral("0")));
    REQUIRE(sceneFile.open(QIODevice::ReadOnly));
    const QByteArray sceneBytes = sceneFile.readAll();
    void *nativeInstance = createNative();
    REQUIRE(nativeInstance);
    REQUIRE(loadNative(nativeInstance, sceneBytes.constData(), size_t(sceneBytes.size())) == 1);
    QImage exported(previewAtMiddle.size(), QImage::Format_RGBA8888);
    REQUIRE(renderNative(nativeInstance, 2., unsigned(exported.width()), unsigned(exported.height()), nullptr, exported.bits()) == 1);
    destroyNative(nativeInstance);
    REQUIRE(exported == previewAtMiddle);
    Mlt::Producer exportProducer(pCore->getProjectProfile(), "color", "#00000000");
    Mlt::Filter exportFilter(pCore->getProjectProfile(), "frei0r.sunimo_text_studio");
    REQUIRE(exportProducer.is_valid()); REQUIRE(exportFilter.is_valid());
    exportFilter.set("0", QFile::encodeName(sceneFile.fileName()).constData());
    exportProducer.attach(exportFilter);
    exportProducer.seek(qRound(2. * pCore->getCurrentFps()));
    std::unique_ptr<Mlt::Frame> exportFrame(exportProducer.get_frame());
    REQUIRE(exportFrame);
    auto exportFormat = mlt_image_rgba;
    int exportWidth = previewAtMiddle.width(), exportHeight = previewAtMiddle.height();
    const auto exportPixels = exportFrame->get_image(exportFormat, exportWidth, exportHeight);
    REQUIRE(exportPixels);
    REQUIRE(QImage(exportPixels, exportWidth, exportHeight, exportWidth * 4, QImage::Format_RGBA8888).copy() == previewAtMiddle);
    input->setPlainText(QStringLiteral("Изменённая надпись"));
    REQUIRE(apply->isEnabled());
    apply->click();
    REQUIRE(undoStack->index() == beforeUndo + 2);
    REQUIRE(SunimoTextQt::metadata(effects.front()->getParam(QStringLiteral("0"))).value(QStringLiteral("text")).toString()
            == QStringLiteral("Изменённая надпись"));
    undoStack->undo();
    page.refreshSelection();
    REQUIRE(input->toPlainText() == QStringLiteral("Проверка надписи"));
    REQUIRE(outlineEnabled->isChecked());
    REQUIRE(outlineWidth->value() == 10);
    REQUIRE(style->currentData().toInt() == 7);
    REQUIRE(amount->value() == 85);
    REQUIRE(timeline->getClipsCount() == 3);
    undoStack->undo();
    REQUIRE(timeline->getClipsCount() == 2);
    REQUIRE(timeline->requestSetSelection({firstClip}));
    page.refreshSelection();
    REQUIRE(input->toPlainText().isEmpty());
    REQUIRE(studioWait([] { return pCore->taskManager.backgroundIdle(); }));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

TEST_CASE("Studio persisted text style is available", "[StudioUI][.persistent]")
{
    QHash<QString, QJsonObject> drafts;
    StudioTextPage page(&drafts);
    auto custom = page.findChild<QComboBox *>(QStringLiteral("studioTextCustomStyles"));
    REQUIRE(custom);
    REQUIRE(custom->findText(QStringLiteral("Стиль проверки")) >= 0);
}

TEST_CASE("Studio subtitle preview uses the exported ASS style and MLT frames", "[StudioUI]")
{
    const QByteArray previousPrefix = qgetenv("STUDIO_PREFIX");
    const QByteArray qaPrefix = qgetenv("STUDIO_QA_PREFIX");
    if (!qaPrefix.isEmpty()) qputenv("STUDIO_PREFIX", qaPrefix);
    const auto restorePrefix = qScopeGuard([previousPrefix] {
        if (previousPrefix.isNull()) qunsetenv("STUDIO_PREFIX"); else qputenv("STUDIO_PREFIX", previousPrefix);
    });
    StudioSubtitlePage page;
    page.resize(440, 1000);
    page.show();
    auto input = page.findChild<QLineEdit *>(QStringLiteral("studioSubtitlePreviewText"));
    auto seek = page.findChild<QSlider *>(QStringLiteral("studioSubtitleSeek"));
    auto previewLabel = page.findChild<QLabel *>(QStringLiteral("studioSubtitlePreview"));
    auto outline = page.findChild<QSpinBox *>(QStringLiteral("studioSubtitle_outline"));
    auto background = page.findChild<QComboBox *>(QStringLiteral("studioSubtitle_background"));
    auto color = page.findChild<QPushButton *>(QStringLiteral("studioSubtitle_text_color"));
    auto play = page.findChild<QPushButton *>(QStringLiteral("studioSubtitlePlay"));
    REQUIRE(input); REQUIRE(seek); REQUIRE(previewLabel); REQUIRE(outline);
    REQUIRE(background); REQUIRE(color); REQUIRE(play);
    outline->setValue(5);
    background->setCurrentIndex(2);
    QTimer::singleShot(0, [] {
        if (auto dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget())) {
            dialog->setCurrentColor(QColor(255, 0, 0, 128)); dialog->accept();
        }
    });
    color->click();
    REQUIRE(studioWait([&] { return !page.previewImage().isNull(); }, 10000));
    QFile ass(page.previewAssPath());
    REQUIRE(ass.open(QIODevice::ReadOnly));
    const QByteArray script = ass.readAll();
    REQUIRE(script.contains("&H7F0000FF"));
    REQUIRE(script.count("Dialogue:") == 2); // separate box and outlined text
    const auto exactFrame = [&] {
        Mlt::Producer producer(pCore->getProjectProfile(), "color", "#182638");
        Mlt::Filter filter(pCore->getProjectProfile(), "avfilter.subtitles");
        REQUIRE(producer.is_valid()); REQUIRE(filter.is_valid());
        filter.set("av.filename", QFile::encodeName(page.previewAssPath()).constData());
        producer.attach(filter);
        producer.seek(qRound(seek->value() * 4. * pCore->getCurrentFps() / 1000.));
        std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
        REQUIRE(frame);
        auto format = mlt_image_rgba;
        int width = page.previewImage().width(), height = page.previewImage().height();
        const auto pixels = frame->get_image(format, width, height);
        REQUIRE(pixels);
        return QImage(pixels, width, height, width * 4, QImage::Format_RGBA8888).copy();
    };
    REQUIRE(page.previewImage() == exactFrame());
    background->setCurrentIndex(1);
    REQUIRE(page.previewImage().isNull());
    REQUIRE(studioWait([&] { return !page.previewImage().isNull(); }, 10000));
    background->setCurrentIndex(2);
    REQUIRE(page.previewImage().isNull());
    REQUIRE(studioWait([&] { return !page.previewImage().isNull(); }, 10000));
    QSettings savedStyles;
    const auto oldStyles = savedStyles.value(QStringLiteral("StudioSubtitles/customStyles"));
    const auto restoreStyles = qScopeGuard([oldStyles] {
        QSettings settings;
        if (oldStyles.isValid()) settings.setValue(QStringLiteral("StudioSubtitles/customStyles"), oldStyles);
        else settings.remove(QStringLiteral("StudioSubtitles/customStyles"));
    });
    auto styleTabs = page.findChild<QTabBar *>();
    auto saveStyle = page.findChild<QPushButton *>(QStringLiteral("studioSubtitleSaveStyle"));
    auto customStyles = page.findChild<QComboBox *>(QStringLiteral("studioSubtitleCustomStyles"));
    REQUIRE(styleTabs); REQUIRE(saveStyle); REQUIRE(customStyles);
    styleTabs->setCurrentIndex(1);
    QTimer::singleShot(0, [] {
        if (auto dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("Стиль субтитров проверки")); dialog->accept();
        }
    });
    saveStyle->click();
    REQUIRE(customStyles->findText(QStringLiteral("Стиль субтитров проверки")) >= 0);
    REQUIRE_FALSE(QSettings().value(QStringLiteral("StudioSubtitles/customStyles")).toByteArray().contains("Пример субтитров"));
    QSettings().sync();
    QProcess restarted;
    restarted.start(QCoreApplication::applicationFilePath(), {QStringLiteral("Studio persisted subtitle style is available")});
    if (!restarted.waitForFinished(30000)) { restarted.kill(); restarted.waitForFinished(1000); }
    INFO(restarted.readAllStandardError().toStdString());
    REQUIRE(restarted.exitStatus() == QProcess::NormalExit);
    REQUIRE(restarted.exitCode() == 0);
    seek->setValue(500);
    REQUIRE(studioWait([&] { return !page.previewImage().isNull(); }, 10000));
    REQUIRE(page.previewImage() == exactFrame());
    input->setText(QStringLiteral("Другой текст"));
    REQUIRE(page.previewImage().isNull());
    REQUIRE(studioWait([&] { return !page.previewImage().isNull(); }, 10000));
    REQUIRE(QFile(page.previewAssPath()).exists());
    play->click();
    const int before = previewLabel->property("studioRenderedFrames").toInt();
    REQUIRE(studioWait([&] { return previewLabel->property("studioRenderedFrames").toInt() > before; }));
    page.hide();
    const int hidden = previewLabel->property("studioRenderedFrames").toInt();
    QThread::msleep(220); QApplication::processEvents();
    REQUIRE(previewLabel->property("studioRenderedFrames").toInt() == hidden);

    QHash<QString, QJsonObject> drafts;
    StudioTextPage textPage(&drafts);
    textPage.show(); page.show();
    styleTabs->setCurrentIndex(0);
    auto presets = page.findChild<QComboBox *>(QStringLiteral("studioSubtitlePreset"));
    REQUIRE(presets);
    REQUIRE(presets->count() == 9); // custom settings and eight ready styles
    for (int index = 1; index <= 8; ++index) {
        INFO("subtitle preset=" << index);
        REQUIRE_FALSE(presets->itemText(index).isEmpty());
        presets->setCurrentIndex(index);
        presets->activated(index);
        REQUIRE(page.previewImage().isNull());
        REQUIRE(studioWait([&] { return !page.previewImage().isNull(); }, 10000));
        QFile styled(page.previewAssPath());
        REQUIRE(styled.open(QIODevice::ReadOnly));
        REQUIRE(styled.readAll().contains("Dialogue:"));
    }
    const auto checkWidth = [](QWidget &widget, int pixels) {
        const QByteArray expectedScale = qgetenv("STUDIO_QA_SCALE_FACTOR");
        if (!expectedScale.isEmpty()) REQUIRE(widget.devicePixelRatioF() == Approx(expectedScale.toDouble()));
        const int logical = qRound(pixels / widget.devicePixelRatioF());
        widget.resize(logical, 1400); QApplication::processEvents();
        REQUIRE(widget.width() <= logical + 1);
        for (auto child : widget.findChildren<QWidget *>()) {
            if (!child->isVisibleTo(&widget)) continue;
            if (!qobject_cast<QAbstractButton *>(child) && !qobject_cast<QComboBox *>(child)
                && !qobject_cast<QAbstractSpinBox *>(child) && !qobject_cast<QLineEdit *>(child)) continue;
            const int left = child->mapTo(&widget, QPoint(0, 0)).x();
            REQUIRE(left >= -1);
            REQUIRE(left + child->width() <= widget.width() + 1);
        }
    };
    for (int width : {300, 440}) { checkWidth(textPage, width); checkWidth(page, width); }
}

TEST_CASE("Studio persisted subtitle style is available", "[StudioUI][.persistent]")
{
    StudioSubtitlePage page;
    auto custom = page.findChild<QComboBox *>(QStringLiteral("studioSubtitleCustomStyles"));
    REQUIRE(custom);
    REQUIRE(custom->findText(QStringLiteral("Стиль субтитров проверки")) >= 0);
}

TEST_CASE("Studio effect model renders a decoded AV clip as a visible Card", "[StudioAVCard][.external]")
{
    const QString profilePath = GENERATE(QStringLiteral("atsc_1080p_25"), QStringLiteral("atsc_1080p_2997"));
    const QString previousProfile = pCore->getCurrentProfilePath();
    const auto restoreProfile = qScopeGuard([=] { pCore->setCurrentProfile(previousProfile); });
    REQUIRE(pCore->setCurrentProfile(profilePath));
    const int sourceIn = GENERATE(600, 0, 60, 1325);
    INFO("Card source in: " << sourceIn);
    const QString media = qEnvironmentVariable("STUDIO_QA_AV_MP4");
    const QString lowerMedia = qEnvironmentVariable("STUDIO_QA_CARD_LOWER_MP4");
    INFO("Set STUDIO_QA_AV_MP4 and STUDIO_QA_CARD_LOWER_MP4 to two readable AV MP4 files.");
    REQUIRE(QFileInfo(media).isFile());
    REQUIRE(QFileInfo(lowerMedia).isFile());
    pCore->projectItemModel()->clean();
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] {
        if (pCore->currentDoc()) {
            pCore->projectItemModel()->clean();
            pCore->projectManager()->closeCurrentDocument(false, false);
        }
    });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    const auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QByteArray path = QFile::encodeName(media);
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), path.constData());
    REQUIRE(producer->is_valid());
    REQUIRE(producer->get_length() >= sourceIn + 60);
    const auto bin = pCore->projectItemModel();
    const QByteArray lowerPath = QFile::encodeName(lowerMedia);
    auto lowerProducer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), lowerPath.constData());
    REQUIRE(lowerProducer->is_valid());
    REQUIRE(lowerProducer->get_length() >= 60);
    QString lowerBinId = QString::number(bin->getFreeClipId());
    Fun binUndo = [] { return true; }, binRedo = [] { return true; };
    REQUIRE(bin->requestAddBinClip(lowerBinId, lowerProducer, bin->getRootFolder()->clipId(), binUndo, binRedo));
    const int lowerTrack = timeline->getTrackIndexFromPosition(2);
    int lowerClipId = -1;
    REQUIRE(timeline->requestClipInsertion(lowerBinId, lowerTrack, 0, lowerClipId));
    REQUIRE(timeline->requestItemResize(lowerClipId, 60, true, true) == 60);
    const QByteArray lower15 = studioFrameHash(timeline, 15);
    const QByteArray lower45 = studioFrameHash(timeline, 45);
    QString binId = QString::number(bin->getFreeClipId());
    REQUIRE(bin->requestAddBinClip(binId, producer, bin->getRootFolder()->clipId(), binUndo, binRedo));
    auto binClip = bin->getClipByBinID(binId);
    REQUIRE(binClip);
    REQUIRE(binClip->hasVideo());
    REQUIRE(binClip->hasAudio());
    const int track = timeline->getTrackIndexFromPosition(3);
    int clipId = -1;
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/%2/%3").arg(binId).arg(sourceIn).arg(sourceIn + 59), track, 0, clipId));
    REQUIRE(timeline->requestItemResize(clipId, 60, true, true) == 60);
    REQUIRE(timeline->requestSetSelection({clipId}));
    const QByteArray baseline0 = studioFrameHash(timeline, 0);
    const QByteArray baseline15 = studioFrameHash(timeline, 15);
    const QByteArray baseline45 = studioFrameHash(timeline, 45);
    auto panel = std::make_unique<StudioPanel>();
    panel->resize(440, 700);
    panel->show();
    QApplication::processEvents();
    QPushButton *add = nullptr;
    for (auto button : panel->findChildren<QPushButton *>())
        if (button->property("studioPrimary").toBool() && button->text().startsWith(QStringLiteral("Добавить карточку"))) add = button;
    REQUIRE(add);
    REQUIRE(add->isEnabled());
    add->click();
    QApplication::processEvents();
    auto effects = effectsById(timeline->getClipEffectStack(clipId), QStringLiteral("card3d"));
    REQUIRE(effects.size() == 1);
    const auto card = effects.front();
    REQUIRE(card->getOwnerId().type == KdenliveObjectType::TimelineClip);
    REQUIRE(card->getOwnerId().itemId == clipId);
    REQUIRE(card->getOwnerId().uuid == timeline->uuid());
    REQUIRE(pCore->getItemDuration(card->getOwnerId()) == timeline->getClipPlaytime(clipId));
    REQUIRE(card->getParam(QStringLiteral("7")).toDouble() == Approx(1));
    REQUIRE(card->getParam(QStringLiteral("17")).toDouble() * 21600.0
            == Approx((timeline->getClipPlaytime(clipId) - 1) / pCore->getCurrentFps()).margin(1e-6));
    const double exitAt = card->getParam(QStringLiteral("17")).toDouble();
    const QByteArray card0 = studioFrameHash(timeline, 0);
    const QByteArray card15 = studioFrameAt(timeline, 15);
    const QByteArray card45 = studioFrameAt(timeline, 45);
    INFO("Card filter range: " << card->filter().get_in() << ".." << card->filter().get_out());
    INFO("Clip range: " << timeline->getClipIn(clipId) << " + " << timeline->getClipPlaytime(clipId));
    const auto visiblePixels = [](const QByteArray &frame) {
        int visible = 0;
        for (int i = 0; i < frame.size(); i += 4)
            visible += int(quint8(frame[i])) + int(quint8(frame[i + 1])) + int(quint8(frame[i + 2])) > 36;
        return visible;
    };
    CHECK(card0 != baseline0);
    CHECK(QCryptographicHash::hash(card15, QCryptographicHash::Sha256) != baseline15);
    CHECK(QCryptographicHash::hash(card45, QCryptographicHash::Sha256) != baseline45);
    CHECK(QCryptographicHash::hash(card15, QCryptographicHash::Sha256) != lower15);
    CHECK(QCryptographicHash::hash(card45, QCryptographicHash::Sha256) != lower45);
    REQUIRE(card->filter().get_in() == sourceIn);
    REQUIRE(card->filter().get_out() == sourceIn + 59);
    REQUIRE(visiblePixels(card15) > 1000);
    REQUIRE(visiblePixels(card45) > 1000);
    undoStack->undo();
    REQUIRE(effectsById(timeline->getClipEffectStack(clipId), QStringLiteral("card3d")).isEmpty());
    REQUIRE(studioFrameHash(timeline, 15) == baseline15);
    undoStack->redo();
    REQUIRE(effectsById(timeline->getClipEffectStack(clipId), QStringLiteral("card3d")).size() == 1);
    REQUIRE(studioFrameHash(timeline, 15) == QCryptographicHash::hash(card15, QCryptographicHash::Sha256));
    const QString saved = folder.filePath(QStringLiteral("видеокарточка.kdenlive"));
    REQUIRE(pCore->projectManager()->testSaveFileAs(saved));
    panel.reset();
    undoStack->clear();
    binUndo = {};
    binRedo = {};
    binClip.reset();
    pCore->projectItemModel()->clean();
    pCore->projectManager()->closeCurrentDocument(false, false);
    QUndoGroup reopenedUndo;
    auto opened = KdenliveDoc::Open(QUrl::fromLocalFile(saved), folder.path(), &reopenedUndo, false, nullptr);
    REQUIRE(opened.isSuccessful());
    auto reopened = opened.getDocument();
    pCore->projectManager()->testSetDocument(reopened.get());
    const auto clearReopened = qScopeGuard([] {
        pCore->projectItemModel()->clean();
        pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QFileInfo(saved).lastModified(), 0));
    const auto sequence = pCore->projectItemModel()->getAllSequenceClips().value(reopened->uuid());
    pCore->projectManager()->openTimeline(sequence, -1, reopened->uuid());
    const auto live = reopened->getTimeline(reopened->uuid());
    pCore->projectManager()->testSetActiveTimeline(live);
    const int reopenedClip = live->getClipByPosition(live->getTrackIndexFromPosition(3), 0);
    REQUIRE(reopenedClip >= 0);
    const auto restored = effectsById(live->getClipEffectStack(reopenedClip), QStringLiteral("card3d"));
    REQUIRE(restored.size() == 1);
    REQUIRE(restored.front()->getParam(QStringLiteral("7")).toDouble() == Approx(1));
    REQUIRE(restored.front()->getParam(QStringLiteral("17")).toDouble() == Approx(exitAt).margin(1e-12));
    REQUIRE(studioFrameHash(live, 0) == card0);
    REQUIRE(studioFrameHash(live, 15) == QCryptographicHash::hash(card15, QCryptographicHash::Sha256));
    REQUIRE(studioFrameHash(live, 45) == QCryptographicHash::hash(card45, QCryptographicHash::Sha256));
    const auto verifyCard = [&](int id) {
        const auto items = effectsById(live->getClipEffectStack(id), QStringLiteral("card3d"));
        REQUIRE(items.size() == 1);
        const auto item = items.front();
        REQUIRE(item->filter().get_in() == live->getClipIn(id));
        REQUIRE(item->filter().get_out() == live->getClipIn(id) + live->getClipPlaytime(id) - 1);
        REQUIRE(item->getParam(QStringLiteral("17")).toDouble() * 21600.0
                == Approx((live->getClipPlaytime(id) - 1) / pCore->getCurrentFps()).margin(1e-6));
        const int middle = live->getItemPosition(id) + live->getClipPlaytime(id) / 2;
        const auto framed = studioFrameHash(live, middle);
        item->filter().set("disable", 1);
        const auto unframed = studioFrameHash(live, middle);
        item->filter().set("disable", 0);
        REQUIRE(framed != unframed);
    };
    verifyCard(reopenedClip);
    auto edits = pCore->undoStack();
    REQUIRE(live->requestItemResize(reopenedClip, 50, true, true) == 50);
    verifyCard(reopenedClip);
    REQUIRE(live->requestItemResize(reopenedClip, 40, false, true) == 40);
    REQUIRE(live->getClipIn(reopenedClip) == sourceIn + 10);
    verifyCard(reopenedClip);
    edits->undo();
    verifyCard(reopenedClip);
    edits->undo();
    verifyCard(reopenedClip);
    const int reopenedTrack = live->getClipTrackId(reopenedClip);
    REQUIRE(live->requestClipMove(reopenedClip, reopenedTrack, 80));
    REQUIRE(live->getClipIn(reopenedClip) == sourceIn);
    verifyCard(reopenedClip);
    edits->undo();
    REQUIRE(TimelineFunctions::requestClipCut(live, reopenedClip, 30));
    const int split = live->getClipByPosition(reopenedTrack, 30);
    REQUIRE(split != reopenedClip);
    verifyCard(reopenedClip);
    verifyCard(split);
    edits->undo();
    verifyCard(reopenedClip);
    edits->redo();
    verifyCard(split);
    edits->undo();
    const QString copied = TimelineFunctions::copyClips(live, {reopenedClip}, reopenedClip);
    REQUIRE_FALSE(copied.isEmpty());
    REQUIRE(TimelineFunctions::pasteClips(live, copied, reopenedTrack, 120));
    const int pasted = live->getClipByPosition(reopenedTrack, 120);
    REQUIRE(pasted >= 0);
    REQUIRE(live->getClipIn(pasted) == sourceIn);
    verifyCard(pasted);
    edits->undo();
    REQUIRE_FALSE(live->isClip(pasted));
    edits->redo();
    verifyCard(pasted);
}

TEST_CASE("Studio analyzes every frame of a trimmed video", "[StudioBackgroundRange][.external]")
{
    const QString profilePath = GENERATE(QStringLiteral("atsc_1080p_25"), QStringLiteral("atsc_1080p_2997"));
    const QString previousProfile = pCore->getCurrentProfilePath();
    const auto restoreProfile = qScopeGuard([=] { pCore->setCurrentProfile(previousProfile); });
    REQUIRE(pCore->setCurrentProfile(profilePath));
    const int sourceIn = GENERATE(43, 1325);
    INFO("Background source in: " << sourceIn);
    const QString media = qEnvironmentVariable("STUDIO_QA_AV_MP4");
    REQUIRE(QFileInfo(media).isFile());
    const auto previousPrefix = qgetenv("STUDIO_PREFIX");
    if (!qgetenv("STUDIO_QA_PREFIX").isEmpty()) qputenv("STUDIO_PREFIX", qgetenv("STUDIO_QA_PREFIX"));
    const auto previousQuality = QSettings().value(QStringLiteral("StudioBackground/analysisSize"));
    QSettings().setValue(QStringLiteral("StudioBackground/analysisSize"), 512);
    const auto restoreEnvironment = qScopeGuard([=] {
        if (previousPrefix.isNull()) qunsetenv("STUDIO_PREFIX"); else qputenv("STUDIO_PREFIX", previousPrefix);
        if (previousQuality.isValid()) QSettings().setValue(QStringLiteral("StudioBackground/analysisSize"), previousQuality);
        else QSettings().remove(QStringLiteral("StudioBackground/analysisSize"));
    });
    pCore->projectItemModel()->clean();
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    document.setProjectFolder(QUrl::fromLocalFile(folder.path()));
    pCore->projectManager()->testSetDocument(&document);
    const auto clearDocument = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), QFile::encodeName(media).constData());
    REQUIRE(producer->is_valid());
    REQUIRE(producer->get_length() >= sourceIn + 103);
    const auto bin = pCore->projectItemModel();
    QString binId = QString::number(bin->getFreeClipId());
    Fun binUndo = [] { return true; }, binRedo = [] { return true; };
    REQUIRE(bin->requestAddBinClip(binId, producer, bin->getRootFolder()->clipId(), binUndo, binRedo));
    const int track = timeline->getTrackIndexFromPosition(2);
    int clipId = -1;
    REQUIRE(timeline->requestClipInsertion(QStringLiteral("%1/%2/%3").arg(binId).arg(sourceIn).arg(sourceIn + 102), track, 0, clipId));
    REQUIRE(timeline->requestSetSelection({clipId}));
    StudioPanel panel;
    panel.show();
    QToolButton *background = nullptr;
    for (auto button : panel.findChildren<QToolButton *>())
        if (button->property("studioNav").toBool() && button->text() == QStringLiteral("Фон")) background = button;
    REQUIRE(background);
    background->click();
    QTabBar *method = nullptr;
    for (auto tabs : panel.findChildren<QTabBar *>())
        if (tabs->count() == 2 && tabs->tabText(1) == QStringLiteral("Человек")) method = tabs;
    REQUIRE(method);
    method->setCurrentIndex(1);
    QPushButton *analyze = nullptr;
    for (auto button : panel.findChildren<QPushButton *>()) if (button->property("studioPrimary").toBool()) analyze = button;
    REQUIRE(analyze);
    REQUIRE(analyze->isEnabled());
    const int before = undoStack->index();
    analyze->click();
    QProcess *worker = nullptr, *reader = nullptr;
    for (auto process : panel.findChildren<QProcess *>()) {
        if (process->arguments().contains(QStringLiteral("--frames"))) worker = process;
        if (process->arguments().contains(QStringLiteral("avformat:pipe:1"))) reader = process;
    }
    REQUIRE(worker);
    REQUIRE(reader);
    const auto arguments = worker->arguments();
    REQUIRE(arguments.value(arguments.indexOf(QStringLiteral("--frames")) + 1).toInt() == 103);
    REQUIRE(reader->arguments().contains(QStringLiteral("in=%1").arg(sourceIn)));
    REQUIRE(reader->arguments().contains(QStringLiteral("out=%1").arg(sourceIn + 102)));
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 120000 && (worker->state() != QProcess::NotRunning || reader->state() != QProcess::NotRunning))
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    QApplication::processEvents();
    REQUIRE(worker->state() == QProcess::NotRunning);
    REQUIRE(reader->state() == QProcess::NotRunning);
    REQUIRE(worker->exitStatus() == QProcess::NormalExit);
    REQUIRE(worker->exitCode() == 0);
    REQUIRE(reader->exitStatus() == QProcess::NormalExit);
    REQUIRE(reader->exitCode() == 0);
    const auto effects = effectsById(timeline->getClipEffectStack(clipId), QStringLiteral("studio_background"));
    REQUIRE(effects.size() == 1);
    const auto mask = effects.front();
    const auto profile = pCore->getCurrentProfile().get();
    const StudioBackground::MaskRequest request{QString::fromUtf8(mask->filter().get("_sbg_mask_path")), media,
        mask->getParam(QStringLiteral("source_sha256")), mask->getParam(QStringLiteral("recipe_sha256")), 512,
        profile->frame_rate_num(), profile->frame_rate_den(), profile->width(), profile->height(), 0, 103, false};
    REQUIRE(StudioBackground::validateMask(request).isEmpty());
    REQUIRE(undoStack->index() == before + 1);
    undoStack->undo();
    REQUIRE(effectsById(timeline->getClipEffectStack(clipId), QStringLiteral("studio_background")).isEmpty());
    undoStack->redo();
    REQUIRE(effectsById(timeline->getClipEffectStack(clipId), QStringLiteral("studio_background")).size() == 1);
}

TEST_CASE("Imported SDR video automatically gets a verified proxy in the real editor", "[OptimizationGUI][.gui]")
{
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    folder.setAutoRemove(false); // Keep the real project, media and GUI cache as acceptance evidence.
    INFO(folder.path().toStdString());
    const QString media = folder.filePath(QStringLiteral("исходник.mp4"));
    QProcess encode;
    encode.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-filter_threads"), QStringLiteral("1"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("testsrc2=size=1280x720:rate=60"),
        QStringLiteral("-frames:v"), QStringLiteral("30"), QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-threads"), QStringLiteral("2"),
        QStringLiteral("-preset"), QStringLiteral("veryfast"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-x264-params"), QStringLiteral("colorprim=bt709:transfer=bt709:colormatrix=bt709"),
        QStringLiteral("-color_primaries"), QStringLiteral("bt709"), QStringLiteral("-colorspace"), QStringLiteral("bt709"),
        QStringLiteral("-color_trc"), QStringLiteral("bt709"), media});
    REQUIRE(encode.waitForFinished(30000)); INFO(encode.readAllStandardError().toStdString()); REQUIRE(encode.exitCode() == 0);
    const QString previousFfmpeg = KdenliveSettings::ffmpegpath();
    KdenliveSettings::setFfmpegpath(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")));
    const auto restoreFfmpeg = qScopeGuard([&] { KdenliveSettings::setFfmpegpath(previousFfmpeg); });
    const QString previousProfile = pCore->getCurrentProfilePath();
    REQUIRE(pCore->setCurrentProfile(QStringLiteral("atsc_1080p_60")));
    const auto restoreProfile = qScopeGuard([&] { pCore->setCurrentProfile(previousProfile); });
    pCore->projectItemModel()->clean();
    auto undo = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undo);
    document.setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    document.setProjectFolder(QUrl::fromLocalFile(folder.path()));
    pCore->projectManager()->testSetDocument(&document);
    const auto clean = qScopeGuard([] { pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    pCore->projectManager()->testSetActiveTimeline(document.getTimeline(document.uuid()));
    const QString project = folder.filePath(QStringLiteral("проект.kdenlive"));
    REQUIRE(pCore->projectManager()->testSaveFileAs(project));
    const QString emptyProject = folder.filePath(QStringLiteral("empty.kdenlive"));
    REQUIRE(QFile::copy(project, emptyProject));
    const auto bus = QDBusConnection::sessionBus();
    REQUIRE(bus.isConnected());
    QProcess editor;
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &name : {QStringLiteral("CONFIG"), QStringLiteral("CACHE"), QStringLiteral("DATA")}) {
        const QString path = folder.filePath(name.toLower()); REQUIRE(QDir().mkpath(path));
        environment.insert(QStringLiteral("XDG_%1_HOME").arg(name), path);
    }
    editor.setProcessEnvironment(environment);
    editor.setProcessChannelMode(QProcess::MergedChannels);
    const auto stopEditor = qScopeGuard([&] { editor.kill(); editor.waitForFinished(2000); });
    QString proxy;
    for (int phase = 0; phase < 3; ++phase) {
        INFO("GUI phase: " << phase << " (import, reopen, corrupt cache)");
        if (phase == 2) {
            QFile damaged(proxy);
            REQUIRE(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
            REQUIRE(damaged.write("corrupt proxy cache") > 0);
            damaged.close();
            REQUIRE(QFile::remove(project));
            REQUIRE(QFile::copy(emptyProject, project));
        }
        const QStringList previousServices = bus.interface()->registeredServiceNames().value();
        QStringList arguments{QStringLiteral("--no-welcome")};
        if (phase != 1) arguments << QStringLiteral("-i") << media;
        arguments << project;
        editor.start(QCoreApplication::applicationDirPath() + QStringLiteral("/kdenlive"), arguments);
        QString service;
        REQUIRE(studioWait([&] {
            for (const auto &name : bus.interface()->registeredServiceNames().value())
                if (name.startsWith(QLatin1String("local.VideoStudio.Kdenlive")) && !previousServices.contains(name)) service = name;
            return !service.isEmpty() || editor.state() == QProcess::NotRunning;
        }, 30000));
        INFO(editor.peek(editor.bytesAvailable()).toStdString());
        REQUIRE_FALSE(service.isEmpty());
        QDBusInterface window(service, QStringLiteral("/kdenlive/MainWindow_1"), QStringLiteral("org.kde.KMainWindow"), bus);
        REQUIRE(window.isValid());
        QDBusInterface widget(service, QStringLiteral("/kdenlive/MainWindow_1"), QStringLiteral("org.qtproject.Qt.QWidget"), bus);
        proxy.clear();
        const bool proxyReady = studioWait([&] {
            // The DBus object exists before the startup project has finished loading.
            if (!widget.property("windowTitle").toString().contains(QStringLiteral("проект"))) return false;
            window.call(QStringLiteral("activateAction"), QStringLiteral("file_save"));
            QFile file(project); if (!file.open(QIODevice::ReadOnly)) return false;
            QDomDocument saved; if (!saved.setContent(file.readAll())) return false;
            const auto producers = saved.elementsByTagName(QStringLiteral("chain"));
            const QDir projectDir(QFileInfo(project).absolutePath());
            for (int i = 0; i < producers.count(); ++i) {
                const auto clip = producers.at(i).toElement();
                const QString candidate = projectDir.absoluteFilePath(Xml::getXmlProperty(clip, QStringLiteral("kdenlive:proxy")));
                if (projectDir.absoluteFilePath(Xml::getXmlProperty(clip, QStringLiteral("kdenlive:originalurl"))) == media
                    && projectDir.absoluteFilePath(Xml::getXmlProperty(clip, QStringLiteral("resource"))) == candidate) proxy = candidate;
            }
            return QFileInfo(proxy).isFile() && QFileInfo(proxy).size() > 1000;
        }, 60000);
        if (!proxyReady) folder.setAutoRemove(false);
        INFO(folder.path().toStdString());
        INFO(editor.readAll().toStdString());
        REQUIRE(proxyReady);
        REQUIRE(QFileInfo(proxy).isFile());
        Mlt::Producer decoded(pCore->getProjectProfile(), QFile::encodeName(proxy).constData());
        REQUIRE(decoded.is_valid());
        REQUIRE(decoded.get_int("meta.media.width") == 640);
        QAtomicInt canceled(0);
        qint64 frames = 0;
        REQUIRE(StudioOptimization::constantFrameRate(QStringLiteral("ffprobe"), proxy, 0, 60, canceled, &frames));
        REQUIRE(frames == 30);
        REQUIRE(widget.call(QStringLiteral("close")).type() != QDBusMessage::ErrorMessage);
        REQUIRE(editor.waitForFinished(15000));
        REQUIRE(editor.exitCode() == 0);
    }
}

TEST_CASE("Preview tracks survive timeline track changes", "[Studio][Optimization][PreviewTrackLifecycle]")
{
    pCore->projectItemModel()->clean();
    QTemporaryDir cache;
    auto undo = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undo);
    document.setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    document.setProjectFolder(QUrl::fromLocalFile(cache.path()));
    pCore->projectManager()->testSetDocument(&document);
    const auto clean = qScopeGuard([] { pCore->taskManager.slotCancelJobs(); pCore->projectItemModel()->clean(); pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    const auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    timeline->initializePreviewManager();
    REQUIRE(timeline->buildPreviewTrack());
    const auto preview = timeline->previewManager();
    const int tracks = timeline->getTracksCount();
    REQUIRE(timeline->requestTrackDeletion(timeline->getTrackIndexFromPosition(tracks - 1)));
    // Deleting a user track shifts the preview away from its cached index.
    preview->disconnectTrack();
    REQUIRE(timeline->tractor()->count() == tracks);
    preview->reconnectTrack();
    REQUIRE(timeline->getTracksCount() == tracks - 1);
    undo->undo();
    preview->disconnectTrack();
    REQUIRE(timeline->tractor()->count() == tracks + 1);
    preview->reconnectTrack();
    REQUIRE(timeline->getTracksCount() == tracks);
    // Adding a track puts an ordinary, untagged track at the old preview index.
    int added = -1;
    REQUIRE(timeline->requestTrackInsertion(-1, added));
    preview->disconnectTrack();
    REQUIRE(timeline->tractor()->count() == tracks + 2);
    preview->disconnectTrack();
    REQUIRE(timeline->tractor()->count() == tracks + 2);
    preview->reconnectTrack();
    REQUIRE(timeline->getTracksCount() == tracks + 1);
    REQUIRE(timeline->isTrack(added));
}

TEST_CASE("Automatic native preview covers the cursor window at quarter size without changing Undo or export XML", "[Studio][Optimization]")
{
    pCore->projectItemModel()->clean();
    QTemporaryDir cache;
    REQUIRE(cache.isValid());
    auto undo = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undo);
    document.setDocumentProperty(QStringLiteral("documentid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    document.setProjectFolder(QUrl::fromLocalFile(cache.path()));
    pCore->projectManager()->testSetDocument(&document);
    const auto clean = qScopeGuard([] { pCore->taskManager.slotCancelJobs(); pCore->projectItemModel()->clean(); pCore->projectManager()->closeCurrentDocument(false, false); });
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    const auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const auto bin = pCore->projectItemModel();
    const QString id = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", bin, 1000, false);
    int clip = -1;
    REQUIRE(timeline->requestClipInsertion(id, timeline->getTrackIndexFromPosition(2), 0, clip));
    const int before = undo->index();
    const int width = pCore->getProjectProfile().width(), height = pCore->getProjectProfile().height();
    const QString renderer = KdenliveSettings::kdenliverendererpath();
    document.setDocumentProperty(QStringLiteral("resizepreview"), QStringLiteral("1"));
    document.setDocumentProperty(QStringLiteral("previewheight"), QStringLiteral("360"));
    KdenliveSettings::setKdenliverendererpath(QStandardPaths::findExecutable(QStringLiteral("kdenlive_render")));
    const auto restoreRenderer = qScopeGuard([&] { KdenliveSettings::setKdenliverendererpath(renderer); });
    timeline->initializePreviewManager();
    REQUIRE(timeline->hasTimelinePreview());
    const auto preview = timeline->previewManager();
    preview->requestAutomaticPreview(500, timeline->duration());
    REQUIRE(undo->index() == before);
    REQUIRE(studioWait([&] { return !preview->isRunning() && !preview->previewChunks().first.isEmpty(); }, 45000));
    const int radius = qRound(pCore->getCurrentFps() * 5), size = KdenliveSettings::timelinechunks();
    const auto rendered = [&] {
        QList<int> result;
        for (const auto &range : preview->previewChunks().first) {
            const auto ends = range.split(QLatin1Char('-'));
            for (int frame = ends.first().toInt(); frame <= ends.last().toInt(); frame += size) result.append(frame);
        }
        return result;
    };
    const auto chunks = rendered();
    for (const auto &chunk : chunks) {
        REQUIRE(chunk + size > 500 - radius);
        REQUIRE(chunk <= 500 + radius);
    }
    const QString file = preview->getCacheDir().absoluteFilePath(QString::number(chunks.first()) + QStringLiteral(".mkv"));
    QProcess probe;
    probe.start(QStringLiteral("ffprobe"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"), QStringLiteral("v:0"),
        QStringLiteral("-show_entries"), QStringLiteral("stream=width,height,avg_frame_rate"), QStringLiteral("-of"), QStringLiteral("csv=p=0"), file});
    REQUIRE(probe.waitForFinished(30000));
    INFO(probe.readAllStandardError().toStdString()); REQUIRE(probe.exitCode() == 0);
    const QByteArray dimensions = probe.readAllStandardOutput(); INFO(dimensions.toStdString());
    const QByteArray expectedDimensions = QByteArray::number(width / 4) + ',' + QByteArray::number(height / 4) + ',';
    REQUIRE(dimensions.startsWith(expectedDimensions));
    REQUIRE(pCore->getProjectProfile().width() == width);
    REQUIRE(pCore->getProjectProfile().height() == height);
    REQUIRE(undo->index() == before);
    preview->invalidatePreview(500, 501);
    REQUIRE_FALSE(rendered().contains(500 / size * size));
    preview->requestAutomaticPreview(500, timeline->duration());
    preview->abortRendering();
    REQUIRE(studioWait([&] { return !preview->isRunning() && pCore->taskManager.backgroundIdle(); }));
    QTemporaryDir folder;
    const QString project = folder.filePath(QStringLiteral("проект.kdenlive"));
    // The model-only save helper omits the GUI's native preview disconnection.
    preview->disconnectTrack();
    const auto reconnect = qScopeGuard([&] { preview->reconnectTrack(); });
    REQUIRE(pCore->projectManager()->testSaveFileAs(project));
    QFile saved(project); REQUIRE(saved.open(QIODevice::ReadOnly)); const QByteArray scene = saved.readAll();
    REQUIRE_FALSE(scene.contains("timeline_preview"));
    REQUIRE(scene.contains("sunimo:optimizationDefaultsVersion"));
}

TEST_CASE("Studio panel follows the host palette when its theme changes", "[StudioUI]")
{
    const QPalette original = QApplication::palette();
    const auto restore = qScopeGuard([&] { QApplication::setPalette(original); });
    StudioPanel panel;
    panel.resize(440, 700);
    panel.show();
    QApplication::processEvents();
    for (const QColor &background : {QColor(35, 38, 41), QColor(239, 240, 241), QColor(35, 38, 41)}) {
        QPalette next = original;
        next.setColor(QPalette::Window, background);
        next.setColor(QPalette::Base, background);
        next.setColor(QPalette::Midlight, background);
        next.setColor(QPalette::Text, background.lightness() < 100 ? Qt::white : Qt::black);
        next.setColor(QPalette::WindowText, next.color(QPalette::Text));
        QApplication::setPalette(next);
        QApplication::processEvents();
        REQUIRE(panel.palette().color(QPalette::Window) == background);
        auto *target = panel.findChild<QLabel *>(QStringLiteral("studioTarget"));
        REQUIRE(target);
        REQUIRE(target->palette().color(QPalette::Base) == background);
        const QImage targetImage = target->grab().toImage();
        REQUIRE_FALSE(targetImage.isNull());
        REQUIRE(targetImage.pixelColor(targetImage.width() - 4, targetImage.height() - 4).lightness()
                == Approx(background.lightness()).margin(2));
        const QImage image = panel.grab().toImage();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.pixelColor(2, 2).lightness() == Approx(background.lightness()).margin(2));
    }
}

TEST_CASE("Eight Studio categories fit supported panel widths", "[StudioUI]")
{
    const QByteArray previousPrefix = qgetenv("STUDIO_PREFIX");
    if (!qgetenv("STUDIO_QA_PREFIX").isEmpty()) qputenv("STUDIO_PREFIX", qgetenv("STUDIO_QA_PREFIX"));
    const auto restorePrefix = qScopeGuard([previousPrefix] {
        if (previousPrefix.isNull()) qunsetenv("STUDIO_PREFIX"); else qputenv("STUDIO_PREFIX", previousPrefix);
    });
    StudioPanel panel;
    const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1"));
    const QString output = qEnvironmentVariable("STUDIO_UI_OUTPUT", QStringLiteral("studio-ui")) + QStringLiteral("/scale-") + scale;
    REQUIRE(QDir().mkpath(output));
    for (int width : {300, 440, 800}) {
        panel.resize(width, 700);
        panel.show();
        for (const auto &name : {QStringLiteral("Карточки"), QStringLiteral("Камера"), QStringLiteral("Фон"),
                                 QStringLiteral("Переходы"), QStringLiteral("Эффекты"), QStringLiteral("Цвет"),
                                 QStringLiteral("Звук"), QStringLiteral("Текст")}) {
            QToolButton *navigation = nullptr;
            for (auto button : panel.findChildren<QToolButton *>())
                if (button->property("studioNav").toBool() && button->text() == name) navigation = button;
            REQUIRE(navigation);
            REQUIRE(navigation->isVisible());
            navigation->click();
            QApplication::processEvents();
            if (name == QStringLiteral("Текст")) {
                auto page = panel.findChild<QWidget *>(QStringLiteral("studioTextPage"));
                REQUIRE(page);
                int catalogCount = -1;
                for (auto combo : page->findChildren<QComboBox *>())
                    if (combo->accessibleName() == QStringLiteral("Вариант появления")) catalogCount = combo->count();
                REQUIRE(catalogCount == 101); // 100 recipes plus "Без появления"
            }
            int navBottom = 0;
            for (auto button : panel.findChildren<QToolButton *>())
                if (button->property("studioNav").toBool() && button->isVisible())
                    navBottom = qMax(navBottom, button->mapTo(&panel, QPoint(0, button->height())).y());
            auto title = panel.findChild<QWidget *>(QStringLiteral("studioPageTitle"));
            REQUIRE(title);
            REQUIRE(title->y() - navBottom <= 32);
            if (name == QStringLiteral("Переходы")) {
                QSlider *dip = nullptr;
                QSlider *level = nullptr;
                for (auto slider : panel.findChildren<QSlider *>())
                    if (slider->accessibleName() == QStringLiteral("Приглушение звука на стыке")) dip = slider;
                    else if (slider->accessibleName() == QStringLiteral("Громкость звука")) level = slider;
                REQUIRE(dip);
                REQUIRE(dip->value() == 25);
                REQUIRE_FALSE(dip->isVisible()); // no linked audio is selected
                QCheckBox *sound = nullptr;
                for (auto check : panel.findChildren<QCheckBox *>())
                    if (check->accessibleName() == QStringLiteral("Звук перехода")) sound = check;
                REQUIRE(sound);
                REQUIRE_FALSE(sound->isChecked());
                REQUIRE(level);
                REQUIRE(level->value() == 50);
                REQUIRE(level->parentWidget()->isHidden());
                QComboBox *source = nullptr;
                for (auto combo : panel.findChildren<QComboBox *>())
                    if (combo->accessibleName() == QStringLiteral("Выбрать звук")) source = combo;
                REQUIRE(source);
                REQUIRE(source->count() == 17); // automatic plus all 16 bundled WAVs
                REQUIRE(source->itemText(0) == QStringLiteral("Автоматически: звук этого перехода"));
                REQUIRE(source->currentData().toString().isEmpty());
                REQUIRE(source->parentWidget()->isHidden());
                QPushButton *browse = nullptr;
                for (auto button : panel.findChildren<QPushButton *>())
                    if (button->accessibleName() == QStringLiteral("Добавить свой звук перехода")) browse = button;
                REQUIRE(browse);
                sound->setChecked(true);
                REQUIRE_FALSE(level->parentWidget()->isHidden());
                REQUIRE_FALSE(source->parentWidget()->isHidden());
                REQUIRE_FALSE(browse->isHidden());
                for (auto scroll : panel.findChildren<QScrollArea *>())
                    if (scroll->isVisible()) REQUIRE(scroll->widget()->minimumSizeHint().width() <= scroll->viewport()->width());
                sound->setChecked(false);
                REQUIRE(level->parentWidget()->isHidden());
                REQUIRE(source->parentWidget()->isHidden());
            }
            if (name == QStringLiteral("Звук")) {
                const auto noise = panel.findChild<QComboBox *>(QStringLiteral("studioNoise"));
                REQUIRE(noise);
                REQUIRE(noise->isVisible());
                REQUIRE(noise->currentData().toString() == QLatin1String("fft"));
            }
            if (name == QStringLiteral("Текст")) {
                const auto page = panel.findChild<QWidget *>(QStringLiteral("studioTextPage"));
                REQUIRE(page);
                const auto input = page->findChild<QPlainTextEdit *>(QStringLiteral("studioTextInput"));
                REQUIRE(input);
                REQUIRE(input->isVisible());
                REQUIRE(input->height() >= 78);
                const auto font = page->findChild<QFontComboBox *>(QStringLiteral("studioTextFont"));
                REQUIRE(font);
                REQUIRE(font->isVisible());
                REQUIRE(font->height() >= font->minimumSizeHint().height());
                REQUIRE(page->findChild<QCheckBox *>(QStringLiteral("studioTextBold")));
                REQUIRE(page->findChild<QSpinBox *>(QStringLiteral("studioTextWidth")));
                REQUIRE(page->findChild<QSpinBox *>(QStringLiteral("studioTextX")));
                REQUIRE(page->findChild<QSpinBox *>(QStringLiteral("studioTextY")));
                QTabBar *tabs = nullptr;
                for (auto bar : panel.findChildren<QTabBar *>())
                    if (bar->count() == 2 && bar->tabText(1) == QStringLiteral("Субтитры")) tabs = bar;
                REQUIRE(tabs);
                tabs->setCurrentIndex(1);
                QApplication::processEvents();
                const auto subtitleSize = panel.findChild<QWidget *>(QStringLiteral("studioSubtitle_font_size"));
                REQUIRE(subtitleSize);
                REQUIRE(subtitleSize->height() >= subtitleSize->minimumSizeHint().height());
                REQUIRE(panel.findChild<QWidget *>(QStringLiteral("studioSubtitle_animation")));
                for (auto scroll : panel.findChildren<QScrollArea *>())
                    if (scroll->isVisible()) REQUIRE(scroll->widget()->minimumSizeHint().width() <= scroll->viewport()->width());
                REQUIRE(panel.grab().save(output + QStringLiteral("/%1-Субтитры.png").arg(width)));
                tabs->setCurrentIndex(0);
                QApplication::processEvents();
            }
            INFO(name.toStdString());
            INFO(width);
            REQUIRE(panel.width() == width);
            for (auto scroll : panel.findChildren<QScrollArea *>())
                if (scroll->isVisible()) REQUIRE(scroll->widget()->minimumSizeHint().width() <= scroll->viewport()->width());
            REQUIRE(panel.grab().save(output + QStringLiteral("/%1-%2.png").arg(width).arg(name)));
        }
        panel.hide();
    }
}

#include "public_demo.hpp"
