// SPDX-License-Identifier: GPL-3.0-only
// Included at the END of studioregressiontest.cpp. Run alone, in a fresh process.
#pragma once
#include "mainwindow.h"
#include "monitor/monitor.h"
#include "monitor/monitormanager.h"
#include "monitor/scopes/sharedframe.h"
#include "timeline2/view/timelinewidget.h"
#include <KActionMenu>
#include <QMenu>
#include <QScreen>
#include <QJsonDocument>
#include <kddockwidgets/DockWidget.h>
#include <algorithm>

TEST_CASE("FactMontage public demo uses the native dark workspace", "[FactMontagePublication][.gui]")
{
    REQUIRE_FALSE(pCore->window());
    const QString root = qEnvironmentVariable("FACTMONTAGE_DEMO_ROOT");
    REQUIRE_FALSE(root.isEmpty());
    const QDir folder(root);
    for (const QString &file : {QStringLiteral("warm.mp4"), QStringLiteral("blue.mp4"),
                               QStringLiteral("key.mp4"), QStringLiteral("tone.wav")})
        REQUIRE(QFileInfo(folder.filePath(QStringLiteral("assets/") + file)).isFile());
    const QString output = folder.filePath(QStringLiteral("screenshots"));
    REQUIRE(QDir().mkpath(output));
    REQUIRE(QGuiApplication::platformName() == QStringLiteral("xcb")); // Native screen capture, real X11 or Xvfb.
    KdenliveSettings::setKdockLayout(QString());
    KdenliveSettings::setDefault_profile(QStringLiteral("atsc_1080p_60"));
    KdenliveSettings::setVideotodefaultfolder(KdenliveDoc::SaveToProjectFolder);
    if (!qgetenv("STUDIO_QA_PREFIX").isEmpty()) qputenv("STUDIO_PREFIX", qgetenv("STUDIO_QA_PREFIX"));
    pCore->initGUI(QString(), QUrl());
    bool windowClosed = false;
    const auto closeDocument = qScopeGuard([&] {
        if (!windowClosed && pCore->currentDoc()) pCore->projectManager()->closeCurrentDocument(false, false);
    });
    REQUIRE(studioWait([] { return pCore->currentDoc() && pCore->window()->getCurrentTimeline()
        && pCore->window()->getCurrentTimeline()->model(); }, 15000));
    auto *window = pCore->window();
    QObject::connect(window, &QObject::destroyed, qApp, [] {
        qApp->setProperty("studioGuiShutdownComplete", true);
    });
    auto *timeline = window->getCurrentTimeline();
    auto model = timeline->model();
    REQUIRE(std::abs(pCore->getCurrentFps() - 60.0) < .001);
    REQUIRE(pCore->getProjectProfile().width() == 1920);
    REQUIRE(pCore->getProjectProfile().height() == 1080);
    // Avoid automatic proxy preparation for this already prepared QA material.
    pCore->currentDoc()->setDocumentProperty(QStringLiteral("enableproxy"), QStringLiteral("0"));
    window->resize(1920, 1080);
    window->show();
    auto *themeMenu = dynamic_cast<KActionMenu *>(window->actionCollection()->action(QStringLiteral("themes_menu")));
    REQUIRE(themeMenu);
    QAction *dark = nullptr;
    for (auto *action : themeMenu->menu()->actions()) {
        const QString name = action->text().remove(QLatin1Char('&'));
        if (name.contains(QStringLiteral("Breeze"), Qt::CaseInsensitive)
            && (name.contains(QStringLiteral("Dark"), Qt::CaseInsensitive)
                || name.contains(QStringLiteral("тём"), Qt::CaseInsensitive))) dark = action;
    }
    REQUIRE(dark); // Install the штатная Breeze Dark scheme; do not fabricate a palette.
    dark->trigger();
    QApplication::processEvents();
    REQUIRE(QApplication::palette().color(QPalette::Window).lightness() < 100);
    StudioPanel *panel = nullptr;
    for (auto *widget : window->findChildren<QWidget *>())
        if (auto *candidate = dynamic_cast<StudioPanel *>(widget)) panel = candidate;
    REQUIRE(panel);
    REQUIRE(panel->palette().color(QPalette::Window).lightness() < 100);
    panel->setMinimumWidth(440);
    KDDockWidgets::QtWidgets::DockWidget *studioDock = nullptr, *effectsDock = nullptr;
    for (auto *widget : QApplication::allWidgets()) {
        auto *dock = dynamic_cast<KDDockWidgets::QtWidgets::DockWidget *>(widget);
        if (!dock) continue;
        if (dock->objectName() == QLatin1String("video_studio")) studioDock = dock;
        if (dock->objectName() == QLatin1String("effect_stack")) effectsDock = dock;
    }
    REQUIRE(studioDock); REQUIRE(effectsDock);
    effectsDock->addDockWidgetAsTab(studioDock);
    studioDock->open(); studioDock->setAsCurrentTab();
    REQUIRE_FALSE(studioDock->isFloating());
    const auto raiseDock = [&](const QString &name) {
        auto *action = window->actionCollection()->action(QStringLiteral("raise_") + name);
        REQUIRE(action); action->trigger(); QApplication::processEvents();
    };
    raiseDock(QStringLiteral("video_studio"));
    raiseDock(QStringLiteral("projectmonitor"));
    auto tracks = model->getTracksIds(false);
    REQUIRE(tracks.size() >= 2);
    std::sort(tracks.begin(), tracks.end(), [&](int a, int b) { return model->getTrackPosition(a) < model->getTrackPosition(b); });
    const int lower = tracks.front(), upper = tracks.back();
    model->setTrackProperty(lower, QStringLiteral("kdenlive:track_name"), QStringLiteral("Blue base"));
    model->setTrackProperty(upper, QStringLiteral("kdenlive:track_name"), QStringLiteral("FactMontage demo"));
    const auto audioTracks = model->getTracksIds(true);
    REQUIRE_FALSE(audioTracks.isEmpty());
    model->setTrackProperty(audioTracks.front(), QStringLiteral("kdenlive:track_name"), QStringLiteral("Original tone"));
    const auto import = [&](const QString &file, const QString &name) {
        const QByteArray path = QFile::encodeName(folder.filePath(QStringLiteral("assets/") + file));
        auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), path.constData());
        REQUIRE(producer->is_valid());
        producer->set("kdenlive:clipname", name.toUtf8().constData());
        const auto bin = pCore->projectItemModel();
        QString id = QString::number(bin->getFreeClipId());
        Fun undo = [] { return true; }, redo = [] { return true; };
        REQUIRE(bin->requestAddBinClip(id, producer, bin->getRootFolder()->clipId(), undo, redo));
        return id;
    };
    const QString warm = import(QStringLiteral("warm.mp4"), QStringLiteral("Warm geometry"));
    const QString blue = import(QStringLiteral("blue.mp4"), QStringLiteral("Blue geometry"));
    const QString key = import(QStringLiteral("key.mp4"), QStringLiteral("Chroma geometry"));
    const QString tone = import(QStringLiteral("tone.wav"), QStringLiteral("Original tone"));
    const QString base = KdenliveTests::createProducer(pCore->getProjectProfile(), "#1b3e78", pCore->projectItemModel(), 2160, true);
    int baseId = -1;
    REQUIRE(model->requestClipInsertion(base, lower, 0, baseId, true, true, false));
    QVector<int> clips;
    for (int slot = 0; slot < 9; ++slot) {
        const QString id = slot == 2 ? key : slot % 2 ? blue : warm;
        int clip = -1;
        // Four seconds with one second of available source on either side of the cut.
        REQUIRE(model->requestClipInsertion(QStringLiteral("%1/60/299").arg(id), upper, slot * 240, clip, true, true, false));
        clips << clip;
    }
    int audio = -1;
    REQUIRE(model->requestClipInsertion(QStringLiteral("A") + tone, audioTracks.front(), 1680, audio, true, true, false));
    REQUIRE(model->requestItemResize(audio, 240, true, true) == 240);
    REQUIRE(studioWait([] { return pCore->taskManager.backgroundIdle(); }, 20000));
    const QString project = folder.filePath(QStringLiteral("FactMontage-demo.kdenlive"));
    REQUIRE_FALSE(QFileInfo::exists(project));
    REQUIRE(pCore->projectManager()->saveFileAs(project)); // Text/audio assets need a saved project.
    const auto showPanel = [&] {
        // The stock clip selection handler may raise the neighbouring effects tab.
        raiseDock(QStringLiteral("video_studio"));
        REQUIRE(panel->isVisible());
    };
    const auto select = [&](int id) {
        REQUIRE(model->requestSetSelection({id}));
        QApplication::processEvents(); panel->refreshSelection(); QApplication::processEvents();
        showPanel();
    };
    const auto navigate = [&](const QString &name) {
        showPanel();
        QToolButton *navigation = nullptr;
        for (auto *button : panel->findChildren<QToolButton *>())
            if (button->property("studioNav").toBool() && button->text() == name) navigation = button;
        REQUIRE(navigation); navigation->click(); QApplication::processEvents(); panel->refreshSelection();
    };
    const auto choice = [&](const QString &name) {
        showPanel();
        QToolButton *target = nullptr;
        for (auto *button : panel->findChildren<QToolButton *>())
            if (button->property("studioBaseText").toString() == name) target = button;
        REQUIRE(target); target->click(); QApplication::processEvents();
    };
    const auto spin = [&](const QString &name, double value) {
        showPanel();
        QDoubleSpinBox *target = nullptr;
        for (auto *widget : panel->findChildren<QDoubleSpinBox *>())
            if (widget->accessibleName() == name) target = widget;
        REQUIRE(target); target->setValue(value); QApplication::processEvents();
    };
    const auto primary = [&] {
        showPanel();
        QPushButton *target = nullptr;
        for (auto *button : panel->findChildren<QPushButton *>())
            if (button->property("studioPrimary").toBool() && button->isVisible() && button->isEnabled()) { REQUIRE_FALSE(target); target = button; }
        REQUIRE(target); REQUIRE(target->isEnabled()); target->click(); QApplication::processEvents();
    };
    const auto effect = [&](int clip, const QString &asset) {
        const auto found = effectsById(model->getClipEffectStack(clip), asset);
        REQUIRE(found.size() == 1);
        return found.front();
    };
    select(clips[0]); navigate(QStringLiteral("Карточки")); primary();
    effect(clips[0], QStringLiteral("card3d")); spin(QStringLiteral("Размер"), 76);
    select(clips[1]); navigate(QStringLiteral("Камера")); primary();
    effect(clips[1], QStringLiteral("studio_camera")); choice(QStringLiteral("Приблизить")); spin(QStringLiteral("Масштаб"), 150);
    select(clips[2]); navigate(QStringLiteral("Фон")); primary();
    REQUIRE(effect(clips[2], QStringLiteral("studio_background"))->getParam(QStringLiteral("method")).toInt() == 0);
    REQUIRE(model->requestSetSelection({clips[3], clips[4]}));
    QApplication::processEvents(); navigate(QStringLiteral("Переходы")); choice(QStringLiteral("Через расфокус"));
    spin(QStringLiteral("Длительность"), .8); primary();
    REQUIRE(model->studioTransitionModel(clips[4])); REQUIRE(model->getMixDuration(clips[4]) == 48);
    select(clips[5]); navigate(QStringLiteral("Эффекты")); choice(QStringLiteral("Глубокие края")); primary();
    effect(clips[5], QStringLiteral("studiofx"));
    select(clips[6]); navigate(QStringLiteral("Цвет")); primary();
    effect(clips[6], QStringLiteral("studio_color")); spin(QStringLiteral("Яркость"), .4);
    spin(QStringLiteral("Теплее / холоднее"), 28); spin(QStringLiteral("Насыщенность"), 125);
    select(audio); navigate(QStringLiteral("Звук"));
    auto *audioPage = panel->findChild<StudioAudioPage *>();
    auto *audioController = panel->findChild<StudioAudioController *>();
    REQUIRE(audioPage); REQUIRE(audioController);
    QTabBar *actions = nullptr;
    for (auto *bar : audioPage->findChildren<QTabBar *>()) if (bar->count() == 4) actions = bar;
    REQUIRE(actions); actions->setCurrentIndex(1); // Actual loudness workflow for an original tone.
    QStringList audioErrors;
    const auto connection = QObject::connect(audioController, &StudioAudioController::statusChanged, audioPage,
        [&](const QString &message, bool error) { if (error) audioErrors << message; });
    const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
    const int beforeAudio = pCore->undoStack()->index();
    primary(); REQUIRE(studioWait([&] { return !audioController->busy(); }, 120000));
    INFO(audioErrors.join(QLatin1Char('\n')).toStdString()); REQUIRE(audioErrors.isEmpty());
    REQUIRE(pCore->undoStack()->index() == beforeAudio + 1);
    select(clips[8]); navigate(QStringLiteral("Текст"));
    auto *text = panel->findChild<QPlainTextEdit *>(QStringLiteral("studioTextInput")); REQUIRE(text);
    text->setPlainText(QStringLiteral("FactMontage\nNative text"));
    auto *textSize = panel->findChild<QSpinBox *>(QStringLiteral("studioTextSize")); REQUIRE(textSize); textSize->setValue(88);
    auto *textApply = panel->findChild<QPushButton *>(QStringLiteral("studioTextApply")); REQUIRE(textApply); REQUIRE(textApply->isEnabled());
    const int beforeText = model->getClipsCount(); textApply->click();
    REQUIRE(studioWait([&] { return model->getClipsCount() == beforeText + 1 && pCore->taskManager.backgroundIdle(); }, 20000));
    REQUIRE(model->getCurrentSelection().size() == 1);
    const int title = *model->getCurrentSelection().begin();
    effect(title, QStringLiteral("sunimo_text_studio"));
    REQUIRE(pCore->projectManager()->saveFile());
    timeline->slotFitZoom();
    QJsonArray evidence;
    const auto capture = [&](const QString &name, const QString &page, int clip, int position) {
        if (name == QLatin1String("transitions")) {
            REQUIRE(model->requestSetSelection({clips[3], clips[4]}));
            QApplication::processEvents(); panel->refreshSelection();
        } else select(clip);
        navigate(page);
        raiseDock(QStringLiteral("projectmonitor")); raiseDock(QStringLiteral("video_studio"));
        REQUIRE(pCore->monitorManager()->activateMonitor(Kdenlive::ProjectMonitor));
        bool frameReady = false;
        const auto frameConnection = QObject::connect(pCore->monitorManager(), &MonitorManager::frameDisplayed, window,
            [&](const SharedFrame &frame) { if (frame.get_position() == position) frameReady = true; });
        const auto disconnectFrame = qScopeGuard([&] { QObject::disconnect(frameConnection); });
        timeline->controller()->setPosition(position);
        pCore->monitorManager()->projectMonitor()->requestSeek(position);
        REQUIRE(studioWait([&] { return frameReady; }, 10000));
        QElapsedTimer settle; settle.start();
        REQUIRE(studioWait([&] { return settle.elapsed() >= 1800; }, 4000));
        REQUIRE(panel->isVisible());
        REQUIRE(pCore->monitorManager()->projectMonitor()->position() == position);
        // Capture the real X11 window including the monitor's native child surface.
        // QWidget::grab alone can omit QQuickView video; never paste a rendered image into that hole.
        const QPixmap screenshot = window->screen()->grabWindow(window->winId());
        REQUIRE_FALSE(screenshot.isNull()); REQUIRE(screenshot.width() >= 1600); REQUIRE(screenshot.height() >= 1000);
        const QString destination = QDir(output).filePath(name + QStringLiteral("-dark.png"));
        REQUIRE(screenshot.save(destination));
        evidence.append(QJsonObject{{QStringLiteral("file"), QString(name + QStringLiteral("-dark.png"))},
            {QStringLiteral("page"), page}, {QStringLiteral("frame"), position}, {QStringLiteral("clip"), clip},
            {QStringLiteral("capture"), QStringLiteral("native X11 window")}});
    };
    capture(QStringLiteral("cards"), QStringLiteral("Карточки"), clips[0], 120);
    capture(QStringLiteral("camera"), QStringLiteral("Камера"), clips[1], 420);
    capture(QStringLiteral("background"), QStringLiteral("Фон"), clips[2], 600);
    capture(QStringLiteral("transitions"), QStringLiteral("Переходы"), clips[4], 952);
    capture(QStringLiteral("effects"), QStringLiteral("Эффекты"), clips[5], 1320);
    capture(QStringLiteral("colour"), QStringLiteral("Цвет"), clips[6], 1560);
    capture(QStringLiteral("audio"), QStringLiteral("Звук"), audio, 1800);
    capture(QStringLiteral("text"), QStringLiteral("Текст"), title, 2040);
    capture(QStringLiteral("workspace"), QStringLiteral("Карточки"), clips[0], 120);
    REQUIRE(pCore->projectManager()->saveFile());
    REQUIRE(studioWait([] { return pCore->taskManager.backgroundIdle(); }, 15000));
    model.reset(); // Release test-owned MLT objects before the native window closes the factory.
    QPointer<MainWindow> closingWindow(window);
    REQUIRE(window->close());
    windowClosed = true; // MainWindow has already destroyed ProjectManager; do not call it from the guard.
    QApplication::processEvents();
    REQUIRE(!closingWindow);
    QFile manifest(folder.filePath(QStringLiteral("capture-evidence.json"))); REQUIRE(manifest.open(QIODevice::WriteOnly));
    const QJsonObject report{{QStringLiteral("project"), project}, {QStringLiteral("screenshots"), evidence},
        {QStringLiteral("background"), QStringLiteral("chroma key only; human segmentation NOT RUN")},
        {QStringLiteral("checks"), QStringLiteral("real panel actions, model ownership, native save, monitor frames, dark palette, normal window close")},
        {QStringLiteral("limitations"), QStringLiteral("visual screenshot review, Save/Reopen, Undo/Redo, export and Deck acceptance are separate checks")}};
    REQUIRE(manifest.write(QJsonDocument(report).toJson()) > 0);
    qInfo().noquote() << "Public demo:" << project << "screenshots:" << output;
}
