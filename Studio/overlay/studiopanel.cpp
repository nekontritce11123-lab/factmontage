// SPDX-License-Identifier: GPL-3.0-only
#include "core/studioresources.hpp"
#include "studiopanel.hpp"
#include "studiobackgroundsource.hpp"
#include "studiomaskanalysis.hpp"
#include "studiohelpers.hpp"
using StudioHelpers::effectsById;
using StudioHelpers::appendParameterChange;
#include "audio/studioaudio.hpp"
#include "audio/studiomanagedaudio.hpp"
#include "subtitles/studiosubtitles.hpp"
#include "text/studiotext.hpp"
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "project/projectmanager.h"
#include "mltcontroller/clipcontroller.h"
#include "mainwindow.h"
#include "kdenlivesettings.h"
#include "profiles/profilemodel.hpp"
#include "assets/model/assetparametermodel.hpp"
#include "effects/effectsrepository.hpp"
#include "effects/effectstack/model/effectitemmodel.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "transitions/transitionsrepository.hpp"
#include "timeline2/model/timelinemodel.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/model/trackmodel.hpp"
#include "timeline2/view/timelinewidget.h"
#include "monitor/monitor.h"
#include "monitor/monitormanager.h"
#include "utils/flowlayout.h"
#include "bin/bin.h"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "bin/clipcreator.hpp"

#include <QButtonGroup>
#include <QApplication>
#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QCryptographicHash>
#include <QDomDocument>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMovie>
#include <QInputDialog>
#include <QImageReader>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QProcess>
#include "studiojobs.hpp"
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QUndoCommand>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>
#include <algorithm>
#include <functional>
#include <utility>

namespace {
std::shared_ptr<TimelineItemModel> activeStudioModel()
{
    if (!pCore || pCore->closing) return {};
    if (auto window = pCore->window()) {
        auto timeline = window->getCurrentTimeline();
        return timeline ? timeline->model() : nullptr;
    }
    // Headless host tests have a document and active model, but no MainWindow.
    auto document = pCore->currentDoc();
    auto manager = pCore->projectManager();
    auto model = manager ? manager->getTimeline() : nullptr;
    return document && model && document->getTimeline(model->uuid(), true) == model ? model : nullptr;
}

class StudioFlowLayout final : public FlowLayout
{
public:
    using FlowLayout::FlowLayout;
    QSize minimumSize() const override
    {
        QSize result = FlowLayout::minimumSize();
        int width = 0;
        for (int i = 0; i < count(); ++i) width = std::max(width, itemAt(i)->minimumSize().width());
        result.setWidth(width);
        return result;
    }
    QSize sizeHint() const override { return minimumSize(); }
};

// Explicit gesture boundaries: separate clicks never merge with a preceding drag.
class StudioCommand : public QUndoCommand
{
public:
    StudioCommand(std::shared_ptr<AssetParameterModel> effect, QStringList names, QStringList before, QStringList after)
        : QUndoCommand(QStringLiteral("Настроить эффект студии")), m_effect(std::move(effect)), m_names(std::move(names)),
          m_before(std::move(before)), m_after(std::move(after)) {}
    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }
private:
    void apply(const QStringList &values) {
        for (int i = 0; i < m_names.size(); ++i) m_effect->setParameter(m_names[i], values[i], true);
    }
    std::shared_ptr<AssetParameterModel> m_effect;
    QStringList m_names, m_before, m_after;
};

bool moveCameraBeforeCard(const std::shared_ptr<EffectStackModel> &stack, Fun &undo, Fun &redo)
{
    const auto cameras = effectsById(stack, QStringLiteral("studio_camera"));
    const auto cards = effectsById(stack, QStringLiteral("card3d"));
    if (cameras.size() != 1 || cards.isEmpty() || cameras.front()->row() < cards.front()->row()) return true;
    return stack->moveEffectWithUndo(cards.front()->row(), cameras.front(), undo, redo);
}

bool moveColorBeforeStudio(const std::shared_ptr<EffectStackModel> &stack, const std::shared_ptr<EffectItemModel> &color,
                           Fun &undo, Fun &redo)
{
    if (!stack || !color) return false;
    int destination = color->row();
    for (const auto &id : {QStringLiteral("studio_camera"), QStringLiteral("card3d")})
        for (const auto &effect : effectsById(stack, id)) destination = std::min(destination, effect->row());
    return destination == color->row() || stack->moveEffectWithUndo(destination, color, undo, redo);
}

bool moveStudioFxBeforeCard(const std::shared_ptr<EffectStackModel> &stack, const std::shared_ptr<EffectItemModel> &effect,
                            Fun &undo, Fun &redo)
{
    const auto cards = effectsById(stack, QStringLiteral("card3d"));
    if (!effect || cards.isEmpty() || effect->row() < cards.front()->row()) return true;
    return stack->moveEffectWithUndo(cards.front()->row(), effect, undo, redo);
}

bool moveBackgroundBeforeStudio(const std::shared_ptr<EffectStackModel> &stack, const std::shared_ptr<EffectItemModel> &background,
                                Fun &undo, Fun &redo)
{
    if (!stack || !background) return false;
    int destination = background->row();
    for (const auto &id : {QStringLiteral("studio_color"), QStringLiteral("studio_camera"), QStringLiteral("studiofx"), QStringLiteral("card3d")})
        for (const auto &effect : effectsById(stack, id)) destination = std::min(destination, effect->row());
    return destination == background->row() || stack->moveEffectWithUndo(destination, background, undo, redo);
}

QString xmlProperty(const QDomElement &owner, const QString &name)
{
    for (auto node = owner.firstChildElement(QStringLiteral("property")); !node.isNull(); node = node.nextSiblingElement(QStringLiteral("property")))
        if (node.attribute(QStringLiteral("name")) == name) return node.text();
    return {};
}

void setXmlProperty(QDomDocument &document, QDomElement owner, const QString &name, const QString &value)
{
    for (auto node = owner.firstChildElement(QStringLiteral("property")); !node.isNull(); node = node.nextSiblingElement(QStringLiteral("property"))) {
        if (node.attribute(QStringLiteral("name")) != name) continue;
        while (!node.firstChild().isNull()) node.removeChild(node.firstChild());
        node.appendChild(document.createTextNode(value));
        return;
    }
    auto property = document.createElement(QStringLiteral("property"));
    property.setAttribute(QStringLiteral("name"), name);
    property.appendChild(document.createTextNode(value));
    owner.appendChild(property);
}

bool studioAnalysisBoundary(const QDomElement &filter)
{
    const QString service = xmlProperty(filter, QStringLiteral("mlt_service"));
    const QString id = xmlProperty(filter, QStringLiteral("kdenlive_id"));
    return service == QLatin1String("studio.background") || id == QLatin1String("studio_background")
        || service == QLatin1String("studio.color") || id == QLatin1String("studio_color")
        || service == QLatin1String("studio.camera") || id == QLatin1String("studio_camera")
        || service == QLatin1String("frei0r.studiofx") || id == QLatin1String("studiofx")
        || service == QLatin1String("frei0r.card3d") || id == QLatin1String("card3d");
}

void stripStudioTail(QDomElement owner)
{
    bool remove = false;
    for (auto node = owner.firstChild(); !node.isNull();) {
        auto next = node.nextSibling();
        const auto element = node.toElement();
        if (!element.isNull() && element.tagName() == QLatin1String("filter")) {
            remove |= studioAnalysisBoundary(element);
            if (remove) owner.removeChild(node);
        } else if (!element.isNull()) stripStudioTail(element);
        node = next;
    }
}

QByteArray backgroundAnalysisXml(const std::shared_ptr<Mlt::Producer> &producer, const QString &proxy, const QString &source)
{
    if (!producer) return {};
    QDomDocument document;
    if (!document.setContent(ClipController::producerXml(*producer, true, true))) return {};
    for (const auto &tag : {QStringLiteral("producer"), QStringLiteral("chain")}) {
        const auto sources = document.elementsByTagName(tag);
        for (int i = 0; i < sources.count(); ++i) {
            auto item = sources.at(i).toElement();
            if (!proxy.isEmpty() && xmlProperty(item, QStringLiteral("resource")) == proxy)
                setXmlProperty(document, item, QStringLiteral("resource"), source);
            if (!proxy.isEmpty() && xmlProperty(item, QStringLiteral("warp_resource")) == proxy)
                setXmlProperty(document, item, QStringLiteral("warp_resource"), source);
            if (!xmlProperty(item, QStringLiteral("kdenlive:proxy")).isEmpty())
                setXmlProperty(document, item, QStringLiteral("kdenlive:proxy"), QString());
        }
    }
    stripStudioTail(document.documentElement());
    return document.toByteArray(2);
}

QString backgroundCacheRoot()
{
    auto document = pCore ? pCore->currentDoc() : nullptr;
    return document && !document->projectDataFolder().isEmpty()
        ? QDir(document->projectDataFolder()).absoluteFilePath(QStringLiteral("studio-background")) : QString();
}

QString backgroundModelSha()
{
    return QStringLiteral("2047b95bea344af15575f4279d6fac44bb07a42e7d537cf8d3265f15fe95cc82");
}

QString backgroundRecipeForXml(const QByteArray &xml, int quality, int fpsNum, int fpsDen, int sourceIn)
{
    QByteArray recipe = QByteArray("studio-background-recipe-v2\n") + QByteArray::number(quality) + ':'
        + QByteArray::number(fpsNum) + ':' + QByteArray::number(fpsDen) + ':' + QByteArray::number(sourceIn)
        + ':' + backgroundModelSha().toLatin1() + '\n';
    QDomDocument document;
    if (!document.setContent(xml)) return {};
    const auto filters = document.elementsByTagName(QStringLiteral("filter"));
    for (int i = 0; i < filters.count(); ++i) {
        const auto filter = filters.at(i).toElement();
        const QString id = xmlProperty(filter, QStringLiteral("kdenlive_id"));
        if (id.isEmpty()) continue;
        recipe += id.toUtf8() + '\n' + (xmlProperty(filter, QStringLiteral("disable")) == QLatin1String("1") ? QByteArray("1\n") : QByteArray("0\n"));
        QMap<QString, QString> parameters;
        for (auto property = filter.firstChildElement(QStringLiteral("property")); !property.isNull();
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

QString cardExitEnd(const std::shared_ptr<AssetParameterModel> &effect)
{
    const int frames = effect ? pCore->getItemDuration(effect->getOwnerId()) : 0;
    const double seconds = frames > 1 ? (frames - 1) / pCore->getCurrentFps() : 0.0;
    return QString::number(seconds / 21600.0, 'g', 17);
}

bool writeXmlAtomically(const QDomDocument &document, const QString &path)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(document.toByteArray(2)) < 0) return false;
    return file.commit();
}

QString transitionPresetFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/studio/transitions");
}

QJsonObject readTransitionPreset(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto object = document.object();
    return object.value(QStringLiteral("schema")).toInt() == 1 ? object : QJsonObject();
}

bool writeJsonAtomically(const QJsonObject &object, const QString &path)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0) return false;
    return file.commit();
}

QList<std::shared_ptr<EffectStackModel>> selectedVideoStacks()
{
    auto model = activeStudioModel();
    if (!model) return {};
    QList<std::shared_ptr<EffectStackModel>> result;
    for (int id : model->getCurrentSelection()) {
        if (!model->isClip(id)) continue;
        auto stack = model->getClipEffectStackModel(id);
        if (!stack || model->clipIsAudio(id)) continue;
        if (!result.contains(stack)) result << stack;
    }
    return result;
}

QList<std::shared_ptr<EffectStackModel>> batchVideoStacks(int *locked, int *audio)
{
    if (locked) *locked = 0;
    if (audio) *audio = 0;
    auto model = activeStudioModel();
    if (!model) return {};
    QList<std::shared_ptr<EffectStackModel>> result;
    for (int id : model->getCurrentSelection()) {
        if (!model->isClip(id)) continue;
        auto stack = model->getClipEffectStackModel(id);
        if (!stack || model->clipIsAudio(id)) {
            if (audio) ++*audio;
            continue;
        }
        const int trackId = model->getItemTrackId(id);
        if (trackId < 0 || model->trackIsLocked(trackId)) {
            if (locked) ++*locked;
            continue;
        }
        if (!result.contains(stack)) result << stack;
    }
    return result;
}

std::shared_ptr<EffectStackModel> selectedVideoStack()
{
    const auto stacks = selectedVideoStacks();
    return stacks.size() == 1 ? stacks.front() : nullptr;
}

bool stackUsesStillImage(const std::shared_ptr<EffectStackModel> &stack)
{
    auto model = activeStudioModel();
    if (!model || !stack) return false;
    for (int id : model->getCurrentSelection()) {
        if (!model->isClip(id) || model->getClipEffectStackModel(id) != stack) continue;
        const auto clip = pCore->projectItemModel()->getClipByBinID(model->getClipBinId(id));
        return clip && clip->clipType() == ClipType::Image;
    }
    return false;
}

std::shared_ptr<EffectStackModel> sequenceStack()
{
    auto model = activeStudioModel();
    return model ? model->getMasterEffectStackModel() : nullptr;
}

int transitionSoundClip(const std::shared_ptr<TimelineItemModel> &model, const QString &identity)
{
    if (identity.isEmpty()) return -1;
    int found = -1;
    for (int track : model->getTracksIds(true)) {
        if (model->getTrackProperty(track, QStringLiteral("kdenlive:studio_transition_sfx_track")).toInt() != 1) continue;
        for (int item : model->getItemsInRange(track, 0, -1, false)) {
            if (!model->isClip(item)) continue;
            const auto producer = model->getClipProducer(item);
            bool owned = producer && QString::fromUtf8(producer->get("studio:sfx:id")) == identity;
            if (!owned) for (const auto &effect : effectsById(model->getClipEffectStack(item), QStringLiteral("volume")))
                if (effect->getParam(QStringLiteral("studio:sfx:id")) == identity) { owned = true; break; }
            if (!owned) continue;
            if (found != -1) return -2;
            found = item;
        }
    }
    return found;
}
}

void StudioPanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ApplicationPaletteChange) {
        setPalette(QApplication::palette());
        // Qt stylesheets retain palette roles resolved before the host changed its theme.
        setStyleSheet(styleSheet());
    }
}

StudioPanel::StudioPanel(QWidget *parent) : QWidget(parent)
{
    setMinimumWidth(180);
    setObjectName(QStringLiteral("studioPanel"));
    setStyleSheet(QStringLiteral(R"(
#studioPanel QToolButton[studioNav="true"] {
    border: 0;
    border-bottom: 3px solid transparent;
    padding: 9px 8px 7px 8px;
    font-weight: 600;
}
#studioPanel QToolButton[studioNav="true"]:checked {
    border-bottom-color: palette(highlight);
    background: palette(midlight);
    color: palette(highlight);
}
#studioPanel QLabel#studioPageTitle { font-size: 18px; font-weight: 700; }
#studioPanel QLabel#studioPageSubtitle { color: palette(text); padding-bottom: 4px; }
#studioPanel QLabel#studioTarget {
    border-left: 3px solid palette(highlight);
    background: palette(base);
    padding: 7px 9px;
}
#studioPanel QToolButton[studioSection="true"] {
    border: 0;
    border-left: 3px solid transparent;
    padding: 8px;
    text-align: left;
    font-weight: 600;
}
#studioPanel QToolButton[studioSection="true"]:checked {
    border-left-color: palette(highlight);
    background: palette(base);
}
#studioPanel QToolButton[studioChoice="true"]:checked {
    border: 2px solid palette(highlight);
    background: palette(midlight);
}
#studioPanel QPushButton[studioPrimary="true"] {
    background: palette(highlight);
    color: palette(highlighted-text);
    border: 2px solid transparent;
    padding: 9px;
    font-weight: 600;
}
#studioPanel QPushButton[studioPrimary="true"]:disabled {
    background: palette(mid);
    color: palette(text);
}
#studioPanel QTabBar::tab:selected {
    color: palette(highlight);
    border-bottom: 2px solid palette(highlight);
    background: palette(midlight);
}
#studioPanel QPushButton:focus, #studioPanel QToolButton:focus,
#studioPanel QComboBox:focus, #studioPanel QLineEdit:focus {
    border: 2px solid palette(highlight);
}
#studioPanel QPushButton[studioPrimary="true"]:focus {
    border-color: palette(highlighted-text);
}
)"));
    auto layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    auto sectionsWidget = new QWidget(this);
    sectionsWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto sections = new StudioFlowLayout(nullptr, 0, 8, 4);
    sectionsWidget->setLayout(sections);
    m_cards = new QToolButton(this);
    m_camera = new QToolButton(this);
    m_background = new QToolButton(this);
    m_transitions = new QToolButton(this);
    m_effectsNav = new QToolButton(this);
    m_color = new QToolButton(this);
    m_audio = new QToolButton(this);
    m_subtitles = new QToolButton(this);
    for (auto button : {m_cards, m_camera, m_background, m_transitions, m_effectsNav, m_color, m_audio, m_subtitles}) {
        button->setCheckable(true);
        button->setProperty("studioNav", true);
        button->setMinimumWidth(88);
        button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        sections->addWidget(button);
    }
    m_cards->setText(QStringLiteral("Карточки"));
    m_camera->setText(QStringLiteral("Камера"));
    m_background->setText(QStringLiteral("Фон"));
    m_transitions->setText(QStringLiteral("Переходы"));
    m_effectsNav->setText(QStringLiteral("Эффекты"));
    m_color->setText(QStringLiteral("Цвет"));
    m_audio->setText(QStringLiteral("Звук"));
    m_subtitles->setText(QStringLiteral("Текст"));
    m_audio->setVisible(!StudioResources::executable(QStringLiteral("studio-audio")).isEmpty()
                        && !StudioResources::dataFile(QStringLiteral("studio-audio/parameters.json")).isEmpty());
    const bool subtitlesAvailable = !StudioResources::executable(QStringLiteral("studio-subtitle")).isEmpty()
                            && !StudioResources::executable(QStringLiteral("whisper-cli")).isEmpty()
                            && QFileInfo::exists(StudioResources::dataFile(QStringLiteral("studio-subtitles/ggml-small-q5_1.bin")));
    const bool textAvailable = !StudioResources::dataFile(QStringLiteral("studio-text/catalog.json")).isEmpty();
    m_subtitles->setVisible(textAvailable || subtitlesAvailable);
    m_cards->setChecked(true);
    auto sectionGroup = new QButtonGroup(this);
    sectionGroup->setExclusive(true);
    sectionGroup->addButton(m_cards, 0);
    sectionGroup->addButton(m_camera, 1);
    sectionGroup->addButton(m_background, 7);
    sectionGroup->addButton(m_transitions, 3);
    sectionGroup->addButton(m_effectsNav, 4);
    sectionGroup->addButton(m_color, 5);
    sectionGroup->addButton(m_audio, 6);
    sectionGroup->addButton(m_subtitles, 10);
    layout->addWidget(sectionsWidget);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto scrollContent = new QWidget(m_scroll);
    auto pageLayout = new QVBoxLayout(scrollContent);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(layout->spacing());
    pageLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    m_scroll->setWidget(scrollContent);
    m_scroll->viewport()->installEventFilter(this);
    layout->addWidget(m_scroll, 1);
    layout->removeWidget(sectionsWidget);
    pageLayout->addWidget(sectionsWidget);
    layout = pageLayout;
    m_pageTitle = new QLabel(QStringLiteral("Карточки"), this);
    m_pageTitle->setObjectName(QStringLiteral("studioPageTitle"));
    layout->addWidget(m_pageTitle);
    m_pageSubtitle = new QLabel(QStringLiteral("Оформление изображения или видео как отдельной карточки."), this);
    m_pageSubtitle->setObjectName(QStringLiteral("studioPageSubtitle"));
    m_pageSubtitle->setWordWrap(true);
    layout->addWidget(m_pageSubtitle);
    m_cameraTabs = new QTabBar(this);
    m_cameraTabs->setExpanding(true);
    m_cameraTabs->addTab(QStringLiteral("Движение"));
    m_cameraTabs->addTab(QStringLiteral("Трекинг лица"));
    m_cameraTabs->hide();
    layout->addWidget(m_cameraTabs);
    m_backgroundTabs = new QTabBar(this);
    m_backgroundTabs->setExpanding(true);
    m_backgroundTabs->addTab(QStringLiteral("Однотонный"));
    m_backgroundTabs->addTab(QStringLiteral("Человек"));
    m_backgroundTabs->hide();
    layout->addWidget(m_backgroundTabs);
    m_effectTabs = new QTabBar(this);
    m_effectTabs->setExpanding(true);
    m_effectTabs->addTab(QStringLiteral("Каталог"));
    m_effectTabs->addTab(QStringLiteral("Глазок"));
    m_effectTabs->addTab(QStringLiteral("Старая камера"));
    m_effectTabs->hide();
    layout->addWidget(m_effectTabs);
    m_textTabs = new QTabBar(this);
    m_textTabs->setExpanding(true);
    m_textTabs->addTab(QStringLiteral("Надписи"));
    m_textTabs->addTab(QStringLiteral("Субтитры"));
    m_textTabs->setTabEnabled(0, textAvailable);
    m_textTabs->setTabEnabled(1, subtitlesAvailable);
    m_textTabs->hide();
    layout->addWidget(m_textTabs);
    m_target = new QLabel(this);
    m_target->setObjectName(QStringLiteral("studioTarget"));
    m_target->setWordWrap(true);
    QFont targetFont = m_target->font();
    targetFont.setBold(true);
    m_target->setFont(targetFont);
    layout->addWidget(m_target);
    auto targets = new QVBoxLayout;
    m_clipTarget = new QToolButton(this);
    m_sequenceTarget = new QToolButton(this);
    m_clipTarget->setText(QStringLiteral("Выбранный клип"));
    m_sequenceTarget->setText(QStringLiteral("Вся последовательность"));
    for (auto button : {m_clipTarget, m_sequenceTarget}) {
        button->setCheckable(true);
        button->setProperty("studioChoice", true);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        targets->addWidget(button);
    }
    m_clipTarget->setChecked(true);
    auto targetGroup = new QButtonGroup(this);
    targetGroup->setExclusive(true);
    targetGroup->addButton(m_clipTarget, 0);
    targetGroup->addButton(m_sequenceTarget, 1);
    layout->addLayout(targets);
    m_scopeNote = new QLabel(QStringLiteral("Камера двигает весь монтаж, включая титры и наложения."), this);
    m_scopeNote->setWordWrap(true);
    layout->addWidget(m_scopeNote);
    auto options = new QToolButton(this);
    options->setText(QStringLiteral("Настройки"));
    options->setPopupMode(QToolButton::InstantPopup);
    auto optionsMenu = new QMenu(options);
    auto previewGroup = new QActionGroup(optionsMenu);
    previewGroup->setExclusive(true);
    for (const auto &[label, scale] : {std::pair{QStringLiteral("Предпросмотр 1:1"), 1},
                                      std::pair{QStringLiteral("Предпросмотр 1:2"), 2},
                                      std::pair{QStringLiteral("Предпросмотр 1:4"), 4}}) {
        auto action = optionsMenu->addAction(label);
        action->setCheckable(true);
        action->setData(scale);
        action->setChecked(std::max(1, KdenliveSettings::previewScaling()) == scale);
        previewGroup->addAction(action);
    }
    optionsMenu->addSeparator();
    auto backgroundQuality = optionsMenu->addMenu(QStringLiteral("Качество анализа человека"));
    m_backgroundQualityMenuAction = backgroundQuality->menuAction();
    auto backgroundQualityGroup = new QActionGroup(backgroundQuality);
    backgroundQualityGroup->setExclusive(true);
    const int savedBackgroundSize = QSettings().value(QStringLiteral("StudioBackground/analysisSize"), 768).toInt();
    for (int size : {512, 768, 1280}) {
        auto action = backgroundQuality->addAction(QStringLiteral("%1 px").arg(size));
        action->setCheckable(true);
        action->setData(size);
        action->setChecked(size == savedBackgroundSize);
        backgroundQualityGroup->addAction(action);
    }
    connect(backgroundQualityGroup, &QActionGroup::triggered, this, [](QAction *action) {
        QSettings().setValue(QStringLiteral("StudioBackground/analysisSize"), action->data().toInt());
    });
    m_fixOrder = optionsMenu->addAction(QStringLiteral("Камера перед карточкой"));
    m_fixOrder->setVisible(false);
    m_duplicateEffect = optionsMenu->addAction(QStringLiteral("Добавить ещё один экземпляр"));
    m_duplicateEffect->setVisible(false);
    m_removeTransition = optionsMenu->addAction(QStringLiteral("Удалить переход"));
    m_removeTransition->setVisible(false);
    options->setMenu(optionsMenu);
    layout->addWidget(options, 0, Qt::AlignRight);
    m_status = new QLabel(QStringLiteral("Выберите один видеоклип или изображение на таймлайне."), this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_backgroundBox = new QWidget(this);
    auto backgroundBoxLayout = new QVBoxLayout(m_backgroundBox);
    backgroundBoxLayout->setContentsMargins(0, 0, 0, 0);
    m_backgroundFrame = new QLabel(QStringLiteral("Выберите видеоклип или изображение на таймлайне"), m_backgroundBox);
    m_backgroundFrame->setAlignment(Qt::AlignCenter);
    m_backgroundFrame->setWordWrap(true);
    m_backgroundFrame->setMinimumHeight(180);
    m_backgroundFrame->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_backgroundFrame->setCursor(Qt::CrossCursor);
    m_backgroundFrame->setStyleSheet(QStringLiteral("border: 1px solid palette(mid); border-radius: 4px; background: palette(base);"));
    m_backgroundFrame->installEventFilter(this);
    backgroundBoxLayout->addWidget(m_backgroundFrame, 0, Qt::AlignLeft);
    m_backgroundColor = new QPushButton(m_backgroundBox);
    backgroundBoxLayout->addWidget(m_backgroundColor);
    m_backgroundBox->hide();
    layout->addWidget(m_backgroundBox);
    m_add = new QPushButton(QStringLiteral("Добавить карточку"), this);
    m_add->setProperty("studioPrimary", true);
    layout->addWidget(m_add);
    m_presetTabs = new QTabBar(this);
    m_presetTabs->setExpanding(true);
    m_presetTabs->addTab(QStringLiteral("Готовые"));
    m_presetTabs->addTab(QStringLiteral("Мои"));
    layout->addWidget(m_presetTabs);
    m_presetBar = new QWidget(this);
    auto presetLayout = new QVBoxLayout(m_presetBar);
    presetLayout->setContentsMargins(0, 0, 0, 0);
    m_presets = new QComboBox(m_presetBar);
    m_presets->setAccessibleName(QStringLiteral("Мои шаблоны"));
    presetLayout->addWidget(m_presets);
    auto presetActions = new QHBoxLayout;
    m_applyPreset = new QPushButton(QStringLiteral("Применить"), m_presetBar);
    m_savePreset = new QPushButton(QStringLiteral("Сохранить"), m_presetBar);
    auto presetMenuButton = new QToolButton(m_presetBar);
    presetMenuButton->setText(QStringLiteral("Ещё"));
    presetMenuButton->setPopupMode(QToolButton::InstantPopup);
    auto presetMenu = new QMenu(presetMenuButton);
    m_replacePreset = presetMenu->addAction(QStringLiteral("Заменить"));
    m_renamePreset = presetMenu->addAction(QStringLiteral("Переименовать"));
    m_deletePreset = presetMenu->addAction(QStringLiteral("Удалить"));
    presetMenuButton->setMenu(presetMenu);
    presetActions->addWidget(m_applyPreset);
    presetActions->addWidget(m_savePreset);
    presetActions->addWidget(presetMenuButton);
    presetLayout->addLayout(presetActions);
    m_batchApply = new QPushButton(m_presetBar);
    presetLayout->addWidget(m_batchApply);
    m_presetBar->hide();
    layout->addWidget(m_presetBar);
    m_instances = new QComboBox(this);
    m_instances->setAccessibleName(QStringLiteral("Экземпляр карточки"));
    layout->addWidget(m_instances);
    m_enabled = new QToolButton(this);
    m_enabled->setCheckable(true);
    m_enabled->hide();
    layout->addWidget(m_enabled);
    m_content = new QWidget(scrollContent);
    m_contentLayout = new QVBoxLayout(m_content);
    m_contentLayout->setContentsMargins(0, 0, 0, 0);
    m_contentLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    layout->addWidget(m_content);
    m_movie = new QMovie(this);
    m_movie->setCacheMode(QMovie::CacheNone);
    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setSingleShot(true);
    m_hoverTimer->setInterval(180);
    m_monitorCommitTimer = new QTimer(this);
    m_monitorCommitTimer->setSingleShot(true);
    m_monitorCommitTimer->setInterval(250);
    m_audioController = new StudioAudioController(this);
    connect(m_hoverTimer, &QTimer::timeout, this, [this] { if (m_hoverPreview) startPreview(m_hoverPreview); });
    connect(m_movie, &QMovie::frameChanged, this, [this] {
        if (m_preview) m_preview->setIcon(QIcon(m_movie->currentPixmap()));
    });
    connect(sectionGroup, &QButtonGroup::idClicked, this, [this](int index) {
        loadProduct(index == 10 && !m_textTabs->isTabEnabled(0) ? 11 : index);
    });
    connect(m_cameraTabs, &QTabBar::currentChanged, this, [this](int tab) { loadProduct(tab + 1); });
    connect(m_backgroundTabs, &QTabBar::currentChanged, this, &StudioPanel::setBackgroundMethod);
    connect(m_effectTabs, &QTabBar::currentChanged, this, [this](int tab) { loadProduct(tab == 0 ? 4 : tab == 1 ? 8 : 9); });
    connect(m_textTabs, &QTabBar::currentChanged, this, [this](int tab) { if (tab >= 0) loadProduct(tab == 0 ? 10 : 11); });
    connect(m_backgroundColor, &QPushButton::clicked, this, [this] {
        QColor color = QColorDialog::getColor(m_backgroundMethod == 0 ? m_backgroundKey : m_backgroundFill, this,
                                               m_backgroundMethod == 0 ? QStringLiteral("Цвет однотонного фона") : QStringLiteral("Цвет заливки"));
        if (!color.isValid()) return;
        if (m_backgroundMethod == 0) m_backgroundKey = color; else m_backgroundFill = color;
        const QStringList names = m_backgroundMethod == 0
            ? QStringList{QStringLiteral("key_r"), QStringLiteral("key_g"), QStringLiteral("key_b")}
            : QStringList{QStringLiteral("fill_r"), QStringLiteral("fill_g"), QStringLiteral("fill_b")};
        const QStringList after{QString::number(color.redF(), 'g', 17), QString::number(color.greenF(), 'g', 17),
                                QString::number(color.blueF(), 'g', 17)};
        if (m_effect && !backgroundNeedsAnalysis()) {
            QStringList before;
            for (const auto &name : names) before << m_effect->getParam(name);
            if (before != after) pCore->pushUndo(new StudioCommand(m_effect, names, before, after));
        } else {
            for (int i = 0; i < names.size(); ++i) m_pendingBackgroundValues.insert(names[i], after[i].toDouble());
        }
        paintBackgroundFrame();
    });
    connect(m_presetTabs, &QTabBar::currentChanged, this, [this](int id) {
        m_presetBar->setVisible(id == 1);
        if (id == 1) loadPresets();
        refreshSelection();
    });
    connect(targetGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_sequence = id == 1;
        refreshSelection();
    });
    connect(m_instances, &QComboBox::currentIndexChanged, this, &StudioPanel::selectInstance);
    connect(m_enabled, &QToolButton::clicked, this, [this](bool enabled) {
        auto effect = std::dynamic_pointer_cast<EffectItemModel>(m_effect);
        if (!effect) return;
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        effect->markEnabled(enabled, undo, redo, false);
        redo();
        pCore->pushUndo(undo, redo, enabled ? QStringLiteral("Включить эффект студии") : QStringLiteral("Выключить эффект студии"));
        refreshValues();
    });
    connect(previewGroup, &QActionGroup::triggered, this, [](QAction *action) {
        KdenliveSettings::setPreviewScaling(action->data().toInt());
        Q_EMIT pCore->monitorManager()->updatePreviewScaling();
    });
    connect(m_savePreset, &QPushButton::clicked, this, [this] { savePreset(false); });
    connect(m_replacePreset, &QAction::triggered, this, [this] { savePreset(true); });
    connect(m_renamePreset, &QAction::triggered, this, &StudioPanel::renamePreset);
    connect(m_deletePreset, &QAction::triggered, this, &StudioPanel::deletePreset);
    connect(m_applyPreset, &QPushButton::clicked, this, [this] { applySelectedPreset(false); });
    connect(m_batchApply, &QPushButton::clicked, this, [this] { applySelectedPreset(true); });
    connect(m_fixOrder, &QAction::triggered, this, &StudioPanel::fixCameraOrder);
    connect(m_duplicateEffect, &QAction::triggered, this, [this] { applyEffectRecipe(true); });
    connect(m_removeTransition, &QAction::triggered, this, &StudioPanel::removeTransition);
    connect(m_presets, &QComboBox::currentIndexChanged, this, [this] {
        const bool selected = m_presets->currentIndex() >= 0;
        m_applyPreset->setEnabled(selected);
        m_replacePreset->setEnabled(selected && (bool(m_effect) || m_assetId == QLatin1String("studio_transition")));
        m_renamePreset->setEnabled(selected);
        m_deletePreset->setEnabled(selected);
        if (m_batchApply->isVisible()) m_batchApply->setEnabled(selected && m_batchApply->text() != QLatin1String("Применить к 0 клипам"));
    });
    connect(m_monitorCommitTimer, &QTimer::timeout, this, &StudioPanel::finishMonitorGesture);
    connect(m_add, &QPushButton::clicked, this, [this] {
        if (m_assetId == QLatin1String("studio_transition")) {
            refreshTransitionSelection();
            auto model = activeStudioModel();
            if (!model || !supported() || m_transitionFirst < 0) return;
            const int frames = qMax(1, qRound(m_controls[QStringLiteral("duration")].spec.value(QStringLiteral("value")).toDouble(.65)
                                               * pCore->getCurrentFps()));
            const QString previousStatus = m_status->text();
            if (!applyTransition(frames, transitionParameters(frames))) {
                if (m_status->text() == previousStatus)
                    m_status->setText(QStringLiteral("Не удалось добавить переход. Проверьте доступные кадры у стыка."));
                return;
            }
            refreshSelection();
            return;
        }
        if (m_assetId == QLatin1String("studiofx")) { applyEffectRecipe(); return; }
        if (m_assetId == QLatin1String("studio_background")) { applyBackground(); return; }
        refreshSelection();
        if (!currentTarget() || !supported() || !m_effects.isEmpty()) return;
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        if (!m_stack->appendEffectWithUndo(m_assetId, undo, redo).first) {
            m_status->setText(QStringLiteral("Не удалось добавить %1. Проверьте наличие эффекта.").arg(m_title));
            return;
        }
        if (m_assetId == QLatin1String("studio_camera") && !m_sequence) {
            if (!moveCameraBeforeCard(m_stack, undo, redo)) {
                undo();
                m_status->setText(QStringLiteral("Не удалось расположить камеру перед карточкой."));
                return;
            }
        }
        if (m_assetId == QLatin1String("studio_color")) {
            const auto colors = effectsById(m_stack, m_assetId);
            if (colors.size() != 1 || !moveColorBeforeStudio(m_stack, colors.front(), undo, redo)) {
                undo();
                m_status->setText(QStringLiteral("Не удалось добавить цвет без изменения чужого стека."));
                return;
            }
            appendParameterChange(colors.front(), {QStringLiteral("studio_input_validated"), QStringLiteral("studio_color_algorithm")},
                                  {QStringLiteral("1"), QStringLiteral("sdr-primary-v1")}, undo, redo);
        }
        if (m_assetId == QLatin1String("card3d")) {
            const auto cards = effectsById(m_stack, m_assetId);
            if (cards.isEmpty()) { undo(); return; }
            const QString source = stackUsesStillImage(m_stack) ? QStringLiteral("0") : QStringLiteral("1");
            appendParameterChange(cards.back(), {QStringLiteral("7"), QStringLiteral("17")}, {source, cardExitEnd(cards.back())}, undo, redo);
        }
        pCore->pushUndo(undo, redo, QStringLiteral("Добавить эффект монтажной студии"));
        refreshSelection();
    });
    loadProduct(0);
}

StudioPanel::~StudioPanel()
{
    // Text drafts belong to this panel; save the active page before members die.
    if (m_textPage) delete m_textPage;
    if (m_audioController) m_audioController->cancel();
    if (m_subtitlePage) m_subtitlePage->deactivate();
    m_backgroundCanceled = true;
    StudioJobs::dispose(m_backgroundProducer);
    StudioJobs::dispose(m_backgroundAnalyzer);
    if (!m_backgroundXml.isEmpty()) QFile::remove(m_backgroundXml);
    if (!m_backgroundOutput.isEmpty()) {
        const QFileInfo partBase(m_backgroundOutput);
        QDir partFolder(partBase.absolutePath());
        for (const QString &part : partFolder.entryList({partBase.fileName() + QStringLiteral(".part.*")}, QDir::Files))
            partFolder.remove(part);
    }
    stopFraming();
    StudioJobs::dispose(m_tracker);
}

void StudioPanel::loadProduct(int index)
{
    cancelAnalyses();
    m_add->setEnabled(true);
    if (m_audioPage) m_audioPage->deactivate();
    if (m_subtitlePage) m_subtitlePage->deactivate();
    finishDrag();
    stopPreview();
    stopFraming();
    m_loaded = false;
    m_effect.reset();
    m_effects.clear();
    m_controls.clear();
    m_groups.clear();
    m_framing = nullptr;
    m_framingDone = nullptr;
    m_cardPosition = nullptr;
    m_cardPositionDone = nullptr;
    m_effectCenter = nullptr;
    m_effectCenterDone = nullptr;
    m_effectFilters = nullptr;
    m_effectSearch = nullptr;
    m_effectCategory = nullptr;
    m_effectFilter = nullptr;
    m_effectFavorite = nullptr;
    m_effectRecipes = QJsonArray();
    m_backgroundPresets = QJsonArray();
    m_backgroundParameters.clear();
    m_center = nullptr;
    m_swap = nullptr;
    m_pointStart = nullptr;
    m_pointEnd = nullptr;
    m_trackingFrame = nullptr;
    m_trackingApply = nullptr;
    m_headCancel = nullptr;
    m_trackingImage = QImage();
    m_trackingPoint = QPointF(-1, -1);
    m_pendingTrackingZoom = 120.0;
    while (auto item = m_contentLayout->takeAt(0)) { delete item->widget(); delete item; }
    m_content->show();
    m_scroll->verticalScrollBar()->setValue(0);
    m_scroll->horizontalScrollBar()->setValue(0);
    m_page = index;
    const bool transitionPage = index == 3;
    const bool catalogEffectsPage = index == 4;
    const bool lensPage = index == 8;
    const bool vintagePage = index == 9;
    const bool effectsPage = catalogEffectsPage || lensPage || vintagePage;
    const bool colorPage = index == 5;
    const bool audioPage = index == 6;
    const bool backgroundPage = index == 7;
    const bool textPage = index == 10;
    const bool subtitlePage = index == 11;
    const bool camera = index == 1 || index == 2;
    const bool trackingPage = index == 2;
    m_cards->setChecked(index == 0);
    m_camera->setChecked(camera);
    m_background->setChecked(backgroundPage);
    m_transitions->setChecked(transitionPage);
    m_effectsNav->setChecked(effectsPage);
    m_color->setChecked(colorPage);
    m_audio->setChecked(audioPage);
    m_subtitles->setChecked(textPage || subtitlePage);
    m_cameraTabs->setVisible(camera);
    m_backgroundTabs->setVisible(backgroundPage);
    m_effectTabs->setVisible(effectsPage);
    m_textTabs->setVisible(textPage || subtitlePage);
    { QSignalBlocker blocker(m_cameraTabs); m_cameraTabs->setCurrentIndex(trackingPage ? 1 : 0); }
    { QSignalBlocker blocker(m_backgroundTabs); m_backgroundTabs->setCurrentIndex(m_backgroundMethod); }
    { QSignalBlocker blocker(m_effectTabs); m_effectTabs->setCurrentIndex(catalogEffectsPage ? 0 : lensPage ? 1 : 2); }
    { QSignalBlocker blocker(m_textTabs); m_textTabs->setCurrentIndex(subtitlePage ? 1 : 0); }
    m_clipTarget->setVisible(camera && !trackingPage);
    m_sequenceTarget->setVisible(camera && !trackingPage);
    if (!camera || trackingPage || effectsPage || transitionPage || colorPage || audioPage || backgroundPage || textPage || subtitlePage) {
        m_sequence = false;
        m_clipTarget->setChecked(true);
    }
    m_scopeNote->setVisible(camera && m_sequence);
    m_pageTitle->setText(textPage || subtitlePage ? QStringLiteral("Текст и субтитры") : backgroundPage ? QStringLiteral("Фон") : audioPage ? QStringLiteral("Звук") : colorPage ? QStringLiteral("Цвет") : transitionPage ? QStringLiteral("Переходы") : effectsPage ? QStringLiteral("Эффекты")
                                                                            : camera ? QStringLiteral("Камера") : QStringLiteral("Карточки"));
    m_pageSubtitle->setText(textPage ? QStringLiteral("Надпись на отдельной видеодорожке: текст, появление, движение и исчезновение.")
                            : subtitlePage ? QStringLiteral("Точная локальная расшифровка русской речи и готовое оформление без ручной разбивки строк.")
                            : backgroundPage ? (m_backgroundMethod == 0
                                ? QStringLiteral("Убирает ровный однотонный фон без предварительного анализа.")
                                : QStringLiteral("Рассчитывает маску человека и размывает, скрывает или заливает фон."))
                            : audioPage ? QStringLiteral("Готовая обработка выделенного звука одним действием.")
                            : colorPage ? QStringLiteral("Восемь стилей и ручная коррекция цвета в SDR.")
                            : trackingPage ? QStringLiteral("Нажмите на лицо в стоп-кадре, задайте приближение и примените трекинг.")
                            : transitionPage ? QStringLiteral("Плавно соединяют два соседних видеоклипа.")
                            : lensPage ? QStringLiteral("Квадратный или круглый глазок с настоящей оптической геометрией и тёмными краями.")
                            : vintagePage ? QStringLiteral("Сигнал старой камеры: кассета, строки, зерно, пыль и нестабильность.")
                            : effectsPage ? QStringLiteral("Готовые видеоэффекты для одного или нескольких выбранных клипов.")
                            : camera ? QStringLiteral("Готовые движения, кадрирование и живая камера.")
                                     : QStringLiteral("Оформление изображения или видео как отдельной карточки."));
    m_target->setVisible(!audioPage && !textPage && !subtitlePage);
    m_status->setVisible(!audioPage && !textPage && !subtitlePage);
    m_backgroundBox->setVisible(backgroundPage);
    m_backgroundQualityMenuAction->setVisible(backgroundPage);
    m_presetTabs->setVisible(!trackingPage && !audioPage && !textPage && !subtitlePage);
    m_presetBar->setVisible(!trackingPage && !audioPage && !textPage && !subtitlePage && m_presetTabs->currentIndex() == 1);
    if (textPage) {
        m_add->hide(); m_instances->hide(); m_enabled->hide(); m_scroll->show(); m_content->setEnabled(true);
        m_textPage = new StudioTextPage(&m_textDrafts, m_content);
        m_contentLayout->addWidget(m_textPage);
        m_contentLayout->addStretch();
        m_scroll->verticalScrollBar()->setValue(0);
        m_loaded = true;
        return;
    }
    if (subtitlePage) {
        m_add->hide();
        m_instances->hide();
        m_enabled->hide();
        m_scroll->show();
        m_content->setEnabled(true);
        m_subtitlePage = new StudioSubtitlePage(m_content);
        m_contentLayout->addWidget(m_subtitlePage);
        m_contentLayout->addStretch();
        m_loaded = true;
        m_subtitlePage->activate();
        return;
    }
    if (audioPage) {
        m_add->hide();
        m_instances->hide();
        m_enabled->hide();
        m_scroll->show();
        m_content->setEnabled(true);
        m_audioPage = new StudioAudioPage(m_audioController, m_content);
        m_contentLayout->addWidget(m_audioPage);
        m_contentLayout->addStretch();
        m_loaded = true;
        m_audioPage->activate();
        return;
    }
    m_assetId = backgroundPage ? QStringLiteral("studio_background") : colorPage ? QStringLiteral("studio_color") : transitionPage ? QStringLiteral("studio_transition")
        : lensPage ? QStringLiteral("studio_lens") : vintagePage ? QStringLiteral("studio_vintage") : effectsPage ? QStringLiteral("studiofx")
                                                                        : camera ? QStringLiteral("studio_camera") : QStringLiteral("card3d");
    m_title = backgroundPage ? QStringLiteral("фон") : colorPage ? QStringLiteral("цвет") : transitionPage ? QStringLiteral("переход")
        : lensPage ? QStringLiteral("оптический глазок") : vintagePage ? QStringLiteral("старую камеру") : effectsPage ? QStringLiteral("видеоэффект")
                                                               : camera ? QStringLiteral("камеру") : QStringLiteral("карточку");
    m_add->setText(backgroundPage ? (m_backgroundMethod == 0 ? QStringLiteral("Убрать фон") : QStringLiteral("Проанализировать и применить"))
                            : colorPage ? QStringLiteral("Добавить цвет") : transitionPage ? QStringLiteral("Добавить переход")
                              : lensPage ? QStringLiteral("Добавить глазок") : vintagePage ? QStringLiteral("Добавить старую камеру") : effectsPage ? QStringLiteral("Применить эффект")
                                                                             : camera ? QStringLiteral("Добавить камеру") : QStringLiteral("Добавить карточку"));
    m_instances->setAccessibleName(backgroundPage ? QStringLiteral("Экземпляр удаления фона") : transitionPage ? QStringLiteral("Переход") : effectsPage ? QStringLiteral("Экземпляр эффекта")
                                                                                  : camera ? QStringLiteral("Экземпляр камеры") : QStringLiteral("Экземпляр карточки"));
    const auto fileName = backgroundPage ? QStringLiteral("studio/studio_background.json") : colorPage ? QStringLiteral("studio/studio_color.json") : transitionPage ? QStringLiteral("studio/studio_transition.json")
        : lensPage ? QStringLiteral("studio/studio_lens.json") : vintagePage ? QStringLiteral("studio/studio_vintage.json") : effectsPage ? QStringLiteral("studio/studiofx.json")
                                                                                              : camera ? QStringLiteral("studio/studio_camera.json") : QStringLiteral("studio/studio.json");
    const auto schemaPath = QStandardPaths::locate(QStandardPaths::AppDataLocation, fileName);
    if (m_resources.isEmpty()) m_resources = schemaPath.left(schemaPath.lastIndexOf(QLatin1Char('/')) + 1);
    QFile file(schemaPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_add->hide(); m_instances->hide(); m_content->hide();
        m_status->setText(QStringLiteral("Не найдено описание раздела. Переустановите сборку студии."));
        return;
    }
    const auto schema = QJsonDocument::fromJson(file.readAll()).object();
    m_effectRecipes = schema.value(QStringLiteral("recipes")).toArray();
    m_backgroundPresets = backgroundPage ? schema.value(QStringLiteral("presets")).toArray() : QJsonArray();
    if (backgroundPage) {
        for (const auto &value : schema.value(QStringLiteral("parameters")).toArray()) {
            const QString key = value.toObject().value(QStringLiteral("key")).toString();
            if (!key.isEmpty()) m_backgroundParameters << key;
        }
    }
    if (backgroundPage && m_pendingBackgroundValues.isEmpty() && !m_backgroundPresets.isEmpty())
        m_pendingBackgroundValues = m_backgroundPresets.first().toObject().value(QStringLiteral("values")).toObject();
    m_groupsSpec = schema.value(QStringLiteral("groups")).toArray();
    const auto specs = schema.value(QStringLiteral("controls")).toArray();
    const int expectedControls = lensPage ? 16 : vintagePage ? 17 : backgroundPage ? 8 : colorPage ? 9 : transitionPage ? 9 : effectsPage ? 10 : camera ? 13 : 17;
    const int expectedGroups = backgroundPage ? 3 : colorPage ? 3 : transitionPage ? 2 : effectsPage ? 4 : camera ? 5 : 6;
    if (schema.value(QStringLiteral("version")).toInt() != 2 || specs.size() != expectedControls || m_groupsSpec.size() != expectedGroups
        || (catalogEffectsPage && m_effectRecipes.size() != 80)
        || (backgroundPage && (m_backgroundPresets.size() != 6 || m_backgroundParameters.size() != 15))
        || schema.value(QStringLiteral("effect")).toString() != m_assetId) {
        m_add->hide(); m_instances->hide(); m_content->hide();
        m_status->setText(QStringLiteral("Описание панели несовместимо с этой сборкой."));
        return;
    }
    if (catalogEffectsPage) {
        m_pendingRecipe = std::clamp(m_pendingRecipe, 0, int(m_effectRecipes.size()) - 1);
        m_effectFilters = new QWidget(m_content);
        auto filterLayout = new QVBoxLayout(m_effectFilters);
        filterLayout->setContentsMargins(0, 0, 0, 8);
        m_effectSearch = new QLineEdit(m_effectFilters);
        m_effectSearch->setPlaceholderText(QStringLiteral("Поиск: VHS, линза, плёнка…"));
        m_effectSearch->setClearButtonEnabled(true);
        filterLayout->addWidget(m_effectSearch);
        auto filterRow = new StudioFlowLayout(nullptr, 0, 6, 6);
        m_effectCategory = new QComboBox(m_effectFilters);
        m_effectCategory->addItem(QStringLiteral("Все категории"), QString());
        QStringList categories;
        for (const auto &value : m_effectRecipes) {
            const auto recipe = value.toObject();
            const auto id = recipe.value(QStringLiteral("category_id")).toString();
            if (categories.contains(id)) continue;
            categories << id;
            m_effectCategory->addItem(recipe.value(QStringLiteral("category")).toString(), id);
        }
        m_effectFilter = new QComboBox(m_effectFilters);
        m_effectFilter->addItems({QStringLiteral("Все"), QStringLiteral("Избранное"), QStringLiteral("Недавние")});
        m_effectFavorite = new QToolButton(m_effectFilters);
        m_effectFavorite->setCheckable(true);
        filterRow->addWidget(m_effectCategory);
        filterRow->addWidget(m_effectFilter);
        filterRow->addWidget(m_effectFavorite);
        filterLayout->addLayout(filterRow);
        m_contentLayout->addWidget(m_effectFilters);
        connect(m_effectSearch, &QLineEdit::textChanged, this, &StudioPanel::filterEffectRecipes);
        connect(m_effectCategory, &QComboBox::currentIndexChanged, this, &StudioPanel::filterEffectRecipes);
        connect(m_effectFilter, &QComboBox::currentIndexChanged, this, &StudioPanel::filterEffectRecipes);
        connect(m_effectFavorite, &QToolButton::clicked, this, [this](bool favorite) {
            if (m_pendingRecipe < 0 || m_pendingRecipe >= m_effectRecipes.size()) return;
            const auto id = m_effectRecipes[m_pendingRecipe].toObject().value(QStringLiteral("id")).toString();
            auto favorites = QSettings().value(QStringLiteral("StudioFX/favorites")).toStringList();
            favorites.removeAll(id);
            if (favorite) favorites << id;
            QSettings().setValue(QStringLiteral("StudioFX/favorites"), favorites);
            filterEffectRecipes();
        });
    }
    if (colorPage) {
        auto looks = new QWidget(m_content);
        auto looksLayout = new StudioFlowLayout(nullptr, 0, 8, 8);
        looks->setLayout(looksLayout);
        for (const auto &value : schema.value(QStringLiteral("presets")).toArray()) {
            const auto preset = value.toObject();
            auto button = new QToolButton(looks);
            button->setText(preset.value(QStringLiteral("name")).toString());
            button->setToolTip(preset.value(QStringLiteral("description")).toString());
            button->setProperty("studioChoice", true);
            button->setCheckable(true);
            button->setProperty("studioPresetValues", preset.value(QStringLiteral("manual")).toObject().toVariantMap());
            looksLayout->addWidget(button);
            connect(button, &QToolButton::clicked, this, [this, preset] {
                if (!m_effect || m_assetId != QLatin1String("studio_color")) return;
                QStringList names, before, after;
                const auto manual = preset.value(QStringLiteral("manual")).toObject();
                for (auto it = manual.begin(); it != manual.end(); ++it) {
                    names << it.key(); before << m_effect->getParam(it.key()); after << QString::number(it.value().toDouble(), 'g', 17);
                }
                if (before != after) pCore->pushUndo(new StudioCommand(m_effect, names, before, after));
                refreshValues();
            });
        }
        m_contentLayout->addWidget(looks);
    }
    if (backgroundPage) {
        auto ready = new QWidget(m_content);
        auto readyLayout = new StudioFlowLayout(nullptr, 0, 8, 8);
        ready->setLayout(readyLayout);
        for (const auto &value : m_backgroundPresets) {
            const auto preset = value.toObject();
            auto button = new QToolButton(ready);
            button->setText(preset.value(QStringLiteral("name")).toString());
            button->setToolTip(preset.value(QStringLiteral("description")).toString());
            button->setProperty("studioChoice", true);
            button->setProperty("studioPreview", QString(m_resources + preset.value(QStringLiteral("preview")).toString()));
            button->setIcon(QIcon(button->property("studioPreview").toString() + QStringLiteral(".png")));
            button->setIconSize(QSize(144, 81));
            button->setCheckable(true);
            button->setProperty("studioPresetValues", preset.value(QStringLiteral("values")).toObject().toVariantMap());
            button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            button->installEventFilter(this);
            readyLayout->addWidget(button);
            connect(button, &QToolButton::clicked, this, [this, preset] { applyBackgroundPreset(preset); });
        }
        m_contentLayout->addWidget(ready);
    }
    if (lensPage || vintagePage) {
        auto ready = new QWidget(m_content);
        auto readyLayout = new StudioFlowLayout(nullptr, 0, 8, 8);
        ready->setLayout(readyLayout);
        for (const auto &value : schema.value(QStringLiteral("presets")).toArray()) {
            const auto preset = value.toObject();
            auto button = new QToolButton(ready);
            button->setText(preset.value(QStringLiteral("name")).toString());
            button->setToolTip(preset.value(QStringLiteral("description")).toString());
            button->setCheckable(true);
            button->setProperty("studioChoice", true);
            button->setProperty("studioPresetValues", preset.value(QStringLiteral("values")).toObject().toVariantMap());
            readyLayout->addWidget(button);
            connect(button, &QToolButton::clicked, this, [this, preset] {
                if (!m_effect || !currentTarget() || !supported()) return;
                QStringList names, before, after;
                const auto values = preset.value(QStringLiteral("values")).toObject();
                for (auto it = values.begin(); it != values.end(); ++it) {
                    names << it.key(); before << m_effect->getParam(it.key());
                    after << QString::number(it.value().toDouble(), 'g', 17);
                }
                if (before != after) pCore->pushUndo(new StudioCommand(m_effect, names, before, after));
                refreshValues();
            });
        }
        m_contentLayout->addWidget(ready);
    }
    QString openGroup;
    for (const auto &groupValue : m_groupsSpec) {
        const auto groupSpec = groupValue.toObject();
        const auto groupKey = groupSpec.value(QStringLiteral("key")).toString();
        auto section = new QWidget(m_content);
        auto sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(0, 0, 0, 8);
        auto &group = m_groups[groupKey];
        group.spec = groupSpec;
        group.section = section;
        group.header = new QToolButton(section);
        group.header->setText(groupSpec.value(QStringLiteral("label")).toString());
        group.header->setCheckable(true);
        const QString settingKey = QStringLiteral("studio/groups/%1/%2").arg(m_assetId, groupKey);
        const bool requestedOpen = trackingPage ? groupKey == QLatin1String("tracking")
            : QSettings().value(settingKey, !groupSpec.value(QStringLiteral("collapsed")).toBool()).toBool();
        group.header->setChecked(requestedOpen && openGroup.isEmpty());
        if (group.header->isChecked()) openGroup = groupKey;
        group.header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        group.header->setProperty("studioSection", true);
        group.header->setArrowType(group.header->isChecked() ? Qt::DownArrow : Qt::RightArrow);
        group.header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        sectionLayout->addWidget(group.header);
        group.body = new QWidget(section);
        auto bodyLayout = new QVBoxLayout(group.body);
        bodyLayout->setContentsMargins(12, 0, 0, 0);
        if (camera && groupKey == QLatin1String("framing")) {
            auto hint = new QLabel(QStringLiteral("Рамка редактируется в мониторе. Перемещайте её для выбора цели, меняйте размер для масштаба."), group.body);
            hint->setWordWrap(true); bodyLayout->addWidget(hint);
            m_framing = new QPushButton(QStringLiteral("Настроить кадрирование"), group.body);
            m_framing->setProperty("studioPrimary", true);
            bodyLayout->addWidget(m_framing);
            auto points = new QHBoxLayout;
            m_pointStart = new QPushButton(QStringLiteral("Выбирать начало"), group.body);
            m_pointEnd = new QPushButton(QStringLiteral("Выбирать конец / цель"), group.body);
            m_pointStart->setCheckable(true); m_pointEnd->setCheckable(true); m_pointEnd->setChecked(true);
            points->addWidget(m_pointStart); points->addWidget(m_pointEnd); bodyLayout->addLayout(points);
            auto framingActions = new QHBoxLayout;
            m_center = new QPushButton(QStringLiteral("По центру"), group.body);
            m_swap = new QPushButton(QStringLiteral("Поменять начало и конец"), group.body);
            m_framingDone = new QPushButton(QStringLiteral("Готово"), group.body);
            framingActions->addWidget(m_center); framingActions->addWidget(m_swap); framingActions->addWidget(m_framingDone);
            bodyLayout->addLayout(framingActions);
            m_framingDone->hide();
            connect(m_framing, &QPushButton::clicked, this, &StudioPanel::startFraming);
            connect(m_framingDone, &QPushButton::clicked, this, &StudioPanel::stopFraming);
            connect(m_center, &QPushButton::clicked, this, &StudioPanel::centerFraming);
            connect(m_swap, &QPushButton::clicked, this, &StudioPanel::swapFraming);
            connect(m_pointStart, &QPushButton::clicked, this, [this] {
                finishMonitorGesture(); m_editEnd = false; m_pointStart->setChecked(true); m_pointEnd->setChecked(false); updateFramingRect();
            });
            connect(m_pointEnd, &QPushButton::clicked, this, [this] {
                finishMonitorGesture(); m_editEnd = true; m_pointStart->setChecked(false); m_pointEnd->setChecked(true); updateFramingRect();
            });
        }
        if (!camera && groupKey == QLatin1String("placement")) {
            auto hint = new QLabel(QStringLiteral("Перетащите рамку в мониторе проекта. Ползунки положения меняются вместе с ней."), group.body);
            hint->setWordWrap(true); bodyLayout->addWidget(hint);
            m_cardPosition = new QPushButton(QStringLiteral("Расположить карточку в мониторе"), group.body);
            m_cardPosition->setProperty("studioPrimary", true);
            m_cardPositionDone = new QPushButton(QStringLiteral("Готово"), group.body);
            m_cardPositionDone->hide();
            bodyLayout->addWidget(m_cardPosition);
            bodyLayout->addWidget(m_cardPositionDone);
            connect(m_cardPosition, &QPushButton::clicked, this, &StudioPanel::startCardPosition);
            connect(m_cardPositionDone, &QPushButton::clicked, this, &StudioPanel::stopFraming);
        }
        if (effectsPage && groupKey == QLatin1String("shape")) {
            auto hint = new QLabel(QStringLiteral("Сначала укажите центр в мониторе, затем при необходимости уточните X/Y."), group.body);
            hint->setWordWrap(true);
            bodyLayout->addWidget(hint);
            m_effectCenter = new QPushButton(QStringLiteral("Указать центр в мониторе"), group.body);
            m_effectCenter->setProperty("studioPrimary", true);
            m_effectCenterDone = new QPushButton(QStringLiteral("Готово"), group.body);
            m_effectCenterDone->hide();
            bodyLayout->addWidget(m_effectCenter);
            bodyLayout->addWidget(m_effectCenterDone);
            connect(m_effectCenter, &QPushButton::clicked, this, &StudioPanel::startEffectCenter);
            connect(m_effectCenterDone, &QPushButton::clicked, this, &StudioPanel::stopFraming);
        }
        if (camera && groupKey == QLatin1String("tracking")) {
            auto hint = new QLabel(QStringLiteral("Стоп-кадр берётся из выбранного клипа на таймлайне. Нажмите на лицо один раз."), group.body);
            hint->setWordWrap(true);
            bodyLayout->addWidget(hint);
            m_trackingFrame = new QLabel(QStringLiteral("Выберите видеоклип на таймлайне"), group.body);
            m_trackingFrame->setAlignment(Qt::AlignCenter);
            m_trackingFrame->setMinimumHeight(180);
            m_trackingFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            m_trackingFrame->setCursor(Qt::CrossCursor);
            m_trackingFrame->setStyleSheet(QStringLiteral("border: 1px solid palette(mid); border-radius: 4px; background: palette(base);"));
            m_trackingFrame->installEventFilter(this);
            bodyLayout->addWidget(m_trackingFrame);
            if (trackingPage && m_controls.contains(QStringLiteral("zoom"))) {
                auto zoom = m_controls[QStringLiteral("zoom")].row;
                zoom->setParent(group.body);
                bodyLayout->addWidget(zoom);
            }
            m_trackingApply = new QPushButton(QStringLiteral("Применить трекинг"), group.body);
            m_trackingApply->setProperty("studioPrimary", true);
            m_headCancel = new QPushButton(QStringLiteral("Отменить"), group.body);
            bodyLayout->addWidget(m_trackingApply);
            bodyLayout->addWidget(m_headCancel);
            m_headCancel->hide();
            connect(m_trackingApply, &QPushButton::clicked, this, &StudioPanel::runHeadTracking);
            connect(m_headCancel, &QPushButton::clicked, this, [this] {
                StudioJobs::cancel(m_tracker);
            });
        }
        for (const auto &key : groupSpec.value(QStringLiteral("keys")).toArray()) {
            for (const auto &value : specs) {
                const auto spec = value.toObject();
                if (spec.value(QStringLiteral("key")) == key) {
                    const QString controlKey = key.toString();
                    if (camera && groupKey == QLatin1String("tracking")
                        && (controlKey == QLatin1String("tracking") || controlKey == QLatin1String("track_path")
                            || controlKey == QLatin1String("track_offset"))) {
                        auto &control = m_controls[controlKey];
                        control.spec = spec;
                        control.row = new QWidget(group.body);
                        control.row->hide();
                        bodyLayout->addWidget(control.row);
                        continue;
                    }
                    auto control = makeControl(spec);
                    if (!camera && spec.value(QStringLiteral("key")) == QLatin1String("POSITION_MODE")) control->hide();
                    if (camera && spec.value(QStringLiteral("key")).toString().contains(QLatin1Char('_'))
                        && (spec.value(QStringLiteral("key")).toString().startsWith(QLatin1String("start"))
                            || spec.value(QStringLiteral("key")).toString().startsWith(QLatin1String("end")))) control->hide();
                    bodyLayout->addWidget(control);
                }
            }
        }
        auto reset = new QPushButton(QStringLiteral("Сбросить раздел"), group.body);
        connect(reset, &QPushButton::clicked, this, [this, groupKey] { resetGroup(groupKey); });
        bodyLayout->addWidget(reset);
        reset->setVisible(!trackingPage && !(effectsPage && groupKey == QLatin1String("catalog")));
        group.body->setVisible(group.header->isChecked());
        connect(group.header, &QToolButton::toggled, this, [this, groupKey, settingKey](bool open) {
            auto &item = m_groups[groupKey];
            if (open) {
                for (auto it = m_groups.begin(); it != m_groups.end(); ++it)
                    if (it.key() != groupKey && it.value().header && it.value().header->isChecked()) it.value().header->setChecked(false);
            }
            item.header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            item.body->setVisible(open);
            stopPreview();
            if (!open) stopFraming();
            QSettings().setValue(settingKey, open);
        });
        sectionLayout->addWidget(group.body);
        const bool methodAllows = !backgroundPage || !groupSpec.contains(QStringLiteral("method"))
            || groupSpec.value(QStringLiteral("method")).toInt() == m_backgroundMethod;
        section->setVisible(methodAllows && (trackingPage ? groupKey == QLatin1String("tracking")
                                                           : !camera || groupKey != QLatin1String("tracking")));
        m_contentLayout->addWidget(section);
    }
    m_contentLayout->addStretch();
    m_loaded = m_controls.size() == expectedControls;
    if (catalogEffectsPage) { selectEffectRecipe(m_pendingRecipe); filterEffectRecipes(); }
    m_add->hide(); m_instances->hide(); m_scroll->show(); m_content->setEnabled(false);
    loadPresets();
    refreshSelection();
}

QString StudioPanel::parameterName(const QJsonObject &spec) const
{
    if (spec.contains(QStringLiteral("parameter"))) return spec.value(QStringLiteral("parameter")).toString();
    if (spec.contains(QStringLiteral("index"))) return QString::number(spec.value(QStringLiteral("index")).toInt());
    return spec.value(QStringLiteral("key")).toString();
}

double StudioPanel::displayedValue(const QJsonObject &spec) const
{
    if (m_assetId == QLatin1String("studio_transition")
        && (!m_effect || spec.value(QStringLiteral("key")) == QLatin1String("duration")))
        return spec.value(QStringLiteral("value")).toDouble();
    const QString name = parameterName(spec);
    const bool stagedBackground = m_assetId == QLatin1String("studio_background") && (!m_effect || backgroundNeedsAnalysis());
    const QString stored = stagedBackground && m_pendingBackgroundValues.contains(name)
        ? m_pendingBackgroundValues.value(name).toString() : m_effect->getParam(name);
    double value = stored.isEmpty() && m_assetId == QLatin1String("studio_transition") && name == QLatin1String("studio:sfx_level")
        ? .5 : stored.toDouble();
    if (m_assetId == QLatin1String("studio_transition") && spec.value(QStringLiteral("key")) == QLatin1String("style")) {
        int nearest = 0;
        double distance = 2;
        for (const auto &entry : spec.value(QStringLiteral("options")).toArray()) {
            const auto option = entry.toObject();
            const double difference = std::abs(value - option.value(QStringLiteral("code")).toDouble());
            if (difference < distance) { distance = difference; nearest = option.value(QStringLiteral("value")).toInt(); }
        }
        return nearest;
    }
    return spec.contains(QStringLiteral("normalized")) ? value * spec.value(QStringLiteral("hi")).toDouble() : value;
}

QString StudioPanel::storedValue(const QJsonObject &spec, double value) const
{
    if (m_assetId == QLatin1String("studio_transition") && spec.value(QStringLiteral("key")) == QLatin1String("style")) {
        const int index = qRound(value);
        for (const auto &entry : spec.value(QStringLiteral("options")).toArray()) {
            const auto option = entry.toObject();
            if (option.value(QStringLiteral("value")).toInt() == index)
                return QString::number(option.value(QStringLiteral("code")).toDouble(), 'g', 17);
        }
        return QStringLiteral("0");
    }
    if (spec.contains(QStringLiteral("normalized"))) value /= spec.value(QStringLiteral("hi")).toDouble();
    return QString::number(value, 'g', 17);
}

QString StudioPanel::choiceName(const QJsonObject &spec, int value) const
{
    for (const auto &optionValue : spec.value(QStringLiteral("options")).toArray()) {
        const auto option = optionValue.toObject();
        if (option.value(QStringLiteral("value")).toInt() == value) return option.value(QStringLiteral("name")).toString();
    }
    return {};
}

QWidget *StudioPanel::makeControl(const QJsonObject &spec)
{
    const auto key = spec.value(QStringLiteral("key")).toString();
    const auto kind = spec.value(QStringLiteral("kind")).toString();
    auto &control = m_controls[key];
    control.spec = spec;
    control.row = new QWidget;
    auto layout = new QVBoxLayout(control.row);
    layout->setContentsMargins(0, 0, 0, 8);
    control.label = new QLabel(spec.value(QStringLiteral("label")).toString(), control.row);
    auto title = control.label;
    layout->addWidget(title);
    control.row->setToolTip(spec.value(QStringLiteral("tip")).toString());
    if (kind == QLatin1String("number")) {
        control.spin = new QDoubleSpinBox;
        control.spin->setDecimals(spec.value(QStringLiteral("decimals")).toInt());
        control.spin->setRange(spec.value(QStringLiteral("lo")).toDouble(), spec.value(QStringLiteral("hi")).toDouble());
        control.spin->setSuffix(spec.value(QStringLiteral("suffix")).toString());
        control.spin->setKeyboardTracking(false);
        control.spin->setAccessibleName(title->text());
        title->setBuddy(control.spin);
        const int factor = int(std::pow(10, control.spin->decimals()));
        control.spin->setSingleStep(spec.contains(QStringLiteral("step")) ? spec.value(QStringLiteral("step")).toDouble() : 1.0/factor);
        control.slider = new QSlider(Qt::Horizontal);
        control.slider->setRange(int(std::round(control.spin->minimum()*factor)), int(std::round(control.spin->maximum()*factor)));
        if (m_assetId == QLatin1String("studio_transition")) control.slider->setTracking(false);
        control.slider->setAccessibleName(title->text());
        auto valueLayout = new QHBoxLayout;
        valueLayout->addWidget(control.slider, 1);
        valueLayout->addWidget(control.spin);
        layout->addLayout(valueLayout);
        if (m_assetId == QLatin1String("studio_transition") && key == QLatin1String("duration")) control.slider->hide();
        connect(control.spin, &QDoubleSpinBox::valueChanged, this, [this, key](double value) { commit(key, value); });
        connect(control.slider, &QSlider::sliderPressed, this, [this, key] {
            finishDrag();
            if (!currentTarget() || !m_effect || !supported() || backgroundNeedsAnalysis()) return;
            m_dragEffect = m_effect;
            m_dragUndo = pCore->undoStack();
            m_dragNames = {parameterName(m_controls[key].spec)};
            if (m_assetId == QLatin1String("card3d") && (key == QLatin1String("X") || key == QLatin1String("Y")))
                m_dragNames.prepend(parameterName(m_controls[QStringLiteral("POSITION_MODE")].spec));
            for (const auto &name : m_dragNames) m_dragBefore << m_effect->getParam(name);
            m_dragAfter = m_dragBefore;
        });
        connect(control.slider, &QSlider::valueChanged, this, [this, key, factor](int value) { commit(key, double(value)/factor); });
        connect(control.slider, &QSlider::sliderReleased, this, &StudioPanel::finishDrag);
    } else if (kind == QLatin1String("bool")) {
        title->hide();
        control.check = new QCheckBox(spec.value(QStringLiteral("label")).toString(), control.row);
        control.check->setAccessibleName(control.check->text());
        layout->addWidget(control.check);
        connect(control.check, &QCheckBox::toggled, this, [this, key](bool checked) { commit(key, checked ? 1.0 : 0.0); });
    } else if (kind == QLatin1String("sound")) {
        control.combo = new QComboBox(control.row);
        control.combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        control.combo->setMinimumContentsLength(10);
        control.combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        for (const auto &option : spec.value(QStringLiteral("options")).toArray()) {
            const auto item = option.toObject();
            control.combo->addItem(item.value(QStringLiteral("name")).toString(), item.value(QStringLiteral("value")).toString());
        }
        control.combo->setAccessibleName(title->text());
        title->setBuddy(control.combo);
        layout->addWidget(control.combo);
        connect(control.combo, &QComboBox::activated, this, [this, key](int index) {
            chooseTransitionSound(m_controls[key].combo->itemData(index).toString());
        });
        auto browse = new QPushButton(QStringLiteral("Добавить свой звук…"), control.row);
        browse->setAccessibleName(QStringLiteral("Добавить свой звук перехода"));
        layout->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QSettings settings;
            const QString start = settings.value(QStringLiteral("studio/transitionSoundDirectory"),
                QStandardPaths::writableLocation(QStandardPaths::MusicLocation)).toString();
            const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Выбрать звук перехода"), start,
                QStringLiteral("Аудиофайлы (*.wav *.mp3 *.flac *.ogg *.opus *.m4a *.aac);;Все файлы (*)"));
            if (path.isEmpty()) return;
            QSettings().setValue(QStringLiteral("studio/transitionSoundDirectory"), QFileInfo(path).absolutePath());
            chooseTransitionSound(QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
        });
    } else if (key == QLatin1String("SOURCE") || key == QLatin1String("BACKGROUND")
               || key == QLatin1String("live") || key == QLatin1String("tracking")) {
        control.combo = new QComboBox;
        for (const auto &option : spec.value(QStringLiteral("options")).toArray())
            control.combo->addItem(option.toObject().value(QStringLiteral("name")).toString());
        title->setBuddy(control.combo);
        control.combo->setAccessibleName(title->text());
        layout->addWidget(control.combo);
        auto explanation = new QLabel(spec.value(QStringLiteral("tip")).toString(), control.row);
        explanation->setWordWrap(true);
        layout->addWidget(explanation);
        connect(control.combo, &QComboBox::activated, this, [this, key](int choice) { commit(key, choice); });
    } else {
        const auto options = spec.value(QStringLiteral("options")).toArray();
        const auto positions = spec.value(QStringLiteral("positions")).toArray();
        auto grid = positions.isEmpty() ? nullptr : new QGridLayout;
        auto flow = positions.isEmpty() ? new StudioFlowLayout(nullptr, 0, 8, 8) : nullptr;
        for (int i = 0; i < options.size(); ++i) {
            const auto option = options[i].toObject();
            const auto name = option.value(QStringLiteral("name")).toString();
            const int optionValue = option.value(QStringLiteral("value")).toInt(i);
            auto button = new QToolButton;
            button->setText(name);
            button->setProperty("studioBaseText", name);
            button->setProperty("studioChoice", true);
            button->setProperty("studioCategory", option.value(QStringLiteral("category_id")).toString());
            button->setProperty("studioRecipeId", option.value(QStringLiteral("value")).toInt(i));
            button->setAccessibleName(title->text() + QStringLiteral(": ") + button->text());
            button->setCheckable(true);
            button->setMinimumHeight(36);
            button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
            button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            auto cell = new QWidget;
            if (positions.isEmpty()) cell->setMinimumWidth(150);
            auto cellLayout = new QVBoxLayout(cell);
            cellLayout->setContentsMargins(0, 0, 0, 0);
            cellLayout->addWidget(button);
            const auto description = option.value(QStringLiteral("description")).toString();
            if (!description.isEmpty()) {
                auto descriptionLabel = new QLabel(description, cell);
                descriptionLabel->setWordWrap(true);
                descriptionLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
                cellLayout->addWidget(descriptionLabel);
            }
            const auto preview = option.value(QStringLiteral("preview")).toString();
            if (!preview.isEmpty()) {
                const QString base = m_resources + preview;
                button->setProperty("studioPreview", base);
                button->setIcon(QIcon(base + QStringLiteral(".png")));
                button->setIconSize(QSize(144, 81));
                button->installEventFilter(this);
            }
            if (grid) {
                const auto pos = positions[i].toArray();
                grid->addWidget(cell, pos[0].toInt(), pos[1].toInt());
            } else flow->addWidget(cell);
            control.choices.append(button);
            connect(button, &QToolButton::clicked, this, [this, key, optionValue] {
                if (m_assetId == QLatin1String("studiofx") && key == QLatin1String("recipe")) selectEffectRecipe(optionValue);
                else { commit(key, optionValue); refreshValues(); }
            });
        }
        layout->addLayout(grid ? static_cast<QLayout *>(grid) : static_cast<QLayout *>(flow));
    }
    return control.row;
}

bool StudioPanel::supported() const
{
    // This preference requests only 10-bit-compatible effects; it is not a
    // measurement of the source footage or the project processing precision.
    if (!pCore->getCurrentProfile() || KdenliveSettings::tenbitpipeline()) return false;
    const int colorspace = pCore->getCurrentProfile()->colorspace();
    return colorspace == 709 || colorspace == 601;
}

bool StudioPanel::currentTarget() const
{
    if (m_assetId == QLatin1String("studio_transition")) {
        auto model = activeStudioModel();
        return model && m_effect && model->studioTransitionModel(m_transitionSecond) == m_effect;
    }
    return m_stack && (m_sequence ? sequenceStack() : selectedVideoStack()) == m_stack;
}

void StudioPanel::refreshSelection()
{
    if (!m_loaded) return;
    auto activeModel = activeStudioModel();
    auto stillSelected = [&activeModel](int clipId, const std::weak_ptr<EffectStackModel> &stack) {
        return activeModel && activeModel->isClip(clipId) && activeModel->getCurrentSelection().count(clipId)
            && activeModel->getClipEffectStackModel(clipId) == stack.lock();
    };
    if ((!m_trackingOutput.isEmpty() && !stillSelected(m_trackingClipId, m_trackingStack))
        || (m_backgroundClipId >= 0 && !stillSelected(m_backgroundClipId, m_backgroundStack))) cancelAnalyses();
    if (m_backgroundAssetDocument != pCore->currentDoc() || m_backgroundAssetRoot != backgroundCacheRoot()) {
        m_backgroundAssetDocument = pCore->currentDoc();
        m_backgroundAssetRoot = backgroundCacheRoot();
        resolveBackgroundAssets();
    }
    if (m_page == 6) {
        m_audioController->selectionChanged();
        if (m_audioPage) m_audioPage->refreshSelection();
        return;
    }
    if (m_page == 10) {
        if (m_textPage) m_textPage->refreshSelection();
        return;
    }
    if (m_page == 11) {
        if (m_subtitlePage) m_subtitlePage->refreshSelection();
        return;
    }
    if (m_framingActive) stopFraming();
    finishDrag();
    auto previous = m_effect;
    for (const auto &connection : std::as_const(m_connections)) disconnect(connection);
    m_connections.clear();
    disconnect(m_parameterConnection);
    m_stack.reset(); m_effect.reset(); m_effects.clear();
    for (auto button : m_content->findChildren<QToolButton *>())
        if (button->property("studioPresetValues").isValid()) button->setChecked(false);
    m_selectedClipId = -1;
    m_add->hide(); m_fixOrder->setVisible(false); m_duplicateEffect->setVisible(false); m_removeTransition->setVisible(false);
    m_instances->hide(); m_enabled->hide(); m_content->setEnabled(true);
    m_transitionFirst = -1; m_transitionSecond = -1;
    for (auto it = m_controls.begin(); it != m_controls.end(); ++it) it.value().row->setEnabled(false);
    for (auto button : {m_framing, m_cardPosition, m_effectCenter, m_trackingApply}) if (button) button->setEnabled(false);
    m_batchApply->hide();
    m_status->setText(QStringLiteral("Шаг 1 · выберите один видеоклип или изображение на таймлайне."));
    m_scopeNote->setVisible(m_assetId == QLatin1String("studio_camera") && m_sequence);
    for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
        const bool pageAllows = m_page == 2 ? it.key() == QLatin1String("tracking")
                                           : m_page != 1 || it.key() != QLatin1String("tracking");
        if (it.value().section)
            it.value().section->setVisible(pageAllows && (!it.value().spec.value(QStringLiteral("clip_only")).toBool() || !m_sequence));
    }
    auto model = activeModel;
    if (m_assetId == QLatin1String("studio_transition")) { refreshTransitionSelection(); return; }
    const auto stacks = selectedVideoStacks();
    if (m_sequence && m_assetId == QLatin1String("studio_camera")) {
        m_target->setText(QStringLiteral("Последовательность · активная"));
        m_stack = sequenceStack();
        const bool clipCamera = stacks.size() == 1 && !effectsById(stacks.front(), QStringLiteral("studio_camera")).isEmpty();
        m_scopeNote->setText(clipCamera
            ? QStringLiteral("Камера двигает весь монтаж, включая титры и наложения. На выбранном клипе тоже есть камера: движения складываются.")
            : QStringLiteral("Камера двигает весь монтаж, включая титры и наложения."));
    } else if (stacks.size() == 1 && model) {
        m_stack = stacks.front();
        for (int id : model->getCurrentSelection()) {
            if (!model->isClip(id) || model->getClipEffectStackModel(id) != m_stack) continue;
            m_selectedClipId = id;
            resolveBackgroundAssets(id);
            m_target->setText(QStringLiteral("%1 · %2").arg(model->getTrackTagById(model->getItemTrackId(id)), model->getClipName(id)));
            break;
        }
    } else if (stacks.size() > 1) {
        m_target->setText(QStringLiteral("Выбрано клипов: %1").arg(stacks.size()));
        if (m_assetId == QLatin1String("studio_background")) {
            m_status->setText(QStringLiteral("Удаление фона работает с одним выбранным клипом. Выберите один клип."));
            loadBackgroundFrame();
            return;
        }
        int locked = 0, audio = 0;
        const auto targets = batchVideoStacks(&locked, &audio);
        if (m_assetId == QLatin1String("studiofx")) {
            m_status->setText(QStringLiteral("Выберите готовый эффект. Он будет добавлен ко всем подходящим клипам."));
            m_add->show();
            m_batchApply->setText(QStringLiteral("Применить мой шаблон к %1 клипам").arg(targets.size()));
            m_batchApply->setVisible(m_presetTabs->currentIndex() == 1);
            m_batchApply->setEnabled(!targets.isEmpty() && m_presets->currentIndex() >= 0);
            if (m_controls.contains(QStringLiteral("recipe"))) m_controls[QStringLiteral("recipe")].row->setEnabled(true);
            selectEffectRecipe(m_pendingRecipe);
            loadTrackingFrame();
            return;
        }
        m_status->setText(QStringLiteral("Для нескольких клипов выберите сохранённый шаблон и примените его отдельной кнопкой."));
        int ambiguous = 0;
        for (const auto &stack : targets) if (effectsById(stack, m_assetId).size() > 1) ++ambiguous;
        const int applicable = targets.size() - ambiguous;
        m_batchApply->setText(QStringLiteral("Применить к %1 клипам").arg(applicable));
        m_batchApply->setToolTip(QStringLiteral("Исключено: заблокировано: %1, только аудио: %2, несколько экземпляров: %3")
                                     .arg(locked).arg(audio).arg(ambiguous));
        m_batchApply->setVisible(m_presetTabs->currentIndex() == 1);
        m_batchApply->setEnabled(applicable > 0 && m_presets->currentIndex() >= 0);
    } else {
        m_target->setText(QStringLiteral("Нет выбранного клипа"));
    }
    m_savePreset->setEnabled(false);
    m_replacePreset->setEnabled(false);
    m_applyPreset->setEnabled(m_presets->currentIndex() >= 0 && bool(m_stack));
    if (!m_stack) { loadTrackingFrame(); loadBackgroundFrame(); return; }
    auto refresh = [this] { QTimer::singleShot(0, this, &StudioPanel::refreshSelection); };
    m_connections << connect(m_stack.get(), &QAbstractItemModel::rowsInserted, this, refresh);
    m_connections << connect(m_stack.get(), &QAbstractItemModel::rowsRemoved, this, refresh);
    m_connections << connect(m_stack.get(), &QAbstractItemModel::modelReset, this, refresh);
    m_connections << connect(m_stack.get(), &QAbstractItemModel::layoutChanged, this, refresh);
    for (const auto &effect : effectsById(m_stack, m_assetId)) m_effects << effect;
    {
        QSignalBlocker block(m_instances);
        m_instances->clear();
        for (int i = 0; i < m_effects.size(); ++i) {
            const int slot = m_assetId == QLatin1String("studiofx") ? qRound(m_effects[i]->getParam(QStringLiteral("0")).toDouble() * 127.0) : -1;
            m_instances->addItem(m_assetId == QLatin1String("studiofx")
                                     ? QStringLiteral("%1: %2").arg(effectRecipeName(slot), QString::number(i + 1))
                                     : QStringLiteral("%1: %2").arg(m_title, QString::number(i + 1)));
        }
    }
    m_instances->setVisible(m_effects.size() > 1);
    if (m_assetId == QLatin1String("studiofx")) {
        m_add->show();
        m_duplicateEffect->setVisible(!m_effects.isEmpty());
        if (m_controls.contains(QStringLiteral("recipe"))) m_controls[QStringLiteral("recipe")].row->setEnabled(true);
        selectEffectRecipe(m_pendingRecipe);
    }
    int chosen = m_effects.indexOf(previous);
    if (chosen < 0 && m_effects.size() == 1) chosen = 0;
    { QSignalBlocker block(m_instances); m_instances->setCurrentIndex(chosen); }
    if (!supported()) {
        m_status->setText(KdenliveSettings::tenbitpipeline()
            ? QStringLiteral("Включён показ только 10-битных эффектов. Студия обрабатывает SDR 8 бит.")
            : QStringLiteral("Раздел доступен только в SDR-проекте Rec.709/601, 8 бит."));
        loadTrackingFrame();
        return;
    }
    if (m_effects.isEmpty()) {
        const bool available = EffectsRepository::get()->exists(m_assetId);
        const bool emptySequence = m_sequence && model && model->duration() <= 0;
        m_add->setVisible(available && !emptySequence);
        if (m_assetId == QLatin1String("studiofx")) selectEffectRecipe(m_pendingRecipe);
        else if (m_assetId == QLatin1String("studio_background"))
            m_add->setText(m_backgroundMethod == 0 ? QStringLiteral("Убрать фон") : QStringLiteral("Проанализировать и применить"));
        else m_add->setText(m_sequence ? QStringLiteral("Добавить камеру ко всей последовательности")
                                       : QStringLiteral("Добавить %1 к выбранному клипу").arg(m_title));
        m_status->setText(emptySequence ? QStringLiteral("Последовательность пуста. Добавьте клип перед камерой всей последовательности.")
                          : available ? QStringLiteral("Эффект ещё не добавлен. Примеры ниже не меняют проект.")
                                    : QStringLiteral("Эффект раздела не найден в этой сборке."));
        m_content->setEnabled(available && !emptySequence);
        for (auto it = m_controls.begin(); it != m_controls.end(); ++it) {
            bool hasPreview = false;
            for (auto button : it.value().choices) hasPreview |= !button->property("studioPreview").toString().isEmpty();
            it.value().row->setEnabled(hasPreview);
        }
        if (m_page == 2 && m_controls.contains(QStringLiteral("zoom"))) {
            auto &zoom = m_controls[QStringLiteral("zoom")];
            m_pendingTrackingZoom = zoom.spec.value(QStringLiteral("value")).toDouble();
            zoom.row->setEnabled(true);
            if (zoom.spin) { QSignalBlocker block(zoom.spin); zoom.spin->setValue(m_pendingTrackingZoom); }
            if (zoom.slider) {
                QSignalBlocker block(zoom.slider);
                zoom.slider->setValue(int(std::round(m_pendingTrackingZoom * std::pow(10, zoom.spin->decimals()))));
            }
        }
        loadTrackingFrame();
        loadBackgroundFrame();
    } else selectInstance(chosen);
}

void StudioPanel::selectInstance(int index)
{
    finishDrag();
    disconnect(m_parameterConnection);
    m_effect = index >= 0 && index < m_effects.size() ? m_effects[index] : nullptr;
    m_content->setEnabled(bool(m_effect) && supported());
    for (auto it = m_controls.begin(); it != m_controls.end(); ++it) it.value().row->setEnabled(bool(m_effect) && supported());
    for (auto button : {m_framing, m_cardPosition, m_effectCenter}) if (button) button->setEnabled(bool(m_effect) && supported());
    m_savePreset->setEnabled(bool(m_effect));
    m_replacePreset->setEnabled(bool(m_effect) && m_presets->currentIndex() >= 0);
    m_applyPreset->setEnabled(m_presets->currentIndex() >= 0 && bool(m_stack));
    m_status->setText(m_effect ? QStringLiteral("Изменения сразу видны в мониторе. Отмена: Ctrl+Z.") : QStringLiteral("Выберите экземпляр эффекта."));
    if (!m_effect) { loadTrackingFrame(); return; }
    if (m_assetId == QLatin1String("studiofx")) {
        selectEffectRecipe(qRound(m_effect->getParam(QStringLiteral("0")).toDouble() * 127.0));
        if (m_effectCenter) m_effectCenter->setEnabled(supported());
    }
    if (m_assetId == QLatin1String("studio_background")) {
        m_backgroundMethod = std::clamp(qRound(m_effect->getParam(QStringLiteral("method")).toDouble()), 0, 1);
        m_backgroundKey = QColor::fromRgbF(m_effect->getParam(QStringLiteral("key_r")).toDouble(),
                                           m_effect->getParam(QStringLiteral("key_g")).toDouble(),
                                           m_effect->getParam(QStringLiteral("key_b")).toDouble());
        m_backgroundFill = QColor::fromRgbF(m_effect->getParam(QStringLiteral("fill_r")).toDouble(),
                                            m_effect->getParam(QStringLiteral("fill_g")).toDouble(),
                                            m_effect->getParam(QStringLiteral("fill_b")).toDouble());
        { QSignalBlocker blocker(m_backgroundTabs); m_backgroundTabs->setCurrentIndex(m_backgroundMethod); }
        m_add->setVisible(m_backgroundMethod == 1);
        m_add->setText(QStringLiteral("Проанализировать заново"));
    }
    m_enabled->show();
    m_parameterConnection = connect(m_effect.get(), &QAbstractItemModel::dataChanged, this, [this] { refreshValues(); });
    if (m_assetId == QLatin1String("studio_camera") && !m_sequence) {
        const auto cards = effectsById(m_stack, QStringLiteral("card3d"));
        const auto camera = std::dynamic_pointer_cast<EffectItemModel>(m_effect);
        m_fixOrder->setVisible(camera && !cards.isEmpty() && camera->row() > cards.front()->row());
        if (m_fixOrder->isVisible()) m_status->setText(QStringLiteral("Камера стоит после карточки. Она двигает уже оформленный кадр."));
    }
    refreshValues();
    loadTrackingFrame();
    loadBackgroundFrame();
}

void StudioPanel::refreshValues()
{
    for (auto button : m_content->findChildren<QToolButton *>()) {
        if (!button->property("studioPresetValues").isValid()) continue;
        const auto values = button->property("studioPresetValues").toMap();
        bool selected = !values.isEmpty() && (bool(m_effect) || backgroundNeedsAnalysis());
        for (auto it = values.cbegin(); selected && it != values.cend(); ++it) {
            const double current = backgroundNeedsAnalysis() ? m_pendingBackgroundValues.value(it.key()).toDouble()
                                                              : m_effect->getParam(it.key()).toDouble();
            selected = std::abs(current - it.value().toDouble()) < 1e-8;
        }
        QSignalBlocker blocker(button);
        button->setChecked(selected);
    }
    if (!m_effect && m_assetId != QLatin1String("studio_transition")
        && !(m_assetId == QLatin1String("studio_background") && !m_pendingBackgroundValues.isEmpty())) return;
    if (const auto effect = std::dynamic_pointer_cast<EffectItemModel>(m_effect)) {
        QSignalBlocker blocker(m_enabled);
        m_enabled->setChecked(effect->isAssetEnabled());
        m_enabled->setText(effect->isAssetEnabled() ? QStringLiteral("✓ Эффект включён") : QStringLiteral("Эффект выключен"));
    }
    if (m_assetId == QLatin1String("studio_camera") && m_controls.contains(QStringLiteral("mode"))) {
        const bool cycle = int(std::round(displayedValue(m_controls[QStringLiteral("mode")].spec))) == 5;
        if (m_controls[QStringLiteral("zoom")].label)
            m_controls[QStringLiteral("zoom")].label->setText(cycle ? QStringLiteral("Максимальный масштаб") : QStringLiteral("Масштаб"));
    }
    QSet<int> studioFxControls;
    if (m_assetId == QLatin1String("studiofx")) {
        const int slot = std::clamp(qRound(m_effect->getParam(QStringLiteral("0")).toDouble() * 127.0), 0, int(m_effectRecipes.size()) - 1);
        for (const auto &value : m_effectRecipes[slot].toObject().value(QStringLiteral("controls")).toArray()) studioFxControls.insert(value.toInt());
    }
    for (auto it = m_controls.begin(); it != m_controls.end(); ++it) {
        auto &c = it.value();
        if (m_assetId == QLatin1String("studio_transition") && it.key() == QLatin1String("audio_dip")) {
            auto model = activeStudioModel();
            c.row->setVisible(model && model->studioTransitionSelection().audioMix);
        }
        if (m_assetId == QLatin1String("studio_transition") && it.key() == QLatin1String("sfx_level"))
            c.row->setVisible(displayedValue(m_controls[QStringLiteral("sfx_enabled")].spec) > .5);
        if (m_assetId == QLatin1String("studio_transition") && it.key() == QLatin1String("sfx_source")) {
            c.row->setVisible(displayedValue(m_controls[QStringLiteral("sfx_enabled")].spec) > .5);
            const QString source = m_effect ? m_effect->getParam(QStringLiteral("studio:sfx_source")) : m_transitionSoundSource;
            const int fixedCount = c.spec.value(QStringLiteral("options")).toArray().size();
            QSignalBlocker block(c.combo);
            while (c.combo->count() > fixedCount) c.combo->removeItem(c.combo->count() - 1);
            int selected = c.combo->findData(source);
            if (selected < 0) {
                QString name;
                if (source.startsWith(QLatin1String("bin:"))) {
                    const auto clip = pCore->projectItemModel()->getClipByBinID(source.mid(4));
                    name = clip ? QFileInfo(clip->url()).fileName() : QStringLiteral("Свой звук (файл недоступен)");
                } else if (source.startsWith(QLatin1String("file:"))) {
                    name = QFileInfo(QUrl(source).toLocalFile()).fileName();
                } else name = QStringLiteral("Недоступный звук");
                c.combo->addItem(QStringLiteral("Свой: %1").arg(name), source);
                selected = c.combo->count() - 1;
            }
            c.combo->setCurrentIndex(selected);
            continue;
        }
        if (m_assetId == QLatin1String("studio_camera")
            && (it.key() == QLatin1String("tracking") || it.key() == QLatin1String("track_path")
                || it.key() == QLatin1String("track_offset"))) {
            c.row->hide();
            continue;
        }
        if (m_assetId == QLatin1String("studiofx") && it.key() != QLatin1String("recipe"))
            c.row->setVisible(studioFxControls.contains(c.spec.value(QStringLiteral("index")).toInt()));
        const bool fileKind = c.spec.value(QStringLiteral("kind")).toString() == QLatin1String("file");
        const double value = fileKind ? 0.0 : displayedValue(c.spec);
        if (m_assetId == QLatin1String("studio_transition") && m_effect && it.key() != QLatin1String("duration"))
            c.spec.insert(QStringLiteral("value"), value);
        if (c.line) {
            QSignalBlocker block(c.line);
            const QString path = m_effect->getParam(parameterName(c.spec));
            c.line->setText(path);
            if (c.error) {
                const bool tracking = !m_controls.contains(QStringLiteral("tracking"))
                    || displayedValue(m_controls[QStringLiteral("tracking")].spec) > .5;
                const bool missing = tracking && (path.isEmpty() || !QFileInfo::exists(path));
                c.error->setText(path.isEmpty() ? QStringLiteral("Создайте трек или выберите существующий файл .scam.")
                                                : QStringLiteral("Файл трека не найден. Выберите его заново или создайте новый."));
                c.error->setVisible(missing);
            }
        }
        if (c.spin) { QSignalBlocker block(c.spin); c.spin->setValue(value); }
        if (c.slider && !c.slider->isSliderDown()) {
            QSignalBlocker block(c.slider); c.slider->setValue(int(std::round(value*std::pow(10, c.spin->decimals()))));
        }
        if (c.combo) { QSignalBlocker block(c.combo); c.combo->setCurrentIndex(int(std::round(value))); }
        if (c.check) { QSignalBlocker block(c.check); c.check->setChecked(value > .5); }
        for (int i = 0; i < c.choices.size(); ++i) {
            const bool selected = i == int(std::round(value));
            c.choices[i]->setChecked(selected);
            c.choices[i]->setText((selected ? QStringLiteral("✓ ") : QString()) + c.choices[i]->property("studioBaseText").toString());
        }
        const auto conditionValue = c.spec.value(QStringLiteral("visible_when"));
        if (!conditionValue.isUndefined()) {
            QJsonArray conditions = conditionValue.isArray() ? conditionValue.toArray() : QJsonArray{conditionValue};
            bool visible = true;
            for (const auto &value : conditions) {
                const auto condition = value.toObject();
                const auto other = m_controls.value(condition.value(QStringLiteral("key")).toString()).spec;
                const int choice = int(std::round(displayedValue(other)));
                if (condition.contains(QStringLiteral("not_value"))) visible &= choice != condition.value(QStringLiteral("not_value")).toInt();
                if (condition.contains(QStringLiteral("equals"))) visible &= choice == condition.value(QStringLiteral("equals")).toInt();
            }
            c.row->setVisible(visible);
        }
        if (c.spec.contains(QStringLiteral("visible_styles"))) {
            const int style = int(std::round(displayedValue(m_controls[QStringLiteral("style")].spec)));
            bool visible = false;
            for (const auto &candidate : c.spec.value(QStringLiteral("visible_styles")).toArray()) visible |= candidate.toInt() == style;
            c.row->setVisible(visible);
        }
    }
    if (m_assetId == QLatin1String("studiofx")) {
        for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
            bool available = it.key() == QLatin1String("catalog");
            for (const auto &key : it.value().spec.value(QStringLiteral("keys")).toArray()) {
                const auto control = m_controls.value(key.toString());
                available |= key.toString() == QLatin1String("recipe") || studioFxControls.contains(control.spec.value(QStringLiteral("index")).toInt());
            }
            if (it.value().section) it.value().section->setVisible(available);
        }
        const bool center = studioFxControls.contains(5) && studioFxControls.contains(6);
        if (m_effectCenter) m_effectCenter->setVisible(center && !m_effectCentering);
        if (m_effectCenterDone) m_effectCenterDone->setVisible(center && m_effectCentering);
    }
    if (m_framing && m_controls.contains(QStringLiteral("start_x"))) {
        const bool pan = int(std::round(displayedValue(m_controls[QStringLiteral("mode")].spec))) == 4;
        m_pointStart->setVisible(pan);
        m_swap->setVisible(pan);
        if (!pan) {
            m_editEnd = true;
            m_pointStart->setChecked(false);
            m_pointEnd->setChecked(true);
        }
        if (m_framingActive && m_monitorEffect == m_effect && !m_monitorCommitTimer->isActive()) updateFramingRect();
    }
    if (m_cardPositioning && m_monitorEffect == m_effect && !m_monitorCommitTimer->isActive()) updateCardPositionRect();
    if (m_effectCentering && m_monitorEffect == m_effect && !m_monitorCommitTimer->isActive()) updateEffectCenterRect();
    if (m_assetId == QLatin1String("studio_background")) {
        const int output = std::clamp(qRound(displayedValue(m_controls[QStringLiteral("output")].spec)), 0, 2);
        m_backgroundColor->setVisible(m_backgroundMethod == 0 || output == 2);
        const QColor color = m_backgroundMethod == 0 ? m_backgroundKey : m_backgroundFill;
        m_backgroundColor->setText(m_backgroundMethod == 0 ? QStringLiteral("Цвет фона")
                                                            : QStringLiteral("Цвет заливки"));
        m_backgroundColor->setToolTip(m_backgroundMethod == 0 ? QStringLiteral("Выберите цвет на стоп-кадре или нажмите здесь.") : QString());
        m_backgroundColor->setStyleSheet(QStringLiteral("text-align:left; border-left:24px solid %1;").arg(color.name()));
        m_pageSubtitle->setText(m_backgroundMethod == 0
            ? QStringLiteral("Щёлкните по однотонному фону на стоп-кадре и нажмите «Убрать фон».")
            : QStringLiteral("Фон человека рассчитывается заранее; по умолчанию он мягко размывается."));
    }
    for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
        const bool pageAllows = m_page == 2 ? it.key() == QLatin1String("tracking")
                                           : m_page != 1 || it.key() != QLatin1String("tracking");
        bool available = pageAllows && (!it.value().spec.value(QStringLiteral("clip_only")).toBool() || !m_sequence);
        if (available && m_assetId == QLatin1String("studio_background") && it.value().spec.contains(QStringLiteral("method")))
            available = it.value().spec.value(QStringLiteral("method")).toInt() == m_backgroundMethod;
        if (available && m_assetId == QLatin1String("studiofx") && it.key() != QLatin1String("catalog")) {
            available = false;
            for (const auto &key : it.value().spec.value(QStringLiteral("keys")).toArray())
                available |= studioFxControls.contains(m_controls.value(key.toString()).spec.value(QStringLiteral("index")).toInt());
        }
        if (it.value().section) it.value().section->setVisible(available);
        if (!available) continue;
        QString summary;
        if (m_assetId == QLatin1String("card3d") && it.key() == QLatin1String("placement")
            && int(std::round(displayedValue(m_controls[QStringLiteral("POSITION_MODE")].spec))) == 1)
            summary = QStringLiteral("Вручную");
        for (const auto &keyValue : it.value().spec.value(QStringLiteral("keys")).toArray()) {
            if (m_assetId == QLatin1String("studio_camera") && it.key() == QLatin1String("tracking")) break;
            if (!summary.isEmpty()) break;
            const auto key = keyValue.toString();
            const auto spec = m_controls[key].spec;
            if (spec.value(QStringLiteral("kind")).toString() == QLatin1String("choice")
                || spec.value(QStringLiteral("kind")).toString() == QLatin1String("list")) {
                summary = choiceName(spec, int(std::round(displayedValue(spec))));
                break;
            }
        }
        it.value().header->setText(it.value().spec.value(QStringLiteral("label")).toString()
                                   + (summary.isEmpty() ? QString() : QStringLiteral(" · ") + summary));
    }
    if (m_page == 2 && m_controls.contains(QStringLiteral("zoom")))
        m_pendingTrackingZoom = displayedValue(m_controls[QStringLiteral("zoom")].spec);
}

void StudioPanel::commit(const QString &key, double value)
{
    if (m_assetId == QLatin1String("studio_transition")) {
        auto &control = m_controls[key];
        const auto spec = control.spec;
        const double lo = spec.contains(QStringLiteral("lo")) ? spec.value(QStringLiteral("lo")).toDouble() : 0.0;
        const double hi = spec.contains(QStringLiteral("hi")) ? spec.value(QStringLiteral("hi")).toDouble()
                                                               : spec.value(QStringLiteral("options")).toArray().size() - 1.0;
        value = std::clamp(value, lo, hi);
        control.spec.insert(QStringLiteral("value"), value);
        if (!m_effect) { refreshValues(); return; }
        if (key == QLatin1String("duration")) {
            auto model = activeStudioModel();
            const int frames = qMax(1, qRound(value * pCore->getCurrentFps()));
            if (!model || !applyTransition(frames, {}))
                m_status->setText(QStringLiteral("Не удалось изменить длительность без нарушения соседних клипов."));
            refreshSelection();
            return;
        }
        if (key == QLatin1String("audio_dip") || key == QLatin1String("sfx_enabled") || key == QLatin1String("sfx_level")) {
            auto model = activeStudioModel();
            const int frames = model ? model->getMixDuration(m_transitionSecond) : 0;
            if (!model) { m_status->setText(QStringLiteral("Переход больше не выбран.")); return; }
            if (!applyTransition(frames, {{parameterName(spec), storedValue(spec, value)}})) {
                refreshValues();
                return;
            }
            return;
        }
        auto model = activeStudioModel();
        const int frames = model ? model->getMixDuration(m_transitionSecond) : 0;
        if (!model || !applyTransition(frames, {{parameterName(spec), storedValue(spec, value)}}))
            m_status->setText(QStringLiteral("Не удалось изменить переход."));
        refreshSelection();
        return;
    }
    if (key == QLatin1String("zoom") && m_page == 2 && !m_effect && currentTarget() && supported()) {
        const auto spec = m_controls[key].spec;
        value = std::clamp(value, spec.value(QStringLiteral("lo")).toDouble(), spec.value(QStringLiteral("hi")).toDouble());
        m_pendingTrackingZoom = value;
        auto &control = m_controls[key];
        if (control.spin) { QSignalBlocker block(control.spin); control.spin->setValue(value); }
        if (control.slider && !control.slider->isSliderDown()) {
            QSignalBlocker block(control.slider);
            control.slider->setValue(int(std::round(value * std::pow(10, control.spin->decimals()))));
        }
        return;
    }
    if (backgroundNeedsAnalysis() && currentTarget() && supported()) {
        const auto spec = m_controls[key].spec;
        const double lo = spec.contains(QStringLiteral("lo")) ? spec.value(QStringLiteral("lo")).toDouble() : 0.0;
        const double hi = spec.contains(QStringLiteral("hi")) ? spec.value(QStringLiteral("hi")).toDouble()
                                                               : spec.value(QStringLiteral("options")).toArray().size() - 1.0;
        value = std::clamp(value, lo, hi);
        m_pendingBackgroundValues.insert(parameterName(spec), storedValue(spec, value).toDouble());
        refreshValues();
        return;
    }
    if (!m_effect || !currentTarget() || !supported()) { refreshSelection(); return; }
    const auto spec = m_controls[key].spec;
    const double lo = spec.contains(QStringLiteral("lo")) ? spec.value(QStringLiteral("lo")).toDouble() : 0.0;
    const double hi = spec.contains(QStringLiteral("hi")) ? spec.value(QStringLiteral("hi")).toDouble()
                                                           : spec.value(QStringLiteral("options")).toArray().size()-1.0;
    value = std::clamp(value, lo, hi);
    const auto name = parameterName(spec);
    const auto after = storedValue(spec, value);
    const auto before = m_effect->getParam(name);
    if (m_assetId == QLatin1String("studio_camera") && key == QLatin1String("timing") && value == 0.0) {
        const QStringList names{name, QStringLiteral("studio_time_origin"), QStringLiteral("studio_time_span")};
        const QStringList old{before,
                              m_effect->getParam(names[1]),
                              m_effect->getParam(names[2])};
        const QStringList reset{after, QStringLiteral("0"), QStringLiteral("0")};
        if (old != reset) pCore->pushUndo(new StudioCommand(m_effect, names, old, reset));
        refreshValues();
        return;
    }
    QStringList names{name}, old{before}, next{after};
    if (m_assetId == QLatin1String("card3d") && key == QLatin1String("PLACEMENT")) {
        static const double positions[7][2] = {{50, 50}, {0, 50}, {100, 50}, {0, 0}, {100, 0}, {0, 100}, {100, 100}};
        const int placement = std::clamp(int(std::round(value)), 0, 6);
        const auto mode = m_controls[QStringLiteral("POSITION_MODE")].spec;
        const auto x = m_controls[QStringLiteral("X")].spec;
        const auto y = m_controls[QStringLiteral("Y")].spec;
        names << parameterName(mode) << parameterName(x) << parameterName(y);
        old << m_effect->getParam(names[1]) << m_effect->getParam(names[2]) << m_effect->getParam(names[3]);
        next << storedValue(mode, 0) << storedValue(x, positions[placement][0]) << storedValue(y, positions[placement][1]);
    } else if (m_assetId == QLatin1String("card3d") && (key == QLatin1String("X") || key == QLatin1String("Y"))) {
        const auto mode = m_controls[QStringLiteral("POSITION_MODE")].spec;
        names.prepend(parameterName(mode));
        old.prepend(m_effect->getParam(names[0]));
        next.prepend(storedValue(mode, 1));
    }
    if (old == next) return;
    if (m_dragEffect == m_effect && m_dragNames == names) {
        m_dragAfter = next;
        for (int i = 0; i < names.size(); ++i) m_effect->setParameter(names[i], next[i], true);
    } else pCore->pushUndo(new StudioCommand(m_effect, names, old, next));
    refreshValues();
}

void StudioPanel::commitText(const QString &key, const QString &value)
{
    if (!m_effect || !currentTarget() || !supported() || !m_controls.contains(key)) { refreshSelection(); return; }
    const QString name = parameterName(m_controls[key].spec);
    const QString before = m_effect->getParam(name);
    const QString trimmed = value.trimmed();
    const QUrl url(trimmed);
    const QString after = trimmed.isEmpty() ? QString() : url.isLocalFile() ? QDir::cleanPath(url.toLocalFile()) : QDir::cleanPath(trimmed);
    if (before == after) return;
    pCore->pushUndo(new StudioCommand(m_effect, {name}, {before}, {after}));
    refreshValues();
}

void StudioPanel::finishDrag()
{
    auto effect = std::move(m_dragEffect);
    if (effect && m_dragBefore != m_dragAfter) {
        if (auto undo = m_dragUndo.lock())
            undo->push(new StudioCommand(effect, m_dragNames, m_dragBefore, m_dragAfter));
    }
    m_dragUndo.reset();
    m_dragNames.clear();
    m_dragBefore.clear();
    m_dragAfter.clear();
}

void StudioPanel::resetGroup(const QString &key)
{
    finishDrag();
    if (m_assetId == QLatin1String("studio_transition")) {
        if (!m_groups.contains(key)) return;
        for (const auto &controlKey : m_groups[key].spec.value(QStringLiteral("keys")).toArray()) {
            auto &spec = m_controls[controlKey.toString()].spec;
            spec.insert(QStringLiteral("value"), spec.value(QStringLiteral("key")) == QLatin1String("duration") ? .65
                                                                                                                   : spec.value(QStringLiteral("value")));
            if (controlKey.toString() == QLatin1String("direction")) spec.insert(QStringLiteral("value"), 0);
            if (controlKey.toString() == QLatin1String("strength")) spec.insert(QStringLiteral("value"), 45);
            if (controlKey.toString() == QLatin1String("softness")) spec.insert(QStringLiteral("value"), 35);
            if (controlKey.toString() == QLatin1String("audio_dip")) spec.insert(QStringLiteral("value"), 25);
            if (controlKey.toString() == QLatin1String("sfx_enabled")) spec.insert(QStringLiteral("value"), 0);
            if (controlKey.toString() == QLatin1String("sfx_level")) spec.insert(QStringLiteral("value"), 50);
            if (controlKey.toString() == QLatin1String("sfx_source")) m_transitionSoundSource.clear();
            if (controlKey.toString() == QLatin1String("style")) spec.insert(QStringLiteral("value"), 0);
        }
        if (m_effect) {
            auto model = activeStudioModel();
            const int frames = qMax(1, qRound(m_controls[QStringLiteral("duration")].spec.value(QStringLiteral("value")).toDouble() * pCore->getCurrentFps()));
            if (!model || !applyTransition(frames, transitionParameters(frames))) {
                m_status->setText(QStringLiteral("Не удалось сбросить настройки перехода."));
                return;
            }
            refreshSelection();
        } else refreshValues();
        return;
    }
    if (backgroundNeedsAnalysis() && currentTarget() && supported()) {
        if (!m_groups.contains(key)) return;
        for (const auto &controlKey : m_groups[key].spec.value(QStringLiteral("keys")).toArray()) {
            const auto spec = m_controls[controlKey.toString()].spec;
            m_pendingBackgroundValues.insert(parameterName(spec), spec.value(QStringLiteral("value")).toDouble());
        }
        refreshValues();
        return;
    }
    if (!m_effect || !currentTarget() || !supported()) { refreshSelection(); return; }
    QStringList names, before, after;
    if (!m_groups.contains(key)) return;
    const int studioFxSlot = m_assetId == QLatin1String("studiofx")
        ? std::clamp(qRound(m_effect->getParam(QStringLiteral("0")).toDouble() * 127.0), 0, int(m_effectRecipes.size()) - 1) : -1;
    const auto studioFxDefaults = studioFxSlot >= 0 ? m_effectRecipes[studioFxSlot].toObject().value(QStringLiteral("values")).toArray() : QJsonArray();
    for (const auto &controlKey : m_groups[key].spec.value(QStringLiteral("keys")).toArray()) {
        const auto spec = m_controls[controlKey.toString()].spec;
        names << parameterName(spec);
        before << m_effect->getParam(names.last());
        const int index = spec.value(QStringLiteral("index")).toInt(-1);
        after << (studioFxSlot >= 0 && index >= 0 && index < studioFxDefaults.size()
                      ? QString::number(studioFxDefaults[index].toDouble(), 'g', 17)
                      : spec.value(QStringLiteral("kind")).toString() == QLatin1String("file")
                      ? spec.value(QStringLiteral("value")).toString()
                      : storedValue(spec, spec.value(QStringLiteral("value")).toDouble()));
    }
    if (before != after) pCore->pushUndo(new StudioCommand(m_effect, names, before, after));
}

void StudioPanel::loadPresets()
{
    const QString selectedPath = m_presets->currentData().toString();
    QSignalBlocker blocker(m_presets);
    m_presets->clear();
    if (m_assetId == QLatin1String("studio_transition")) {
        QDir dir(transitionPresetFolder());
        for (const auto &fileName : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            const QString path = dir.absoluteFilePath(fileName);
            const auto preset = readTransitionPreset(path);
            if (preset.isEmpty()) continue;
            m_presets->addItem(preset.value(QStringLiteral("name")).toString(QFileInfo(path).completeBaseName()), path);
        }
        int selected = m_presets->findData(selectedPath);
        if (selected < 0 && m_presets->count() > 0) selected = 0;
        m_presets->setCurrentIndex(selected);
        const bool available = selected >= 0;
        m_applyPreset->setEnabled(available && m_transitionFirst > -1);
        m_replacePreset->setEnabled(available);
        m_renamePreset->setEnabled(available);
        m_deletePreset->setEnabled(available);
        return;
    }
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/effects"));
    for (const auto &fileName : dir.entryList({QStringLiteral("*.xml")}, QDir::Files, QDir::Name)) {
        const QString path = dir.absoluteFilePath(fileName);
        QFile file(path);
        QDomDocument document;
        if (!file.open(QIODevice::ReadOnly) || !document.setContent(&file)) continue;
        const QDomElement root = document.documentElement();
        const QString tag = root.attribute(QStringLiteral("tag"));
        const bool matches = root.attribute(QStringLiteral("studio_base")) == m_assetId
            || (m_assetId == QLatin1String("card3d") && tag == QLatin1String("frei0r.card3d")
                && root.attribute(QStringLiteral("type")).startsWith(QLatin1String("custom")))
            || (m_assetId == QLatin1String("studio_camera") && tag == QLatin1String("studio.camera")
                && root.attribute(QStringLiteral("type")).startsWith(QLatin1String("custom")))
            || (m_assetId == QLatin1String("studio_color") && tag == QLatin1String("studio.color")
                && root.attribute(QStringLiteral("type")).startsWith(QLatin1String("custom")))
            || (m_assetId == QLatin1String("studio_background") && tag == QLatin1String("studio.background")
                && root.attribute(QStringLiteral("type")).startsWith(QLatin1String("custom")));
        if (!matches) continue;
        QString name = root.firstChildElement(QStringLiteral("name")).text().trimmed();
        if (name.isEmpty()) name = QFileInfo(path).completeBaseName();
        m_presets->addItem(name, path);
        m_presets->setItemData(m_presets->count() - 1, root.firstChildElement(QStringLiteral("description")).text(), Qt::ToolTipRole);
    }
    int selected = m_presets->findData(selectedPath);
    if (selected < 0 && m_presets->count() > 0) selected = 0;
    m_presets->setCurrentIndex(selected);
    const bool available = selected >= 0;
    m_applyPreset->setEnabled(available && bool(m_stack));
    m_replacePreset->setEnabled(available && bool(m_effect));
    m_renamePreset->setEnabled(available);
    m_deletePreset->setEnabled(available);
}

QMap<QString, QString> StudioPanel::presetValues(const QString &path) const
{
    QMap<QString, QString> values;
    if (m_assetId == QLatin1String("studio_transition")) {
        const auto preset = readTransitionPreset(path);
        if (preset.isEmpty()) return {};
        values.insert(QStringLiteral("1"), storedValue(m_controls[QStringLiteral("style")].spec, preset.value(QStringLiteral("style")).toInt()));
        values.insert(QStringLiteral("2"), QString::number(preset.value(QStringLiteral("strength")).toDouble(.45), 'g', 17));
        values.insert(QStringLiteral("3"), QString::number(preset.value(QStringLiteral("direction")).toInt() / 3.0, 'g', 17));
        values.insert(QStringLiteral("4"), QString::number(preset.value(QStringLiteral("softness")).toDouble(.35), 'g', 17));
        values.insert(QStringLiteral("duration"), QString::number(preset.value(QStringLiteral("duration")).toDouble(.65), 'g', 17));
        values.insert(QStringLiteral("studio:audio_dip"), QString::number(preset.value(QStringLiteral("audio_dip")).toDouble(), 'g', 17));
        values.insert(QStringLiteral("studio:sfx_enabled"), QString::number(preset.value(QStringLiteral("sfx_enabled")).toInt()));
        values.insert(QStringLiteral("studio:sfx_level"), QString::number(preset.value(QStringLiteral("sfx_level")).toDouble(.5), 'g', 17));
        values.insert(QStringLiteral("studio:sfx_source"), preset.value(QStringLiteral("sfx_source")).toString());
        return values;
    }
    QFile file(path);
    QDomDocument document;
    if (!file.open(QIODevice::ReadOnly) || !document.setContent(&file)) return values;
    const QDomElement root = document.documentElement();
    const QString tag = root.attribute(QStringLiteral("tag"));
    if ((m_assetId == QLatin1String("card3d") && tag != QLatin1String("frei0r.card3d"))
        || (m_assetId == QLatin1String("studio_camera") && tag != QLatin1String("studio.camera"))
        || (m_assetId == QLatin1String("studio_color") && tag != QLatin1String("studio.color"))
        || (m_assetId == QLatin1String("studio_background") && tag != QLatin1String("studio.background"))
        || (m_assetId == QLatin1String("studiofx") && tag != QLatin1String("frei0r.studiofx"))) return {};
    const auto parameters = root.elementsByTagName(QStringLiteral("parameter"));
    for (int i = 0; i < parameters.count(); ++i) {
        const auto parameter = parameters.at(i).toElement();
        if (!parameter.hasAttribute(QStringLiteral("value"))) continue;
        values.insert(parameter.attribute(QStringLiteral("name")), parameter.attribute(QStringLiteral("value")));
    }
    if (m_assetId == QLatin1String("card3d")) {
        if (!values.contains(QStringLiteral("8"))) {
            const double style = values.value(QStringLiteral("0"), QStringLiteral("0")).toDouble();
            values.insert(QStringLiteral("8"), style < .25 ? QStringLiteral("0.25") : style < .75 ? QStringLiteral("0.04") : QStringLiteral("0"));
        }
        if (!values.contains(QStringLiteral("9"))) values.insert(QStringLiteral("9"), QStringLiteral("0"));
        if (!values.contains(QStringLiteral("10"))) values.insert(QStringLiteral("10"), QStringLiteral("0.35"));
        if (!values.contains(QStringLiteral("11"))) values.insert(QStringLiteral("11"), QStringLiteral("0"));
        if (!values.contains(QStringLiteral("12")) || !values.contains(QStringLiteral("13"))) {
            static const double positions[7][2] = {{.5, .5}, {0, .5}, {1, .5}, {0, 0}, {1, 0}, {0, 1}, {1, 1}};
            const int placement = std::clamp(int(std::round(values.value(QStringLiteral("3"), QStringLiteral("0.16666666666666666")).toDouble() * 6)), 0, 6);
            values.insert(QStringLiteral("12"), QString::number(positions[placement][0], 'g', 17));
            values.insert(QStringLiteral("13"), QString::number(positions[placement][1], 'g', 17));
        }
        if (!values.contains(QStringLiteral("14"))) values.insert(QStringLiteral("14"), QStringLiteral("1"));
        if (!values.contains(QStringLiteral("15"))) values.insert(QStringLiteral("15"), QStringLiteral("0"));
        if (!values.contains(QStringLiteral("16"))) values.insert(QStringLiteral("16"), QStringLiteral("0.34"));
    }
    if (m_assetId == QLatin1String("studio_camera")) {
        if (!values.contains(QStringLiteral("tracking"))) values.insert(QStringLiteral("tracking"), QStringLiteral("0"));
        if (!values.contains(QStringLiteral("track_path"))) values.insert(QStringLiteral("track_path"), QString());
        if (!values.contains(QStringLiteral("track_offset"))) values.insert(QStringLiteral("track_offset"), QStringLiteral("0"));
    }
    return values;
}

void StudioPanel::savePreset(bool replace)
{
    if (m_assetId == QLatin1String("studio_transition")) {
        QString path;
        QString name;
        if (replace && m_presets->currentIndex() >= 0) {
            path = m_presets->currentData().toString();
            name = m_presets->currentText();
        } else {
            bool accepted = false;
            name = QInputDialog::getText(this, QStringLiteral("Сохранить шаблон"), QStringLiteral("Название:"), QLineEdit::Normal, {}, &accepted).simplified();
            if (!accepted || name.isEmpty()) return;
            QDir dir(transitionPresetFolder());
            if (!dir.mkpath(QStringLiteral("."))) {
                m_status->setText(QStringLiteral("Не удалось создать каталог пользовательских шаблонов."));
                return;
            }
            path = dir.absoluteFilePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
        }
        QString soundSource = m_effect ? m_effect->getParam(QStringLiteral("studio:sfx_source")) : m_transitionSoundSource;
        if (soundSource.startsWith(QLatin1String("bin:"))) {
            const auto clip = pCore->projectItemModel()->getClipByBinID(soundSource.mid(4));
            if (!clip || !QFileInfo(clip->url()).isFile()) {
                m_status->setText(QStringLiteral("Свой звук недоступен. Выберите файл заново перед сохранением шаблона."));
                return;
            }
            soundSource = QUrl::fromLocalFile(clip->url()).toString(QUrl::FullyEncoded);
        }
        QJsonObject preset{{QStringLiteral("schema"), 1}, {QStringLiteral("name"), name},
                           {QStringLiteral("style"), qRound(displayedValue(m_controls[QStringLiteral("style")].spec))},
                           {QStringLiteral("duration"), m_controls[QStringLiteral("duration")].spec.value(QStringLiteral("value")).toDouble(.65)},
                           {QStringLiteral("direction"), qRound(displayedValue(m_controls[QStringLiteral("direction")].spec))},
                           {QStringLiteral("strength"), storedValue(m_controls[QStringLiteral("strength")].spec,
                                                                    displayedValue(m_controls[QStringLiteral("strength")].spec)).toDouble()},
                           {QStringLiteral("softness"), storedValue(m_controls[QStringLiteral("softness")].spec,
                                                                    displayedValue(m_controls[QStringLiteral("softness")].spec)).toDouble()},
                           {QStringLiteral("audio_dip"), storedValue(m_controls[QStringLiteral("audio_dip")].spec,
                                                                     displayedValue(m_controls[QStringLiteral("audio_dip")].spec)).toDouble()},
                           {QStringLiteral("sfx_enabled"), qRound(displayedValue(m_controls[QStringLiteral("sfx_enabled")].spec))},
                           {QStringLiteral("sfx_source"), soundSource},
                           {QStringLiteral("sfx_level"), storedValue(m_controls[QStringLiteral("sfx_level")].spec,
                                                                     displayedValue(m_controls[QStringLiteral("sfx_level")].spec)).toDouble()}};
        if (!writeJsonAtomically(preset, path)) {
            m_status->setText(QStringLiteral("Не удалось сохранить шаблон. Прежний файл не изменён."));
            return;
        }
        loadPresets();
        const int current = m_presets->findData(path);
        if (current >= 0) m_presets->setCurrentIndex(current);
        m_presetTabs->setCurrentIndex(1);
        m_presetBar->show();
        m_status->setText(replace ? QStringLiteral("Шаблон заменён.") : QStringLiteral("Шаблон сохранён."));
        return;
    }
    if (!m_effect) {
        m_status->setText(QStringLiteral("Сначала добавьте или выберите эффект, настройки которого нужно сохранить."));
        return;
    }
    QString path;
    QString name;
    QString id;
    if (replace && m_presets->currentIndex() >= 0) {
        path = m_presets->currentData().toString();
        name = m_presets->currentText();
        QFile oldFile(path);
        QDomDocument oldDocument;
        if (oldFile.open(QIODevice::ReadOnly) && oldDocument.setContent(&oldFile)) id = oldDocument.documentElement().attribute(QStringLiteral("id"));
    } else {
        bool accepted = false;
        name = QInputDialog::getText(this, QStringLiteral("Сохранить шаблон"), QStringLiteral("Название:"), QLineEdit::Normal, {}, &accepted).simplified();
        if (!accepted || name.isEmpty()) return;
        id = QStringLiteral("studio_%1_%2").arg(m_assetId, QUuid::createUuid().toString(QUuid::WithoutBraces));
        QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/effects"));
        if (!dir.mkpath(QStringLiteral("."))) {
            m_status->setText(QStringLiteral("Не удалось создать каталог пользовательских шаблонов."));
            return;
        }
        path = dir.absoluteFilePath(id + QStringLiteral(".xml"));
    }
    if (id.isEmpty()) id = QFileInfo(path).completeBaseName();
    QDomDocument document;
    const auto base = EffectsRepository::get()->getXml(m_assetId);
    if (base.isNull()) {
        m_status->setText(QStringLiteral("Не найдено базовое описание эффекта."));
        return;
    }
    document.appendChild(document.importNode(base, true));
    auto root = document.documentElement();
    root.setAttribute(QStringLiteral("id"), id);
    root.setAttribute(QStringLiteral("type"), QStringLiteral("customVideo"));
    root.setAttribute(QStringLiteral("studio_base"), m_assetId);
    root.removeAttribute(QStringLiteral("kdenlive_ix"));
    auto nameNode = root.firstChildElement(QStringLiteral("name"));
    if (nameNode.isNull()) {
        nameNode = document.createElement(QStringLiteral("name"));
        root.insertBefore(nameNode, root.firstChild());
    }
    while (!nameNode.firstChild().isNull()) nameNode.removeChild(nameNode.firstChild());
    nameNode.appendChild(document.createTextNode(name));
    const auto parameters = root.elementsByTagName(QStringLiteral("parameter"));
    for (int i = 0; i < parameters.count(); ++i) {
        auto parameter = parameters.at(i).toElement();
        const QString parameterName = parameter.attribute(QStringLiteral("name"));
        if (m_assetId == QLatin1String("studio_background") && m_backgroundParameters.contains(parameterName)) {
            parameter.setAttribute(QStringLiteral("value"), m_effect->getParam(parameterName));
            continue;
        }
        if (m_assetId == QLatin1String("studio_camera")
            && (parameterName == QLatin1String("tracking") || parameterName == QLatin1String("track_path")
                || parameterName == QLatin1String("track_offset"))) continue;
        for (auto it = m_controls.cbegin(); it != m_controls.cend(); ++it) {
            if (this->parameterName(it.value().spec) == parameterName) {
                parameter.setAttribute(QStringLiteral("value"), m_effect->getParam(parameterName));
                break;
            }
        }
    }
    if (!writeXmlAtomically(document, path)) {
        m_status->setText(QStringLiteral("Не удалось сохранить шаблон. Прежний файл не изменён."));
        return;
    }
    EffectsRepository::get()->reloadCustom(path);
    loadPresets();
    const int current = m_presets->findData(path);
    if (current >= 0) m_presets->setCurrentIndex(current);
    m_presetTabs->setCurrentIndex(1);
    m_presetBar->show();
    m_status->setText(replace ? QStringLiteral("Шаблон заменён.") : QStringLiteral("Шаблон сохранён."));
}

void StudioPanel::renamePreset()
{
    const QString path = m_presets->currentData().toString();
    if (path.isEmpty()) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Переименовать шаблон"), QStringLiteral("Название:"), QLineEdit::Normal,
                                               m_presets->currentText(), &accepted).simplified();
    if (!accepted || name.isEmpty()) return;
    if (m_assetId == QLatin1String("studio_transition")) {
        auto preset = readTransitionPreset(path);
        if (preset.isEmpty()) { m_status->setText(QStringLiteral("Не удалось прочитать шаблон.")); return; }
        preset.insert(QStringLiteral("name"), name);
        if (!writeJsonAtomically(preset, path)) {
            m_status->setText(QStringLiteral("Не удалось переименовать шаблон. Прежний файл не изменён."));
            return;
        }
        loadPresets();
        const int current = m_presets->findData(path);
        if (current >= 0) m_presets->setCurrentIndex(current);
        return;
    }
    QFile file(path);
    QDomDocument document;
    if (!file.open(QIODevice::ReadOnly) || !document.setContent(&file)) {
        m_status->setText(QStringLiteral("Не удалось прочитать шаблон."));
        return;
    }
    auto nameNode = document.documentElement().firstChildElement(QStringLiteral("name"));
    while (!nameNode.firstChild().isNull()) nameNode.removeChild(nameNode.firstChild());
    nameNode.appendChild(document.createTextNode(name));
    if (!writeXmlAtomically(document, path)) {
        m_status->setText(QStringLiteral("Не удалось переименовать шаблон. Прежний файл не изменён."));
        return;
    }
    EffectsRepository::get()->reloadCustom(path);
    loadPresets();
    const int current = m_presets->findData(path);
    if (current >= 0) m_presets->setCurrentIndex(current);
}

void StudioPanel::deletePreset()
{
    const QString path = m_presets->currentData().toString();
    if (path.isEmpty()) return;
    if (QMessageBox::question(this, QStringLiteral("Удалить шаблон"), QStringLiteral("Удалить «%1»? Проекты, где он уже применён, не изменятся.")
                                  .arg(m_presets->currentText())) != QMessageBox::Yes) return;
    if (m_assetId == QLatin1String("studio_transition")) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            m_status->setText(QStringLiteral("Не удалось удалить файл шаблона."));
            return;
        }
        loadPresets();
        m_status->setText(QStringLiteral("Шаблон удалён. Проекты не изменены."));
        return;
    }
    QFile file(path);
    QDomDocument document;
    QString id = QFileInfo(path).completeBaseName();
    if (file.open(QIODevice::ReadOnly) && document.setContent(&file)) id = document.documentElement().attribute(QStringLiteral("id"), id);
    EffectsRepository::get()->deleteEffect(id);
    if (QFile::exists(path) && !QFile::remove(path)) {
        m_status->setText(QStringLiteral("Не удалось удалить файл шаблона."));
        return;
    }
    loadPresets();
    m_status->setText(QStringLiteral("Шаблон удалён. Проекты не изменены."));
}

void StudioPanel::applySelectedPreset(bool batch)
{
    const auto values = presetValues(m_presets->currentData().toString());
    if (m_assetId == QLatin1String("studio_transition")) {
        if (batch || !values.contains(QStringLiteral("duration")) || !values.contains(QStringLiteral("1"))
            || !values.contains(QStringLiteral("2")) || !values.contains(QStringLiteral("3")) || !values.contains(QStringLiteral("4"))) {
            m_status->setText(QStringLiteral("Шаблон неполный или несовместим с этой версией."));
            return;
        }
        m_controls[QStringLiteral("style")].spec.insert(QStringLiteral("value"), [&] {
            const auto spec = m_controls[QStringLiteral("style")].spec;
            double nearest = 2;
            int style = 0;
            for (const auto &entry : spec.value(QStringLiteral("options")).toArray()) {
                const auto option = entry.toObject();
                const double difference = std::abs(values.value(QStringLiteral("1")).toDouble() - option.value(QStringLiteral("code")).toDouble());
                if (difference < nearest) { nearest = difference; style = option.value(QStringLiteral("value")).toInt(); }
            }
            return style;
        }());
        m_controls[QStringLiteral("strength")].spec.insert(QStringLiteral("value"), values.value(QStringLiteral("2")).toDouble() * 100.0);
        m_controls[QStringLiteral("direction")].spec.insert(QStringLiteral("value"), values.value(QStringLiteral("3")).toDouble() * 3.0);
        m_controls[QStringLiteral("softness")].spec.insert(QStringLiteral("value"), values.value(QStringLiteral("4")).toDouble() * 100.0);
        m_controls[QStringLiteral("duration")].spec.insert(QStringLiteral("value"), values.value(QStringLiteral("duration")).toDouble());
        m_controls[QStringLiteral("audio_dip")].spec.insert(QStringLiteral("value"), values.value(QStringLiteral("studio:audio_dip")).toDouble() * 100.0);
        m_controls[QStringLiteral("sfx_enabled")].spec.insert(QStringLiteral("value"), values.value(QStringLiteral("studio:sfx_enabled")).toDouble());
        m_controls[QStringLiteral("sfx_level")].spec.insert(QStringLiteral("value"), values.value(QStringLiteral("studio:sfx_level")).toDouble() * 100.0);
        m_transitionSoundSource = values.value(QStringLiteral("studio:sfx_source"));
        auto model = activeStudioModel();
        if (!model || m_transitionFirst < 0 || !supported()) {
            m_status->setText(QStringLiteral("Нет подходящего стыка для применения шаблона."));
            return;
        }
        const int frames = qMax(1, qRound(values.value(QStringLiteral("duration")).toDouble() * pCore->getCurrentFps()));
        const QString previousStatus = m_status->text();
        const bool ok = applyTransition(frames, transitionParameters(frames));
        if (!ok) {
            if (m_status->text() == previousStatus)
                m_status->setText(QStringLiteral("Не удалось применить шаблон к этому стыку."));
            return;
        }
        refreshSelection();
        m_status->setText(QStringLiteral("Шаблон перехода применён одним действием."));
        return;
    }
    QStringList names, next;
    if (m_assetId == QLatin1String("studio_background")) {
        if (batch) {
            m_status->setText(QStringLiteral("Удаление фона применяется только к одному выбранному клипу."));
            return;
        }
        for (const auto &name : m_backgroundParameters) {
            if (!values.contains(name)) {
                m_status->setText(QStringLiteral("Шаблон неполный или несовместим с этой версией."));
                return;
            }
            names << name;
            next << values.value(name);
        }
        const int method = values.value(QStringLiteral("method"), QStringLiteral("0")).toInt();
        if (method == 1 && (!m_effect || m_effect->getParam(QStringLiteral("mask_asset")).isEmpty())) {
            m_pendingBackgroundValues = QJsonObject();
            for (int i = 0; i < names.size(); ++i) m_pendingBackgroundValues.insert(names[i], next[i].toDouble());
            m_backgroundMethod = 1;
            m_backgroundKey = QColor::fromRgbF(m_pendingBackgroundValues.value(QStringLiteral("key_r")).toDouble(),
                                                m_pendingBackgroundValues.value(QStringLiteral("key_g")).toDouble(),
                                                m_pendingBackgroundValues.value(QStringLiteral("key_b")).toDouble());
            m_backgroundFill = QColor::fromRgbF(m_pendingBackgroundValues.value(QStringLiteral("fill_r")).toDouble(),
                                                 m_pendingBackgroundValues.value(QStringLiteral("fill_g")).toDouble(),
                                                 m_pendingBackgroundValues.value(QStringLiteral("fill_b")).toDouble());
            { QSignalBlocker blocker(m_backgroundTabs); m_backgroundTabs->setCurrentIndex(1); }
            m_add->show();
            m_add->setText(QStringLiteral("Проанализировать и применить"));
            refreshValues();
            paintBackgroundFrame();
            m_status->setText(QStringLiteral("Шаблон подготовлен. Проект не изменён; выполните анализ человека."));
            return;
        }
    } else for (auto it = m_controls.cbegin(); it != m_controls.cend(); ++it) {
        const QString name = parameterName(it.value().spec);
        if (m_assetId == QLatin1String("studio_camera")
            && (name == QLatin1String("tracking") || name == QLatin1String("track_path") || name == QLatin1String("track_offset"))) continue;
        if (!values.contains(name)) {
            m_status->setText(QStringLiteral("Шаблон неполный или несовместим с этой версией."));
            return;
        }
        names << name;
        next << values.value(name);
    }
    QList<std::shared_ptr<EffectStackModel>> targets;
    int excludedLocked = 0, excludedAudio = 0, excludedAmbiguous = 0;
    if (batch) targets = batchVideoStacks(&excludedLocked, &excludedAudio);
    else if (m_stack) targets << m_stack;
    if (targets.isEmpty()) {
        m_status->setText(QStringLiteral("Нет подходящих целей для применения шаблона."));
        return;
    }
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    int changed = 0;
    for (const auto &stack : targets) {
        auto effects = effectsById(stack, m_assetId);
        if (effects.size() > 1) {
            ++excludedAmbiguous;
            if (!batch) {
                m_status->setText(QStringLiteral("Выберите один экземпляр эффекта перед применением шаблона."));
                return;
            }
            continue;
        }
        if (effects.isEmpty()) {
            if (!stack->appendEffectWithUndo(m_assetId, undo, redo).first) {
                undo();
                m_status->setText(QStringLiteral("Применение отменено: не удалось добавить эффект к одной из целей."));
                return;
            }
            effects = effectsById(stack, m_assetId);
            const auto added = effects.isEmpty() ? std::shared_ptr<EffectItemModel>() : effects.back();
            if (effects.size() != 1 || (m_assetId == QLatin1String("studio_camera") && !moveCameraBeforeCard(stack, undo, redo))
                || (m_assetId == QLatin1String("studio_background") && !moveBackgroundBeforeStudio(stack, added, undo, redo))
                || (m_assetId == QLatin1String("studio_color") && !moveColorBeforeStudio(stack, effects.front(), undo, redo))
                || (m_assetId == QLatin1String("studiofx") && !moveStudioFxBeforeCard(stack, added, undo, redo))) {
                undo();
                m_status->setText(QStringLiteral("Применение отменено: не удалось подготовить стек эффектов."));
                return;
            }
        }
        if (m_assetId == QLatin1String("card3d") && !stackUsesStillImage(stack)) {
            const int source = names.indexOf(QStringLiteral("7"));
            if (source >= 0) next[source] = QStringLiteral("1");
        }
        appendParameterChange(effects.front(), names, next, undo, redo);
        if (m_assetId == QLatin1String("studio_color"))
            appendParameterChange(effects.front(), {QStringLiteral("studio_input_validated"), QStringLiteral("studio_color_algorithm")},
                                  {QStringLiteral("1"), QStringLiteral("sdr-primary-v1")}, undo, redo);
        if (m_assetId == QLatin1String("card3d")) {
            appendParameterChange(effects.front(), {QStringLiteral("17")}, {cardExitEnd(effects.front())}, undo, redo);
        }
        ++changed;
    }
    if (changed == 0) {
        m_status->setText(QStringLiteral("Ни один клип не изменён: у выбранных клипов несколько экземпляров эффекта."));
        return;
    }
    pCore->pushUndo(undo, redo, batch ? QStringLiteral("Применить шаблон студии к клипам") : QStringLiteral("Применить шаблон студии"));
    refreshSelection();
    m_status->setText(batch
        ? QStringLiteral("Шаблон применён к %1 клипам. Исключено: заблокировано: %2, только аудио: %3, неоднозначно: %4.")
              .arg(changed).arg(excludedLocked).arg(excludedAudio).arg(excludedAmbiguous)
        : QStringLiteral("Шаблон применён одним действием."));
}

void StudioPanel::chooseTransitionSound(const QString &source)
{
    if (!m_effect) {
        m_transitionSoundSource = source;
        refreshValues();
        return;
    }
    if (source == m_effect->getParam(QStringLiteral("studio:sfx_source"))) return;
    auto model = activeStudioModel();
    if (!model || !currentTarget()) {
        m_status->setText(QStringLiteral("Выберите переход на таймлайне ещё раз."));
        refreshValues();
        return;
    }
    if (!applyTransition(model->getMixDuration(m_transitionSecond),
                         {{QStringLiteral("studio:sfx_source"), source}})) refreshValues();
}

bool StudioPanel::applyTransition(int frames, QVector<QPair<QString, QVariant>> parameters, bool remove)
{
    auto model = activeStudioModel();
    if (m_transitionPending) { m_status->setText(QStringLiteral("Дождитесь подключения звука перехода.")); return false; }
    if (!model || m_transitionFirst < 0 || m_transitionSecond < 0) return false;
    const bool existing = bool(m_effect);
    const int first = m_transitionFirst, second = m_transitionSecond;
    const int firstPosition = model->getItemPosition(first), secondPosition = model->getItemPosition(second);
    const auto document = pCore->currentDoc();
    const auto oldId = existing ? m_effect->getParam(QStringLiteral("studio:sfx_id")) : QString();
    const int oldClip = transitionSoundClip(model, oldId);
    if (oldClip == -2) { m_status->setText(QStringLiteral("Найдено несколько звуков одного перехода. Исправьте дубликаты вручную.")); return false; }
    auto parameter = [&](const QString &name, const QString &fallback) {
        for (const auto &entry : parameters) if (entry.first == name) return entry.second.toString();
        const QString current = existing ? m_effect->getParam(name) : QString();
        return current.isEmpty() ? fallback : current;
    };
    const bool enabled = !remove && parameter(QStringLiteral("studio:sfx_enabled"), QStringLiteral("0")).toDouble() > .5;
    const double level = std::clamp(parameter(QStringLiteral("studio:sfx_level"), QStringLiteral("0.5")).toDouble(), 0.0, 1.0);
    const QString source = parameter(QStringLiteral("studio:sfx_source"), QString());
    const QString identity = enabled ? (oldId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : oldId) : QString();
    if (enabled && !std::any_of(parameters.cbegin(), parameters.cend(), [](const auto &entry) {
            return entry.first == QLatin1String("studio:sfx_level");
        })) parameters << qMakePair(QStringLiteral("studio:sfx_level"), QVariant(level));
    if (!remove) {
        parameters.erase(std::remove_if(parameters.begin(), parameters.end(), [](const auto &entry) {
            return entry.first == QLatin1String("studio:sfx_source") || entry.first == QLatin1String("studio:sfx_id");
        }), parameters.end());
        parameters << qMakePair(QStringLiteral("studio:sfx_source"), QVariant(source));
        parameters << qMakePair(QStringLiteral("studio:sfx_id"), QVariant(identity));
    }
    QString soundPath;
    QString selectedBinId;
    const bool customFile = source.startsWith(QLatin1String("file:"));
    if (enabled) {
        if (source.startsWith(QLatin1String("bin:"))) {
            selectedBinId = source.mid(4);
            const auto clip = pCore->projectItemModel()->getClipByBinID(selectedBinId);
            if (!clip || !QFileInfo(clip->url()).isFile()) {
                m_status->setText(QStringLiteral("Свой звук больше не найден в проекте. Выберите файл заново."));
                return false;
            }
        } else if (customFile) {
            const QUrl url(source);
            soundPath = url.isLocalFile() ? url.toLocalFile() : QString();
            if (soundPath.isEmpty() || !QFileInfo(soundPath).isAbsolute()
                || !QFileInfo(soundPath).isFile() || !QFileInfo(soundPath).isReadable()) {
                m_status->setText(QStringLiteral("Свой звуковой файл недоступен. Выберите его заново."));
                return false;
            }
        } else {
            QString name = source;
            if (name.isEmpty()) {
                const double code = parameter(QStringLiteral("1"), QStringLiteral("0")).toDouble();
                int style = 0;
                double nearest = 2;
                for (const auto &entry : m_controls[QStringLiteral("style")].spec.value(QStringLiteral("options")).toArray()) {
                    const auto option = entry.toObject();
                    const double difference = std::abs(code - option.value(QStringLiteral("code")).toDouble());
                    if (difference < nearest) { nearest = difference; style = option.value(QStringLiteral("value")).toInt(); }
                }
                const int direction = qRound(parameter(QStringLiteral("3"), QStringLiteral("0")).toDouble() * 3);
                name = QStringLiteral("transition_%1%2.wav").arg(style, 2, 10, QLatin1Char('0'))
                    .arg(style == 2 && direction == 0 ? QStringLiteral("_right") : QString());
            } else {
                bool known = false;
                for (const auto &option : m_controls[QStringLiteral("sfx_source")].spec.value(QStringLiteral("options")).toArray())
                    known |= option.toObject().value(QStringLiteral("value")).toString() == name;
                if (!known) { m_status->setText(QStringLiteral("Выбранный встроенный звук неизвестен.")); return false; }
            }
            soundPath = StudioResources::dataFile(QStringLiteral("kdenlive/studio/sfx/v1/") + name);
            if (soundPath.isEmpty()) {
                m_status->setText(QStringLiteral("Встроенный звук перехода не найден в этой сборке."));
                return false;
            }
        }
    }
    auto undo = std::make_shared<Fun>([] { return true; });
    auto redo = std::make_shared<Fun>([] { return true; });
    const QPointer<StudioPanel> owner(this);
    const std::weak_ptr<TimelineItemModel> weakModel = model;
    const auto apply = [owner, weakModel, document, first, second, firstPosition, secondPosition, existing, remove,
                        frames, parameters, enabled, level, identity, oldClip, customFile, undo, redo](const QString &binId) -> bool {
        const auto model = weakModel.lock();
        if (!owner || !pCore || !model || pCore->currentDoc() != document) { (*undo)(); return false; }
        auto current = activeStudioModel();
        const auto selection = model->studioTransitionSelection();
        if (current != model || selection.first != first || selection.second != second
            || model->getItemPosition(first) != firstPosition || model->getItemPosition(second) != secondPosition
            || (existing ? selection.status != TimelineModel::StudioTransitionStatus::ExistingStudio
                         : selection.status != TimelineModel::StudioTransitionStatus::Ready)) {
            (*undo)();
            owner->m_status->setText(QStringLiteral("Стык изменился во время загрузки звука. Действие отменено."));
            return false;
        }
        const auto clip = enabled ? pCore->projectItemModel()->getClipByBinID(binId) : nullptr;
        if (enabled && (!clip || !clip->hasAudio() || clip->frameDuration() < 2)) {
            (*undo)();
            owner->m_status->setText(QStringLiteral("Выбранный файл не содержит подходящего звука."));
            return false;
        }
        auto appliedParameters = parameters;
        if (enabled && customFile) for (auto &entry : appliedParameters)
            if (entry.first == QLatin1String("studio:sfx_source")) entry.second = QStringLiteral("bin:%1").arg(binId);
        // Ctrl+A can put the managed sound in the A/B selection group. Deleting
        // that group would also delete the sources and clear the mix target.
        // Rebuild only the validated A/B selection, preserving native AV groups.
        if (oldClip >= 0 && !model->requestSetSelection({first, second})) {
            (*undo)(); owner->m_status->setText(QStringLiteral("Не удалось выбрать стык для замены звука.")); return false;
        }
        // Replace the managed sound within the same native undo transaction.
        if (oldClip >= 0 && !model->requestItemDeletion(oldClip, *undo, *redo, true)) {
            (*undo)(); owner->m_status->setText(QStringLiteral("Не удалось заменить прежний звук перехода.")); return false;
        }
        const bool changed = remove ? model->removeStudioTransition(undo.get(), redo.get())
            : existing ? model->updateStudioTransition(frames, appliedParameters, undo.get(), redo.get())
                       : model->mixClip(first, QStringLiteral("studio_transition"), 1, frames, appliedParameters, undo.get(), redo.get());
        if (!changed) { (*undo)(); owner->m_status->setText(QStringLiteral("Не удалось изменить переход. Изменения отменены.")); return false; }
        if (enabled) {
            QString error;
            const int track = StudioManagedAudio::ensureTrack(model, *undo, *redo, &error,
                QStringLiteral("kdenlive:studio_transition_sfx_track"), QStringLiteral("FactMontage: Звуки переходов"));
            if (track < 0) {
                (*undo)(); owner->m_status->setText(error.isEmpty() ? QStringLiteral("Звук перехода не загрузился.") : error); return false;
            }
            const int mixFrames = model->getMixDuration(second);
            const int soundFrames = std::min(mixFrames, int(clip->frameDuration()));
            const int position = model->getItemPosition(second) + (mixFrames - soundFrames) / 2;
            const int sourceStart = (int(clip->frameDuration()) - soundFrames) / 2;
            int item = -1;
            const QString source = QStringLiteral("A%1/%2/%3").arg(binId).arg(sourceStart).arg(sourceStart + soundFrames - 1);
            if (!model->requestClipInsertion(source, track, position, item, false, true, false, *undo, *redo)) {
                (*undo)(); owner->m_status->setText(QStringLiteral("Не удалось поместить звук на дорожку переходов.")); return false;
            }
            const auto producer = model->getClipProducer(item);
            if (!producer) { (*undo)(); owner->m_status->setText(QStringLiteral("Звуковой клип недоступен.")); return false; }
            producer->set("studio:sfx:id", identity.toUtf8().constData());
            const Fun previousRedo = *redo;
            *redo = [weakModel = std::weak_ptr<TimelineItemModel>(model), item, identity, previousRedo] {
                const auto model = weakModel.lock();
                if (!model || !previousRedo()) return false;
                const auto restored = model->getClipProducer(item);
                if (!restored) return false;
                restored->set("studio:sfx:id", identity.toUtf8().constData());
                return true;
            };
            const auto stack = model->getClipEffectStack(item);
            const auto before = effectsById(stack, QStringLiteral("volume"));
            if (!stack || !stack->appendEffectWithUndo(QStringLiteral("volume"), *undo, *redo).first) {
                (*undo)(); owner->m_status->setText(QStringLiteral("Не удалось установить громкость звука.")); return false;
            }
            bool tagged = false;
            for (const auto &effect : effectsById(stack, QStringLiteral("volume"))) if (!before.contains(effect)) {
                const double db = level <= 0 ? -100 : std::clamp(20 * std::log10(level / .5), -100.0, 6.1);
                appendParameterChange(effect, {QStringLiteral("level"), QStringLiteral("studio:sfx:id")},
                                      {QString::number(db, 'g', 8), identity}, *undo, *redo);
                tagged = true;
                break;
            }
            if (!tagged) { (*undo)(); owner->m_status->setText(QStringLiteral("Не удалось связать звук с переходом.")); return false; }
        }
        if (model->isClip(first) && model->isClip(second)) model->requestSetSelection({first, second});
        pCore->pushUndo(*undo, *redo, remove ? QStringLiteral("Удалить переход") : QStringLiteral("Изменить переход"));
        owner->refreshSelection();
        return true;
    };
    if (!enabled) return apply({});
    if (!selectedBinId.isEmpty()) return apply(selectedBinId);
    const QStringList existingBin = pCore->projectItemModel()->getClipByUrl(QFileInfo(soundPath));
    if (!existingBin.isEmpty()) return apply(existingBin.front());
    m_status->setText(QStringLiteral("Подключаю звук перехода…"));
    m_transitionPending = true;
    auto result = std::make_shared<int>(-1);
    const QString binId = ClipCreator::createClipFromFile(soundPath, pCore->projectItemModel()->getRootFolder()->clipId(),
                                                          pCore->projectItemModel(), *undo, *redo,
                                                          [apply, result, owner](const QString &id) {
                                                              if (*result != -1) return;
                                                              if (owner) owner->m_transitionPending = false;
                                                              *result = apply(id) ? 1 : 0;
                                                          }, false);
    if (binId == QLatin1String("-1")) {
        m_transitionPending = false;
        (*undo)(); m_status->setText(QStringLiteral("Не удалось добавить звук в проект.")); return false;
    }
    QTimer::singleShot(30000, this, [this, result, undo] {
        if (*result != -1) return;
        *result = 0;
        m_transitionPending = false;
        (*undo)();
        m_status->setText(QStringLiteral("Загрузка звука не завершилась за 30 секунд. Действие отменено."));
    });
    return *result < 0 || *result == 1;
}

QVector<QPair<QString, QVariant>> StudioPanel::transitionParameters(int frames) const
{
    const auto value = [this](const QString &key) {
        const auto spec = m_controls[key].spec;
        return storedValue(spec, spec.value(QStringLiteral("value")).toDouble());
    };
    return {{QStringLiteral("0"), QStringLiteral("0=0;%1=1").arg(std::max(0, frames - 1))},
            {QStringLiteral("1"), value(QStringLiteral("style"))},
            {QStringLiteral("2"), value(QStringLiteral("strength"))},
            {QStringLiteral("3"), value(QStringLiteral("direction"))},
            {QStringLiteral("4"), value(QStringLiteral("softness"))},
            {QStringLiteral("5"), QStringLiteral("0.5 0.5")},
            {QStringLiteral("6"), QStringLiteral("0")},
            {QStringLiteral("studio:audio_dip"), value(QStringLiteral("audio_dip"))},
            {QStringLiteral("studio:sfx_enabled"), value(QStringLiteral("sfx_enabled"))},
            {QStringLiteral("studio:sfx_level"), value(QStringLiteral("sfx_level"))},
            {QStringLiteral("studio:sfx_source"), m_transitionSoundSource},
            {QStringLiteral("studio:transition"), 1}};
}

void StudioPanel::refreshTransitionSelection()
{
    disconnect(m_parameterConnection);
    auto model = activeStudioModel();
    m_effect.reset();
    m_transitionFirst = -1;
    m_transitionSecond = -1;
    m_removeTransition->setVisible(false);
    m_add->setText(QStringLiteral("Добавить переход"));
    m_add->setVisible(false);
    m_add->setEnabled(false);
    for (auto it = m_controls.begin(); it != m_controls.end(); ++it) it.value().row->setEnabled(false);
    if (m_controls.contains(QStringLiteral("style"))) m_controls[QStringLiteral("style")].row->setEnabled(true);
    if (!model) {
        m_transitionSoundModel = nullptr;
        m_transitionSoundFirst = m_transitionSoundSecond = -1;
        m_transitionSoundSource.clear();
        m_target->setText(QStringLiteral("Нет выбранного стыка"));
        m_status->setText(QStringLiteral("Откройте последовательность и выберите два соседних видеоклипа."));
        refreshValues();
        return;
    }
    const int requested = qMax(1, qRound(m_controls[QStringLiteral("duration")].spec.value(QStringLiteral("value")).toDouble(.65)
                                         * pCore->getCurrentFps()));
    const auto selection = model->studioTransitionSelection(requested);
    const bool sameTarget = model.get() == m_transitionSoundModel && selection.first == m_transitionSoundFirst
        && selection.second == m_transitionSoundSecond;
    if (!sameTarget) m_transitionSoundSource.clear();
    m_transitionSoundModel = model.get();
    m_transitionSoundFirst = selection.first;
    m_transitionSoundSecond = selection.second;
    using Status = TimelineModel::StudioTransitionStatus;
    if (selection.first > -1 && selection.second > -1) {
        m_transitionFirst = selection.first;
        m_transitionSecond = selection.second;
        m_target->setText(QStringLiteral("%1 · %2 → %3")
                              .arg(model->getTrackTagById(selection.track), model->getClipName(selection.first), model->getClipName(selection.second)));
    } else m_target->setText(QStringLiteral("Нет выбранного стыка"));
    if (!supported()) {
        m_status->setText(KdenliveSettings::tenbitpipeline()
            ? QStringLiteral("Включён 10-битный режим. Переходы студии работают только в SDR / RGBA 8 бит.")
            : QStringLiteral("Переходы студии доступны только в SDR-проекте Rec.709/601, 8 бит."));
        refreshValues();
        return;
    }
    switch (selection.status) {
    case Status::NoSelection:
        m_status->setText(QStringLiteral("Выберите два соседних видеоклипа на одной дорожке.")); break;
    case Status::OneClip:
        m_status->setText(QStringLiteral("Выбран один клип. Добавьте соседний клип к выделению.")); break;
    case Status::TooManyClips:
        m_status->setText(QStringLiteral("Выбрано больше двух видеоклипов. Оставьте ровно два соседних.")); break;
    case Status::DifferentTracks:
        m_status->setText(QStringLiteral("Клипы находятся на разных дорожках. Выберите стык одной дорожки.")); break;
    case Status::Gap:
        m_status->setText(QStringLiteral("Между клипами есть промежуток. Сведите их в один стык.")); break;
    case Status::Locked:
        m_status->setText(QStringLiteral("Дорожка заблокирована. Разблокируйте её перед изменением.")); break;
    case Status::SameSourceFrames:
        m_status->setText(QStringLiteral("Это непрерывный разрез одного исходника: оба входа перехода покажут одинаковые кадры. "
                                         "Выберите разные фрагменты; монтаж автоматически не сокращается.")); break;
    case Status::InsufficientFrames:
        // Keep duration editable, so the user can reduce an overlong request.
        m_controls[QStringLiteral("duration")].row->setEnabled(true);
        m_status->setText(QStringLiteral("Для перехода %1 с нужно %2 кадров суммарно. "
                                         "Доступно: после первого: %3, до второго: %4. Максимум: %5 с.")
                              .arg(m_controls[QStringLiteral("duration")].spec.value(QStringLiteral("value")).toDouble(), 0, 'f', 2)
                              .arg(requested).arg(selection.firstAvailable).arg(selection.secondAvailable)
                              .arg(selection.maxDuration / pCore->getCurrentFps(), 0, 'f', 2)); break;
    case Status::LinkedAudioConflict:
        m_status->setText(QStringLiteral("Связанный звук нельзя безопасно смешать: проверьте его стык, блокировку и существующий микс.")); break;
    case Status::ExistingOther:
        m_add->show();
        m_status->setText(QStringLiteral("На этом стыке уже установлен другой переход. Автоматическая замена отключена.")); break;
    case Status::ExistingStudio: {
        m_effect = model->studioTransitionModel(selection.second);
        m_transitionSoundSource = m_effect ? m_effect->getParam(QStringLiteral("studio:sfx_source")) : QString();
        m_controls[QStringLiteral("duration")].spec.insert(QStringLiteral("value"), model->getMixDuration(selection.second) / pCore->getCurrentFps());
        for (auto it = m_controls.begin(); it != m_controls.end(); ++it) it.value().row->setEnabled(true);
        m_removeTransition->setVisible(true);
        m_savePreset->setEnabled(true);
        m_replacePreset->setEnabled(m_presets->currentIndex() >= 0);
        m_applyPreset->setEnabled(m_presets->currentIndex() >= 0);
        m_status->setText(selection.sameFrames
            ? QStringLiteral("Оба входа показывают те же кадры исходника. Автообрезка отключена: удалите переход и выберите разные фрагменты. "
                             "Настройки можно менять без сдвига монтажа.")
            : QStringLiteral("Переход установлен. Изменения сразу видны в мониторе; отмена: Ctrl+Z."));
        if (m_effect) m_parameterConnection = connect(m_effect.get(), &QAbstractItemModel::dataChanged, this, [this] { refreshValues(); });
        refreshValues();
        return;
    }
    case Status::Ready:
        for (auto it = m_controls.begin(); it != m_controls.end(); ++it) it.value().row->setEnabled(true);
        m_add->setVisible(true);
        m_add->setEnabled(TransitionsRepository::get()->exists(QStringLiteral("studio_transition")));
        m_savePreset->setEnabled(true);
        m_applyPreset->setEnabled(m_presets->currentIndex() >= 0);
        m_status->setText(m_add->isEnabled() ? QStringLiteral("Стык готов. Выберите вариант и добавьте переход.")
                                             : QStringLiteral("Переход студии не найден в этой сборке."));
        refreshValues();
        return;
    }
    m_savePreset->setEnabled(false);
    m_replacePreset->setEnabled(false);
    m_applyPreset->setEnabled(false);
    refreshValues();
}

void StudioPanel::removeTransition()
{
    auto model = activeStudioModel();
    if (!model || !applyTransition(0, {}, true)) {
        m_status->setText(QStringLiteral("Не удалось удалить переход студии. Другие переходы не изменены."));
        return;
    }
    m_transitionSoundSource.clear();
    refreshSelection();
}

void StudioPanel::fixCameraOrder()
{
    if (!m_stack || m_assetId != QLatin1String("studio_camera") || m_sequence) return;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    if (!moveCameraBeforeCard(m_stack, undo, redo)) {
        m_status->setText(QStringLiteral("Не удалось изменить порядок эффектов."));
        return;
    }
    pCore->pushUndo(undo, redo, QStringLiteral("Применять камеру к содержимому"));
    refreshSelection();
}

QString StudioPanel::effectRecipeName(int slot) const
{
    return slot >= 0 && slot < m_effectRecipes.size()
        ? m_effectRecipes[slot].toObject().value(QStringLiteral("name")).toString()
        : QStringLiteral("Эффект");
}

void StudioPanel::selectEffectRecipe(int slot)
{
    if (m_assetId != QLatin1String("studiofx") || m_effectRecipes.isEmpty()) return;
    m_pendingRecipe = std::clamp(slot, 0, int(m_effectRecipes.size()) - 1);
    if (m_controls.contains(QStringLiteral("recipe"))) {
        for (auto button : m_controls[QStringLiteral("recipe")].choices) {
            QSignalBlocker block(button);
            button->setChecked(button->property("studioRecipeId").toInt() == m_pendingRecipe);
        }
    }
    const auto targets = batchVideoStacks(nullptr, nullptr);
    m_add->setText(QStringLiteral("Применить «%1» к %2 клипам").arg(effectRecipeName(m_pendingRecipe)).arg(targets.size()));
    m_add->setEnabled(!targets.isEmpty() && supported() && EffectsRepository::get()->exists(QStringLiteral("studiofx")));
    if (m_effectFavorite) {
        const auto id = m_effectRecipes[m_pendingRecipe].toObject().value(QStringLiteral("id")).toString();
        const bool favorite = QSettings().value(QStringLiteral("StudioFX/favorites")).toStringList().contains(id);
        QSignalBlocker block(m_effectFavorite);
        m_effectFavorite->setChecked(favorite);
        m_effectFavorite->setText(favorite ? QStringLiteral("★") : QStringLiteral("☆"));
        m_effectFavorite->setToolTip(favorite ? QStringLiteral("Убрать из избранного") : QStringLiteral("Добавить в избранное"));
    }
}

void StudioPanel::filterEffectRecipes()
{
    if (m_assetId != QLatin1String("studiofx") || !m_controls.contains(QStringLiteral("recipe"))) return;
    const QString needle = m_effectSearch ? m_effectSearch->text().simplified() : QString();
    const QString category = m_effectCategory ? m_effectCategory->currentData().toString() : QString();
    const int filter = m_effectFilter ? m_effectFilter->currentIndex() : 0;
    const auto favorites = QSettings().value(QStringLiteral("StudioFX/favorites")).toStringList();
    const auto recent = QSettings().value(QStringLiteral("StudioFX/recent")).toStringList();
    for (auto button : m_controls[QStringLiteral("recipe")].choices) {
        const int slot = button->property("studioRecipeId").toInt();
        const auto recipe = slot >= 0 && slot < m_effectRecipes.size() ? m_effectRecipes[slot].toObject() : QJsonObject();
        const QString id = recipe.value(QStringLiteral("id")).toString();
        const QString text = recipe.value(QStringLiteral("name")).toString() + QLatin1Char(' ') + recipe.value(QStringLiteral("category")).toString();
        const bool visible = (category.isEmpty() || recipe.value(QStringLiteral("category_id")).toString() == category)
            && (needle.isEmpty() || text.contains(needle, Qt::CaseInsensitive))
            && (filter == 0 || (filter == 1 ? favorites.contains(id) : recent.contains(id)));
        if (button->parentWidget()) button->parentWidget()->setVisible(visible);
    }
    selectEffectRecipe(m_pendingRecipe);
}

void StudioPanel::applyEffectRecipe(bool duplicate)
{
    if (m_assetId != QLatin1String("studiofx") || m_pendingRecipe < 0 || m_pendingRecipe >= m_effectRecipes.size()) return;
    if (!supported() || !EffectsRepository::get()->exists(QStringLiteral("studiofx"))) {
        m_status->setText(QStringLiteral("Эффект FactMontage недоступен в этой сборке или цветовом режиме."));
        return;
    }
    int locked = 0, audio = 0;
    const auto targets = batchVideoStacks(&locked, &audio);
    if (targets.isEmpty()) {
        m_status->setText(QStringLiteral("Нет подходящих выбранных клипов."));
        return;
    }
    const auto recipe = m_effectRecipes[m_pendingRecipe].toObject();
    const auto values = recipe.value(QStringLiteral("values")).toArray();
    if (values.size() != 11) { m_status->setText(QStringLiteral("Каталог эффектов повреждён.")); return; }
    QStringList names, next;
    for (int i = 0; i < values.size(); ++i) { names << QString::number(i); next << QString::number(values[i].toDouble(), 'g', 17); }
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    int changed = 0, existing = 0;
    for (const auto &stack : targets) {
        const auto before = effectsById(stack, QStringLiteral("studiofx"));
        bool same = false;
        for (const auto &effect : before)
            same |= qRound(effect->getParam(QStringLiteral("0")).toDouble() * 127.0) == m_pendingRecipe;
        if (same && !duplicate) { ++existing; continue; }
        if (!stack->appendEffectWithUndo(QStringLiteral("studiofx"), undo, redo).first) {
            undo();
            m_status->setText(QStringLiteral("Применение отменено: один из стеков не изменён."));
            return;
        }
        const auto after = effectsById(stack, QStringLiteral("studiofx"));
        std::shared_ptr<EffectItemModel> added;
        for (const auto &effect : after) if (!before.contains(effect)) { added = effect; break; }
        if (!added || !moveStudioFxBeforeCard(stack, added, undo, redo)) {
            undo();
            m_status->setText(QStringLiteral("Применение отменено: не удалось безопасно добавить эффект FactMontage."));
            return;
        }
        appendParameterChange(added, names, next, undo, redo);
        ++changed;
    }
    if (changed == 0) {
        m_status->setText(QStringLiteral("Этот эффект уже есть на всех подходящих клипах; ручные настройки не изменены."));
        return;
    }
    pCore->pushUndo(undo, redo, duplicate ? QStringLiteral("Добавить ещё один эффект") : QStringLiteral("Применить эффект к клипам"));
    auto recent = QSettings().value(QStringLiteral("StudioFX/recent")).toStringList();
    const QString id = recipe.value(QStringLiteral("id")).toString();
    recent.removeAll(id); recent.prepend(id); while (recent.size() > 20) recent.removeLast();
    QSettings().setValue(QStringLiteral("StudioFX/recent"), recent);
    refreshSelection();
    m_status->setText(QStringLiteral("Эффект добавлен к %1 клипам. Уже был: %2; пропущено заблокированных: %3; аудио: %4.")
                          .arg(changed).arg(existing).arg(locked).arg(audio));
}

void StudioPanel::startPreview(QToolButton *button)
{
    stopPreview();
    if (!isVisible()) return;
    m_movie->setFileName(button->property("studioPreview").toString() + QStringLiteral(".gif"));
    if (!m_movie->isValid()) return; // A missing preview never disables the renderer.
    m_preview = button;
    m_movie->start();
}
void StudioPanel::stopPreview()
{
    m_movie->stop();
    if (m_preview) m_preview->setIcon(QIcon(m_preview->property("studioPreview").toString() + QStringLiteral(".png")));
    m_preview.clear();
}
void StudioPanel::startFraming()
{
    m_framingClipMonitor = !m_sequence && m_selectedClipId >= 0;
    auto monitor = pCore ? pCore->getMonitor(m_framingClipMonitor ? Kdenlive::ClipMonitor : Kdenlive::ProjectMonitor) : nullptr;
    if (!monitor || !m_effect || !currentTarget() || m_assetId != QLatin1String("studio_camera")) {
        m_status->setText(QStringLiteral("Сначала выберите один эффект камеры."));
        return;
    }
    stopFraming();
    m_cardPositioning = false;
    m_effectCentering = false;
    m_framingClipMonitor = !m_sequence && m_selectedClipId >= 0;
    if (m_framingClipMonitor) {
        auto timeline = pCore && pCore->window() ? pCore->window()->getCurrentTimeline() : nullptr;
        auto model = timeline ? timeline->model() : nullptr;
        auto clip = model ? pCore->bin()->getBinClip(model->getClipBinId(m_selectedClipId)) : nullptr;
        if (!clip) {
            m_status->setText(QStringLiteral("Не удалось открыть исходник выбранного клипа в мониторе."));
            return;
        }
        const int in = model->getClipIn(m_selectedClipId);
        monitor->slotOpenClip(clip, in, in + model->getClipPlaytime(m_selectedClipId) - 1);
        monitor->requestSeek(in);
        pCore->monitorManager()->activateMonitor(Kdenlive::ClipMonitor);
    }
    monitor->stop();
    m_monitorEffect = m_effect;
    m_framingActive = true;
    m_framing->hide();
    m_framingDone->show();
    m_monitorConnection = connect(monitor, &Monitor::effectChanged, this, &StudioPanel::monitorRectChanged);
    m_monitorKeyConnection = connect(monitor, &Monitor::passKeyPress, this, [this](QKeyEvent *event) {
        if (event && event->key() == Qt::Key_Escape) stopFraming();
    });
    monitor->slotShowEffectScene(SceneType::MonitorSceneGeometry, true);
    monitor->setEffectSceneProperty(QStringLiteral("lockratio"), true);
    QTimer::singleShot(0, this, &StudioPanel::updateFramingRect);
    m_status->setText(QStringLiteral("Кадрирование открыто в мониторе. Готово или Escape: выход."));
}

void StudioPanel::stopFraming()
{
    if (!m_framingActive) return;
    finishMonitorGesture();
    disconnect(m_monitorConnection);
    disconnect(m_monitorKeyConnection);
    if (auto monitor = pCore ? pCore->getMonitor(m_framingClipMonitor ? Kdenlive::ClipMonitor : Kdenlive::ProjectMonitor) : nullptr) {
        monitor->slotShowEffectScene(SceneType::MonitorSceneNone, true);
        monitor->refreshMonitorIfActive(true);
    }
    if (m_framingClipMonitor && pCore && pCore->monitorManager()) pCore->monitorManager()->activateMonitor(Kdenlive::ProjectMonitor);
    m_framingActive = false;
    m_framingClipMonitor = false;
    m_cardPositioning = false;
    m_effectCentering = false;
    m_monitorEffect.reset();
    if (m_framing) m_framing->show();
    if (m_framingDone) m_framingDone->hide();
    if (m_cardPosition) m_cardPosition->show();
    if (m_cardPositionDone) m_cardPositionDone->hide();
    if (m_effectCenter) m_effectCenter->show();
    if (m_effectCenterDone) m_effectCenterDone->hide();
}

void StudioPanel::updateFramingRect()
{
    if (!m_framingActive || !m_monitorEffect || m_monitorEffect != m_effect) return;
    auto monitor = pCore->getMonitor(m_framingClipMonitor ? Kdenlive::ClipMonitor : Kdenlive::ProjectMonitor);
    if (!monitor) return;
    const QSize frame = monitor->profileSize();
    if (frame.isEmpty()) return;
    const QString xKey = m_editEnd ? QStringLiteral("end_x") : QStringLiteral("start_x");
    const QString yKey = m_editEnd ? QStringLiteral("end_y") : QStringLiteral("start_y");
    const double x = displayedValue(m_controls[xKey].spec) / 100.0;
    const double y = displayedValue(m_controls[yKey].spec) / 100.0;
    const double zoom = std::max(100.0, displayedValue(m_controls[QStringLiteral("zoom")].spec));
    const double width = frame.width() * 100.0 / zoom;
    const double height = frame.height() * 100.0 / zoom;
    monitor->setUpEffectGeometry(QRect(qRound(x * frame.width() - width / 2.0), qRound(y * frame.height() - height / 2.0),
                                       qRound(width), qRound(height)));
}

void StudioPanel::monitorRectChanged(const QRectF &rect)
{
    if (m_effectCentering && m_framingActive && m_monitorEffect && rect.width() > 0 && rect.height() > 0) {
        auto monitor = pCore->getMonitor(Kdenlive::ClipMonitor);
        const QSize frame = monitor ? monitor->profileSize() : QSize();
        if (frame.isEmpty()) return;
        const auto xSpec = m_controls[QStringLiteral("center_x")].spec;
        const auto ySpec = m_controls[QStringLiteral("center_y")].spec;
        const QStringList names{parameterName(xSpec), parameterName(ySpec)};
        if (m_monitorNames.isEmpty()) {
            m_monitorNames = names;
            for (const auto &name : names) m_monitorBefore << m_monitorEffect->getParam(name);
        }
        const double x = std::clamp(rect.center().x() / frame.width() * 100.0, 0.0, 100.0);
        const double y = std::clamp(rect.center().y() / frame.height() * 100.0, 0.0, 100.0);
        m_monitorAfter = {storedValue(xSpec, x), storedValue(ySpec, y)};
        m_monitorCommitTimer->start();
        for (int i = 0; i < names.size(); ++i) m_monitorEffect->setParameter(names[i], m_monitorAfter[i], true);
        return;
    }
    if (m_cardPositioning && m_framingActive && m_monitorEffect && rect.width() > 0 && rect.height() > 0) {
        auto monitor = pCore->getMonitor(Kdenlive::ProjectMonitor);
        const QSize frame = monitor ? monitor->profileSize() : QSize();
        if (frame.isEmpty()) return;
        const auto mode = m_controls[QStringLiteral("POSITION_MODE")].spec;
        const auto xSpec = m_controls[QStringLiteral("X")].spec;
        const auto ySpec = m_controls[QStringLiteral("Y")].spec;
        const QStringList names{parameterName(mode), parameterName(xSpec), parameterName(ySpec)};
        if (m_monitorNames.isEmpty()) {
            m_monitorNames = names;
            for (const auto &name : names) m_monitorBefore << m_monitorEffect->getParam(name);
        }
        const double x = std::clamp(rect.left() / std::max(1.0, frame.width() - rect.width()) * 100.0, 0.0, 100.0);
        const double y = std::clamp(rect.top() / std::max(1.0, frame.height() - rect.height()) * 100.0, 0.0, 100.0);
        m_monitorAfter = {storedValue(mode, 1), storedValue(xSpec, x), storedValue(ySpec, y)};
        m_monitorCommitTimer->start();
        for (int i = 0; i < names.size(); ++i) m_monitorEffect->setParameter(names[i], m_monitorAfter[i], true);
        return;
    }
    if (!m_framingActive || !m_monitorEffect || rect.width() <= 0 || rect.height() <= 0) return;
    auto monitor = pCore->getMonitor(m_framingClipMonitor ? Kdenlive::ClipMonitor : Kdenlive::ProjectMonitor);
    const QSize frame = monitor ? monitor->profileSize() : QSize();
    if (frame.isEmpty()) return;
    const QString xKey = m_editEnd ? QStringLiteral("end_x") : QStringLiteral("start_x");
    const QString yKey = m_editEnd ? QStringLiteral("end_y") : QStringLiteral("start_y");
    const QStringList names{parameterName(m_controls[xKey].spec), parameterName(m_controls[yKey].spec),
                            parameterName(m_controls[QStringLiteral("zoom")].spec)};
    if (m_monitorNames.isEmpty()) {
        m_monitorNames = names;
        for (const auto &name : names) m_monitorBefore << m_monitorEffect->getParam(name);
    } else if (m_monitorNames != names) {
        finishMonitorGesture();
        m_monitorNames = names;
        for (const auto &name : names) m_monitorBefore << m_monitorEffect->getParam(name);
    }
    const double x = std::clamp(rect.center().x() / frame.width() * 100.0, 0.0, 100.0);
    const double y = std::clamp(rect.center().y() / frame.height() * 100.0, 0.0, 100.0);
    const auto zoomSpec = m_controls[QStringLiteral("zoom")].spec;
    const double zoom = std::clamp(std::min(frame.width() / rect.width(), frame.height() / rect.height()) * 100.0,
                                   zoomSpec.value(QStringLiteral("lo")).toDouble(), zoomSpec.value(QStringLiteral("hi")).toDouble());
    m_monitorAfter = {storedValue(m_controls[xKey].spec, x), storedValue(m_controls[yKey].spec, y), storedValue(zoomSpec, zoom)};
    m_monitorCommitTimer->start();
    for (int i = 0; i < names.size(); ++i) m_monitorEffect->setParameter(names[i], m_monitorAfter[i], true);
}

void StudioPanel::startEffectCenter()
{
    if (!m_effect || !currentTarget() || m_assetId != QLatin1String("studiofx") || m_selectedClipId < 0) {
        m_status->setText(QStringLiteral("Сначала выберите один экземпляр эффекта."));
        return;
    }
    stopFraming();
    auto monitor = pCore ? pCore->getMonitor(Kdenlive::ClipMonitor) : nullptr;
    auto timeline = pCore && pCore->window() ? pCore->window()->getCurrentTimeline() : nullptr;
    auto model = timeline ? timeline->model() : nullptr;
    auto clip = model ? pCore->bin()->getBinClip(model->getClipBinId(m_selectedClipId)) : nullptr;
    if (!monitor || !clip) {
        m_status->setText(QStringLiteral("Не удалось открыть исходник в мониторе."));
        return;
    }
    const int in = model->getClipIn(m_selectedClipId);
    monitor->slotOpenClip(clip, in, in + model->getClipPlaytime(m_selectedClipId) - 1);
    monitor->requestSeek(in);
    pCore->monitorManager()->activateMonitor(Kdenlive::ClipMonitor);
    monitor->stop();
    m_monitorEffect = m_effect;
    m_framingActive = true;
    m_framingClipMonitor = true;
    m_effectCentering = true;
    m_effectCenter->hide();
    m_effectCenterDone->show();
    m_monitorConnection = connect(monitor, &Monitor::effectChanged, this, &StudioPanel::monitorRectChanged);
    m_monitorKeyConnection = connect(monitor, &Monitor::passKeyPress, this, [this](QKeyEvent *event) {
        if (event && event->key() == Qt::Key_Escape) stopFraming();
    });
    monitor->slotShowEffectScene(SceneType::MonitorSceneGeometry, true);
    monitor->setEffectSceneProperty(QStringLiteral("lockratio"), true);
    QTimer::singleShot(0, this, &StudioPanel::updateEffectCenterRect);
    m_status->setText(QStringLiteral("Перетащите метку центра. «Готово» или Escape: выход."));
}

void StudioPanel::updateEffectCenterRect()
{
    if (!m_effectCentering || !m_monitorEffect || m_monitorEffect != m_effect) return;
    auto monitor = pCore->getMonitor(Kdenlive::ClipMonitor);
    const QSize frame = monitor ? monitor->profileSize() : QSize();
    if (frame.isEmpty()) return;
    const double x = displayedValue(m_controls[QStringLiteral("center_x")].spec) / 100.0;
    const double y = displayedValue(m_controls[QStringLiteral("center_y")].spec) / 100.0;
    const int side = std::max(24, std::min(frame.width(), frame.height()) / 12);
    monitor->setUpEffectGeometry(QRect(qRound(x * frame.width() - side / 2.0), qRound(y * frame.height() - side / 2.0), side, side));
}

void StudioPanel::startCardPosition()
{
    if (!m_effect || !currentTarget() || m_assetId != QLatin1String("card3d")) {
        m_status->setText(QStringLiteral("Сначала выберите одну карточку."));
        return;
    }
    stopFraming();
    auto monitor = pCore ? pCore->getMonitor(Kdenlive::ProjectMonitor) : nullptr;
    if (!monitor) return;
    pCore->monitorManager()->activateMonitor(Kdenlive::ProjectMonitor);
    monitor->stop();
    m_monitorEffect = m_effect;
    m_framingActive = true;
    m_framingClipMonitor = false;
    m_cardPositioning = true;
    m_effectCentering = false;
    m_cardPosition->hide();
    m_cardPositionDone->show();
    m_monitorConnection = connect(monitor, &Monitor::effectChanged, this, &StudioPanel::monitorRectChanged);
    m_monitorKeyConnection = connect(monitor, &Monitor::passKeyPress, this, [this](QKeyEvent *event) {
        if (event && event->key() == Qt::Key_Escape) stopFraming();
    });
    monitor->slotShowEffectScene(SceneType::MonitorSceneGeometry, true);
    monitor->setEffectSceneProperty(QStringLiteral("lockratio"), true);
    QTimer::singleShot(0, this, &StudioPanel::updateCardPositionRect);
    m_status->setText(QStringLiteral("Перетащите рамку карточки в мониторе проекта. Готово или Escape: выход."));
}

void StudioPanel::updateCardPositionRect()
{
    if (!m_cardPositioning || !m_monitorEffect || m_monitorEffect != m_effect) return;
    auto monitor = pCore->getMonitor(Kdenlive::ProjectMonitor);
    const QSize frame = monitor ? monitor->profileSize() : QSize();
    if (frame.isEmpty()) return;
    double x = displayedValue(m_controls[QStringLiteral("X")].spec) / 100.0;
    double y = displayedValue(m_controls[QStringLiteral("Y")].spec) / 100.0;
    if (int(std::round(displayedValue(m_controls[QStringLiteral("POSITION_MODE")].spec))) == 0) {
        static const double positions[7][2] = {{.5, .5}, {0, .5}, {1, .5}, {0, 0}, {1, 0}, {0, 1}, {1, 1}};
        const int placement = std::clamp(int(std::round(displayedValue(m_controls[QStringLiteral("PLACEMENT")].spec))), 0, 6);
        x = positions[placement][0];
        y = positions[placement][1];
    }
    const double size = std::clamp(displayedValue(m_controls[QStringLiteral("SIZE")].spec) / 100.0, .15, 1.0);
    const int width = std::max(32, qRound(frame.width() * size));
    const int height = std::max(18, qRound(frame.height() * size));
    monitor->setUpEffectGeometry(QRect(qRound(x * std::max(0, frame.width() - width)),
                                       qRound(y * std::max(0, frame.height() - height)), width, height));
}

void StudioPanel::finishMonitorGesture()
{
    m_monitorCommitTimer->stop();
    auto effect = m_monitorEffect;
    if (effect && !m_monitorNames.isEmpty() && m_monitorBefore != m_monitorAfter)
        pCore->pushUndo(new StudioCommand(effect, m_monitorNames, m_monitorBefore, m_monitorAfter));
    m_monitorNames.clear();
    m_monitorBefore.clear();
    m_monitorAfter.clear();
}

void StudioPanel::centerFraming()
{
    if (!m_effect) return;
    finishMonitorGesture();
    const QString xKey = m_editEnd ? QStringLiteral("end_x") : QStringLiteral("start_x");
    const QString yKey = m_editEnd ? QStringLiteral("end_y") : QStringLiteral("start_y");
    const QStringList names{parameterName(m_controls[xKey].spec), parameterName(m_controls[yKey].spec)};
    const QStringList before{m_effect->getParam(names[0]), m_effect->getParam(names[1])};
    const QStringList after{storedValue(m_controls[xKey].spec, 50.0), storedValue(m_controls[yKey].spec, 50.0)};
    if (before != after) pCore->pushUndo(new StudioCommand(m_effect, names, before, after));
    updateFramingRect();
}

void StudioPanel::swapFraming()
{
    if (!m_effect) return;
    finishMonitorGesture();
    const QStringList names{parameterName(m_controls[QStringLiteral("start_x")].spec), parameterName(m_controls[QStringLiteral("start_y")].spec),
                            parameterName(m_controls[QStringLiteral("end_x")].spec), parameterName(m_controls[QStringLiteral("end_y")].spec)};
    const QStringList before{m_effect->getParam(names[0]), m_effect->getParam(names[1]),
                             m_effect->getParam(names[2]), m_effect->getParam(names[3])};
    const QStringList after{before[2], before[3], before[0], before[1]};
    if (before != after) pCore->pushUndo(new StudioCommand(m_effect, names, before, after));
    updateFramingRect();
}

void StudioPanel::setBackgroundMethod(int method)
{
    method = std::clamp(method, 0, 1);
    if (m_backgroundClipId >= 0) return;
    m_backgroundMethod = method;
    if (m_effect && m_assetId == QLatin1String("studio_background")) {
        if (backgroundNeedsAnalysis()) {
            m_pendingBackgroundValues = QJsonObject();
            for (const auto &name : m_backgroundParameters)
                m_pendingBackgroundValues.insert(name, m_effect->getParam(name).toDouble());
            m_pendingBackgroundValues.insert(QStringLiteral("method"), 1);
            m_pendingBackgroundValues.insert(QStringLiteral("output"), 1);
        } else {
            const QStringList names{QStringLiteral("method"), QStringLiteral("output")};
            const QStringList before{m_effect->getParam(names[0]), m_effect->getParam(names[1])};
            const QStringList after{QString::number(method), QString::number(method == 1 ? 1 : 0)};
            if (before != after) pCore->pushUndo(new StudioCommand(m_effect, names, before, after));
        }
    } else {
        m_pendingBackgroundValues.insert(QStringLiteral("method"), method);
        m_pendingBackgroundValues.insert(QStringLiteral("output"), method == 1 ? 1 : 0);
    }
    { QSignalBlocker blocker(m_backgroundTabs); m_backgroundTabs->setCurrentIndex(method); }
    m_add->setVisible(!m_effect || method == 1);
    m_add->setText(method == 0 ? QStringLiteral("Убрать фон")
                               : backgroundNeedsAnalysis() ? QStringLiteral("Проанализировать и применить")
                                                           : QStringLiteral("Проанализировать заново"));
    refreshValues();
    loadBackgroundFrame();
}

bool StudioPanel::backgroundNeedsAnalysis() const
{
    return m_assetId == QLatin1String("studio_background") && m_backgroundMethod == 1
        && (!m_effect || m_effect->getParam(QStringLiteral("mask_asset")).isEmpty());
}

void StudioPanel::loadBackgroundFrame()
{
    if (!m_backgroundFrame || m_page != 7) return;
    if (StudioJobs::busy(m_backgroundAnalyzer)) return;
    const auto request = ++m_frameRequest;
    m_backgroundImage = QImage();
    m_backgroundFrame->clear();
    m_backgroundFrame->setText(QStringLiteral("Выберите видеоклип или изображение на таймлайне"));
    auto timeline = pCore && pCore->window() ? pCore->window()->getCurrentTimeline() : nullptr;
    auto model = timeline ? timeline->model() : nullptr;
    auto clip = model && m_selectedClipId >= 0 ? pCore->bin()->getBinClip(model->getClipBinId(m_selectedClipId)) : nullptr;
    if (!clip || !clip->hasUrl()) return;
    const int selected = m_selectedClipId;
    m_backgroundFrame->setText(QStringLiteral("Подготовка стоп-кадра…"));
    StudioJobs::thumbnail(clip, model->getClipIn(selected), this, [this, request, selected](QImage image) {
        if (request != m_frameRequest || selected != m_selectedClipId || m_page != 7) return;
        m_backgroundImage = image;
        if (image.isNull()) m_backgroundFrame->setText(QStringLiteral("Не удалось показать стоп-кадр этого клипа"));
        else paintBackgroundFrame();
    });
    if (m_backgroundMethod == 0)
        m_status->setText(QStringLiteral("Щёлкните по однотонному фону на кадре и нажмите «Убрать фон»."));
    else if (m_effect && m_effect->getParam(QStringLiteral("mask_asset")).isEmpty())
        m_status->setText(QStringLiteral("Маска человека ещё не рассчитана. Нажмите «Проанализировать и применить»."));
}

void StudioPanel::paintBackgroundFrame()
{
    if (!m_backgroundFrame) return;
    m_backgroundFrame->setFixedWidth(std::max(1, m_scroll->viewport()->width()));
    if (m_backgroundImage.isNull()) return;
    const QRect contents = m_backgroundFrame->contentsRect();
    const int width = std::clamp(contents.width() - 2 * m_backgroundFrame->margin(), 1, 720);
    const int height = std::clamp(contents.height() - 2 * m_backgroundFrame->margin(), 1, 360);
    m_backgroundFrame->setPixmap(QPixmap::fromImage(m_backgroundImage.scaled(QSize(width, height), Qt::KeepAspectRatio,
                                                                                Qt::SmoothTransformation)));
    const int output = m_effect && !backgroundNeedsAnalysis() ? std::clamp(qRound(m_effect->getParam(QStringLiteral("output")).toDouble()), 0, 2)
                                : m_pendingBackgroundValues.value(QStringLiteral("output")).toInt(m_backgroundMethod == 1 ? 1 : 0);
    m_backgroundColor->setVisible(m_backgroundMethod == 0 || output == 2);
    const QColor color = m_backgroundMethod == 0 ? m_backgroundKey : m_backgroundFill;
    m_backgroundColor->setText(m_backgroundMethod == 0 ? QStringLiteral("Цвет фона")
                                                        : QStringLiteral("Цвет заливки"));
    m_backgroundColor->setToolTip(m_backgroundMethod == 0 ? QStringLiteral("Выберите цвет на стоп-кадре или нажмите здесь.") : QString());
    m_backgroundColor->setStyleSheet(QStringLiteral("text-align:left; border-left:24px solid %1;").arg(color.name()));
}

void StudioPanel::applyBackground()
{
    if (m_backgroundClipId >= 0) {
        cancelAnalyses();
        m_add->setEnabled(false);
        m_add->setText(QStringLiteral("Отмена…"));
        return;
    }
    if (m_backgroundMethod == 1) {
        runBackgroundAnalysis();
        return;
    }
    if (!currentTarget() || !supported() || !m_stack) return;
    QJsonObject values = m_pendingBackgroundValues;
    values.insert(QStringLiteral("method"), 0);
    values.insert(QStringLiteral("key_r"), m_backgroundKey.redF());
    values.insert(QStringLiteral("key_g"), m_backgroundKey.greenF());
    values.insert(QStringLiteral("key_b"), m_backgroundKey.blueF());
    if (!m_effect) {
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        if (!m_stack->appendEffectWithUndo(m_assetId, undo, redo).first) {
            m_status->setText(QStringLiteral("Не удалось добавить удаление фона к выбранному клипу."));
            return;
        }
        const auto effects = effectsById(m_stack, m_assetId);
        const auto effect = effects.isEmpty() ? std::shared_ptr<EffectItemModel>() : effects.back();
        if (!effect || !moveBackgroundBeforeStudio(m_stack, effect, undo, redo)) {
            undo();
            m_status->setText(QStringLiteral("Не удалось безопасно разместить удаление фона в стеке."));
            return;
        }
        QStringList names, after;
        for (const auto &name : m_backgroundParameters) {
            names << name;
            after << QString::number(values.value(name).toDouble(), 'g', 17);
        }
        appendParameterChange(effect, names, after, undo, redo);
        pCore->pushUndo(undo, redo, QStringLiteral("Убрать однотонный фон"));
        m_pendingBackgroundValues = values;
        refreshSelection();
        return;
    }
    QStringList names, before, after;
    for (const auto &name : m_backgroundParameters) {
        names << name;
        before << m_effect->getParam(name);
        after << QString::number(values.value(name).toDouble(), 'g', 17);
    }
    if (before != after) pCore->pushUndo(new StudioCommand(m_effect, names, before, after));
    refreshValues();
}

void StudioPanel::runBackgroundAnalysis()
{
    if (m_backgroundClipId >= 0) return;
    auto model = activeStudioModel();
    const int clipId = m_selectedClipId;
    auto clip = model && clipId >= 0 ? pCore->projectItemModel()->getClipByBinID(model->getClipBinId(clipId)) : nullptr;
    if (!model || !clip || !clip->hasUrl() || !QFileInfo::exists(clip->url()) || !m_stack || !supported()) {
        m_status->setText(QStringLiteral("Выберите один доступный видеоклип или изображение в SDR-проекте."));
        return;
    }
    if (std::abs(model->getClipSpeed(clipId) - 1.0) > 1e-6) {
        m_status->setText(QStringLiteral("Удаление фона человека пока требует обычную скорость 100% без Reverse."));
        return;
    }
    const QString analyzer = StudioResources::executable(QStringLiteral("studio-background-analyzer"));
    const QString melt = StudioResources::executable(QStringLiteral("melt"));
    const QString modelFile = StudioResources::dataFile(QStringLiteral("studio-background/models/pp_humansegv2_lite_portrait_static.onnx"));
    if (analyzer.isEmpty() || melt.isEmpty() || modelFile.isEmpty()) {
        m_status->setText(QStringLiteral("Анализатор человека или модель PP-HumanSegV2-Lite не найдены в этой сборке."));
        return;
    }
    const QString cacheRoot = backgroundCacheRoot();
    QDir cache(cacheRoot);
    if (cacheRoot.isEmpty() || !cache.mkpath(QStringLiteral("."))) {
        m_status->setText(QStringLiteral("Не удалось создать каталог данных проекта. Сохраните проект и повторите."));
        return;
    }
    const auto producer = model->getClipProducer(clipId);
    const QByteArray xml = backgroundAnalysisXml(producer, clip->getProducerProperty(QStringLiteral("kdenlive:proxy")), clip->url());
    if (xml.isEmpty()) {
        m_status->setText(QStringLiteral("Не удалось подготовить поток выбранного клипа для анализа."));
        return;
    }
    int quality = QSettings().value(QStringLiteral("StudioBackground/analysisSize"), 768).toInt();
    if (quality != 512 && quality != 768 && quality != 1280) quality = 768;
    const auto profile = pCore->getCurrentProfile().get();
    const int referenceW = profile->width(), referenceH = profile->height();
    const double scale = double(quality) / std::max(referenceW, referenceH);
    const int width = std::max(2, (qRound(referenceW * scale) / 2) * 2);
    const int height = std::max(2, (qRound(referenceH * scale) / 2) * 2);
    const bool staticImage = StudioBackground::canUseSingleFrameMask(clip->url(), xml);
    const int sourceIn = staticImage ? 0 : model->getClipIn(clipId);
    if (sourceIn < 0) {
        m_status->setText(QStringLiteral("Не удалось определить начало выбранного фрагмента."));
        return;
    }
    const int timelineFrames = model->getClipPlaytime(clipId);
    if (timelineFrames < 1) {
        m_status->setText(QStringLiteral("В выбранном фрагменте нет кадров для анализа."));
        return;
    }
    // get_playtime() on a cut already excludes sourceIn. Analyze the whole selection.
    const int frames = staticImage ? 1 : timelineFrames;
    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_backgroundXml = cache.absoluteFilePath(uuid + QStringLiteral(".analysis.mlt"));
    m_backgroundOutput = cache.absoluteFilePath(uuid + QStringLiteral(".sbg"));
    QSaveFile xmlFile(m_backgroundXml);
    if (!xmlFile.open(QIODevice::WriteOnly) || xmlFile.write(xml) != xml.size() || !xmlFile.commit()) {
        m_backgroundXml.clear(); m_backgroundOutput.clear();
        m_status->setText(QStringLiteral("Не удалось записать временное описание анализа."));
        return;
    }
    const QString modelSha = backgroundModelSha();
    m_backgroundRecipe = backgroundRecipeForXml(xml, quality, profile->frame_rate_num(), profile->frame_rate_den(), sourceIn);
    if (m_backgroundRecipe.isEmpty()) {
        QFile::remove(m_backgroundXml); m_backgroundXml.clear(); m_backgroundOutput.clear();
        m_status->setText(QStringLiteral("Не удалось рассчитать рецепт анализа выбранного клипа."));
        return;
    }
    m_backgroundStack = m_stack;
    m_backgroundJobValues = m_pendingBackgroundValues;
    m_backgroundAnalysisSize = quality;
    m_backgroundEffect = m_effect;
    m_backgroundClipId = clipId;
    m_backgroundClipIn = model->getClipIn(clipId);
    m_backgroundClipFrames = timelineFrames;
    m_backgroundSource = clip->url();
    m_backgroundCanceled = false;
    m_backgroundAnalyzerOutput.clear();
    m_backgroundProcessError.clear();
    if (m_backgroundProducer) m_backgroundProducer->deleteLater();
    if (m_backgroundAnalyzer) m_backgroundAnalyzer->deleteLater();
    m_backgroundProducer = new QProcess(this);
    m_backgroundAnalyzer = new QProcess(this);
    m_backgroundAnalyzer->setProgram(analyzer);
    m_backgroundAnalyzer->setArguments({QStringLiteral("--model"), modelFile, QStringLiteral("--model-sha256"), modelSha,
        QStringLiteral("--output"), m_backgroundOutput, QStringLiteral("--width"), QString::number(width),
        QStringLiteral("--height"), QString::number(height), QStringLiteral("--frames"), QString::number(frames),
        QStringLiteral("--fps-num"), QString::number(profile->frame_rate_num()), QStringLiteral("--fps-den"), QString::number(profile->frame_rate_den()),
        QStringLiteral("--reference-w"), QString::number(referenceW), QStringLiteral("--reference-h"), QString::number(referenceH),
        QStringLiteral("--first-sample"), QStringLiteral("0"), QStringLiteral("--source-file"), clip->url(),
        QStringLiteral("--recipe-sha256"), m_backgroundRecipe, QStringLiteral("--threads"), QStringLiteral("2")});
    m_backgroundProducer->setProgram(melt);
    m_backgroundProducer->setArguments({m_backgroundXml, QStringLiteral("in=%1").arg(sourceIn),
        QStringLiteral("out=%1").arg(qint64(sourceIn) + frames - 1),
        QStringLiteral("-consumer"), QStringLiteral("avformat:pipe:1"), QStringLiteral("f=rawvideo"), QStringLiteral("vcodec=rawvideo"),
        QStringLiteral("pix_fmt=rgba"), QStringLiteral("mlt_image_format=rgba"), QStringLiteral("an=1"),
        QStringLiteral("width=%1").arg(width), QStringLiteral("height=%1").arg(height), QStringLiteral("real_time=-1"),
        QStringLiteral("threads=2"), QStringLiteral("terminate_on_pause=1")});
    m_backgroundProducer->setStandardOutputProcess(m_backgroundAnalyzer);
    connect(m_backgroundProducer, &QProcess::readyReadStandardError, this, [this] {
        const QString diagnostic = QString::fromUtf8(m_backgroundProducer->readAllStandardError()).trimmed();
        if (diagnostic.isEmpty() || diagnostic.contains(QStringLiteral("QThreadStorage"))
            || diagnostic.contains(QStringLiteral("QDBusError"))) return;
        if (diagnostic.contains(QStringLiteral("Could not open"), Qt::CaseInsensitive)
            || diagnostic.contains(QStringLiteral("failed"), Qt::CaseInsensitive))
            m_backgroundProcessError = diagnostic.right(4096);
    });
    connect(m_backgroundAnalyzer, &QProcess::readyReadStandardOutput, this, [this] {
        m_backgroundAnalyzerOutput += QString::fromUtf8(m_backgroundAnalyzer->readAllStandardOutput());
        m_backgroundAnalyzerOutput = m_backgroundAnalyzerOutput.right(65536);
        const auto lines = m_backgroundAnalyzerOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (lines.isEmpty()) return;
        const auto progress = QJsonDocument::fromJson(lines.constLast().toUtf8()).object();
        if (progress.contains(QStringLiteral("done")))
            m_status->setText(QStringLiteral("Анализ человека: %1 из %2 кадров").arg(progress.value(QStringLiteral("done")).toInt())
                                  .arg(progress.value(QStringLiteral("total")).toInt()));
    });
    connect(m_backgroundAnalyzer, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this] { backgroundAnalysisFinished(); });
    connect(m_backgroundProducer, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this] { backgroundAnalysisFinished(); });
    connect(m_backgroundAnalyzer, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_backgroundProcessError = m_backgroundAnalyzer->errorString();
            backgroundAnalysisFinished();
        }
    });
    connect(m_backgroundProducer, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_backgroundProcessError = QStringLiteral("Не удалось запустить чтение клипа: %1").arg(m_backgroundProducer->errorString());
            backgroundAnalysisFinished();
        }
    });
    connect(m_backgroundAnalyzer, &QProcess::started, m_backgroundProducer, [producer = m_backgroundProducer] { producer->start(); });
    connect(pCore->undoStack().get(), &QUndoStack::indexChanged, m_backgroundAnalyzer, [this] {
        if (m_backgroundClipId >= 0) cancelAnalyses();
    });
    m_backgroundTabs->setEnabled(false);
    m_add->setText(QStringLiteral("Отменить"));
    m_status->setText(QStringLiteral("Анализ человека запущен. Старая рабочая маска остаётся активной до успешного завершения."));
    StudioJobs::start(m_backgroundAnalyzer, 20, m_backgroundProducer);
}

void StudioPanel::backgroundAnalysisFinished()
{
    if (m_backgroundClipId < 0) return;
    const auto failed = [](QProcess *process) {
        return !process || process->error() == QProcess::FailedToStart
            || (process->state() == QProcess::NotRunning
                && (process->exitStatus() != QProcess::NormalExit || process->exitCode() != 0));
    };
    const bool failedAnalysis = failed(m_backgroundAnalyzer) || failed(m_backgroundProducer);
    if (failedAnalysis || m_backgroundCanceled) {
        for (auto process : {m_backgroundProducer, m_backgroundAnalyzer})
            if (process && process->state() != QProcess::NotRunning) process->kill();
    }
    // A published SBG is usable only after both the decoder and worker succeed.
    for (auto process : {m_backgroundProducer, m_backgroundAnalyzer})
        if (process && process->state() != QProcess::NotRunning) return;
    const int clipId = std::exchange(m_backgroundClipId, -1);
    const bool canceled = m_backgroundCanceled || m_page != 7;
    const QString output = m_backgroundOutput, xmlPath = m_backgroundXml, recipe = m_backgroundRecipe;
    if (m_backgroundAnalyzer) m_backgroundAnalyzerOutput += QString::fromUtf8(m_backgroundAnalyzer->readAllStandardOutput());
    const QString analyzerError = m_backgroundAnalyzer ? QString::fromUtf8(m_backgroundAnalyzer->readAllStandardError()).trimmed() : QString();
    const QString error = !analyzerError.isEmpty() ? analyzerError : m_backgroundProcessError;
    QFile::remove(xmlPath);
    m_backgroundXml.clear(); m_backgroundOutput.clear(); m_backgroundRecipe.clear();
    m_backgroundTabs->setEnabled(true);
    if (m_page == 7) {
        m_add->setEnabled(true);
        m_add->setText(QStringLiteral("Проанализировать и применить"));
    }
    if (canceled || failedAnalysis) {
        QFile::remove(output);
        const QFileInfo partBase(output);
        QDir partFolder(partBase.absolutePath());
        for (const QString &part : partFolder.entryList({partBase.fileName() + QStringLiteral(".part.*")}, QDir::Files))
            partFolder.remove(part);
        if (m_page == 7) m_status->setText(canceled && m_backgroundProcessError.isEmpty() ? QStringLiteral("Анализ отменён. Предыдущая маска не изменена.")
                                  : error.isEmpty() ? QStringLiteral("Анализ человека не завершён. Эффект не изменён.")
                                                    : QStringLiteral("Анализ не завершён: %1").arg(error));
        return;
    }
    auto model = activeStudioModel();
    auto stack = m_backgroundStack.lock();
    if (!model || !model->isClip(clipId) || !model->getCurrentSelection().count(clipId)
        || !stack || model->getClipEffectStackModel(clipId) != stack
        || model->getClipIn(clipId) != m_backgroundClipIn || model->getClipPlaytime(clipId) != m_backgroundClipFrames) {
        QFile::remove(output);
        m_status->setText(QStringLiteral("Клип был удалён или изменён во время анализа; результат не применён."));
        return;
    }
    QJsonObject published;
    for (const auto &line : m_backgroundAnalyzerOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const auto object = QJsonDocument::fromJson(line.toUtf8()).object();
        if (object.value(QStringLiteral("published")).toBool()) published = object;
    }
    const QString sourceSha = published.value(QStringLiteral("source_sha256")).toString();
    if (sourceSha.size() != 64) {
        QFile::remove(output);
        m_status->setText(QStringLiteral("Анализатор не подтвердил исходный файл; результат не применён."));
        return;
    }
    const auto clip = pCore->projectItemModel()->getClipByBinID(model->getClipBinId(clipId));
    const auto profile = pCore->getCurrentProfile().get();
    if (!clip || !profile) {
        QFile::remove(output);
        m_status->setText(QStringLiteral("Исходный клип или профиль проекта изменился во время анализа."));
        return;
    }
    const int quality = m_backgroundAnalysisSize;
    const auto currentXml = backgroundAnalysisXml(model->getClipProducer(clipId), clip->getProducerProperty(QStringLiteral("kdenlive:proxy")), clip->url());
    const bool staticImage = StudioBackground::canUseSingleFrameMask(clip->url(), currentXml);
    if (clip->url() != m_backgroundSource || std::abs(model->getClipSpeed(clipId) - 1.0) > 1e-6
        || recipe != backgroundRecipeForXml(currentXml, quality, profile->frame_rate_num(), profile->frame_rate_den(),
                                            staticImage ? 0 : model->getClipIn(clipId))) {
        QFile::remove(output);
        m_status->setText(QStringLiteral("Исходник или эффекты клипа изменились во время анализа; прежняя маска сохранена."));
        return;
    }
    const StudioBackground::MaskRequest request{output, clip->url(), sourceSha, recipe, quality,
        profile->frame_rate_num(), profile->frame_rate_den(), profile->width(), profile->height(),
        0, model->getClipPlaytime(clipId), staticImage};
    const QString maskError = StudioBackground::validateMask(request);
    if (!maskError.isEmpty()) {
        QFile::remove(output);
        m_status->setText(QStringLiteral("Маска не применена: %1.").arg(maskError));
        return;
    }
    auto effect = std::dynamic_pointer_cast<EffectItemModel>(m_backgroundEffect.lock());
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    if (!effect) {
        if (!stack->appendEffectWithUndo(QStringLiteral("studio_background"), undo, redo).first) {
            QFile::remove(output); m_status->setText(QStringLiteral("Не удалось добавить эффект фона.")); return;
        }
        const auto effects = effectsById(stack, QStringLiteral("studio_background"));
        effect = effects.isEmpty() ? std::shared_ptr<EffectItemModel>() : effects.back();
        if (!effect || !moveBackgroundBeforeStudio(stack, effect, undo, redo)) {
            undo(); QFile::remove(output); m_status->setText(QStringLiteral("Не удалось безопасно разместить эффект фона.")); return;
        }
    }
    QJsonObject values = m_backgroundJobValues;
    values.insert(QStringLiteral("method"), 1);
    if (!values.contains(QStringLiteral("output"))) values.insert(QStringLiteral("output"), 1);
    QStringList names, after;
    for (const auto &name : m_backgroundParameters) { names << name; after << QString::number(values.value(name).toDouble(), 'g', 17); }
    names << QStringLiteral("mask_asset") << QStringLiteral("source_sha256") << QStringLiteral("recipe_sha256")
          << QStringLiteral("sample_offset") << QStringLiteral("analysis_size") << QStringLiteral("static_image");
    after << QFileInfo(output).fileName() << sourceSha << recipe << QStringLiteral("0") << QString::number(quality)
          << QString::number(staticImage ? 1 : 0);
    appendParameterChange(effect, names, after, undo, redo);
    StudioHelpers::setBackgroundClipIn(effect, model->getClipIn(clipId));
    StudioHelpers::appendMaskPathChange(effect, output, undo, redo);
    pCore->pushUndo(undo, redo, QStringLiteral("Проанализировать и применить фон"));
    m_pendingBackgroundValues = values;
    refreshSelection();
    m_status->setText(QStringLiteral("Маска человека рассчитана и применена. Ctrl+Z отменяет применение одним действием."));
}

void StudioPanel::resolveBackgroundAssets(int selected)
{
    auto model = activeStudioModel();
    const QString root = backgroundCacheRoot();
    if (!model || root.isEmpty()) return;
    const auto resolve = [&](int clipId) {
            if (!model->isClip(clipId)) return;
            const auto stack = model->getClipEffectStackModel(clipId);
            for (const auto &effect : effectsById(stack, QStringLiteral("studio_background"))) {
                StudioHelpers::setBackgroundClipIn(effect, model->getClipIn(clipId));
                const QString stored = effect->getParam(QStringLiteral("mask_asset"));
                const QString fileName = QFileInfo(stored).fileName();
                if (stored.isEmpty() || fileName != stored) {
                    StudioHelpers::setBackgroundMaskPath(effect, {});
                    continue;
                }
                const auto doc = pCore->currentDoc();
                const QString resolved = doc ? StudioBackground::maskPath(fileName, doc->projectDataFolder(), doc->documentRoot(), doc->url()) : QString();
                StudioHelpers::setBackgroundMaskPath(effect, resolved);
            }
    };
    if (selected >= 0) { resolve(selected); return; }
    for (int trackId : model->getAllTracksIds())
        for (int clipId : model->getItemsInRange(trackId, 0, -1, false)) resolve(clipId);
}

void StudioPanel::applyBackgroundPreset(const QJsonObject &preset)
{
    const auto values = preset.value(QStringLiteral("values")).toObject();
    if (values.size() != m_backgroundParameters.size()) {
        m_status->setText(QStringLiteral("Готовый вариант повреждён."));
        return;
    }
    const int method = std::clamp(values.value(QStringLiteral("method")).toInt(), 0, 1);
    m_backgroundMethod = method;
    m_backgroundKey = QColor::fromRgbF(values.value(QStringLiteral("key_r")).toDouble(), values.value(QStringLiteral("key_g")).toDouble(),
                                       values.value(QStringLiteral("key_b")).toDouble());
    m_backgroundFill = QColor::fromRgbF(values.value(QStringLiteral("fill_r")).toDouble(), values.value(QStringLiteral("fill_g")).toDouble(),
                                        values.value(QStringLiteral("fill_b")).toDouble());
    { QSignalBlocker blocker(m_backgroundTabs); m_backgroundTabs->setCurrentIndex(method); }
    if (!m_effect || (method == 1 && m_effect->getParam(QStringLiteral("mask_asset")).isEmpty())) {
        m_pendingBackgroundValues = values;
        m_add->show();
        m_add->setText(method == 0 ? QStringLiteral("Убрать фон") : QStringLiteral("Проанализировать и применить"));
        m_status->setText(method == 0 ? QStringLiteral("Вариант выбран. Нажмите «Убрать фон».")
                                     : QStringLiteral("Вариант выбран. Нажмите «Проанализировать и применить»."));
        refreshValues();
        paintBackgroundFrame();
        return;
    }
    QStringList names, before, after;
    for (const auto &name : m_backgroundParameters) {
        names << name;
        before << m_effect->getParam(name);
        after << QString::number(values.value(name).toDouble(), 'g', 17);
    }
    if (before != after) pCore->pushUndo(new StudioCommand(m_effect, names, before, after));
    m_add->setVisible(method == 1);
    m_add->setText(QStringLiteral("Проанализировать заново"));
    refreshValues();
}

void StudioPanel::loadTrackingFrame()
{
    if (!m_trackingFrame || m_page != 2) return;
    const auto request = ++m_frameRequest;
    m_trackingImage = QImage();
    m_trackingPoint = QPointF(-1, -1);
    m_trackingFrame->clear();
    m_trackingFrame->setText(QStringLiteral("Выберите видеоклип на таймлайне"));
    auto timeline = pCore && pCore->window() ? pCore->window()->getCurrentTimeline() : nullptr;
    auto model = timeline ? timeline->model() : nullptr;
    auto clip = model && m_selectedClipId >= 0 ? pCore->bin()->getBinClip(model->getClipBinId(m_selectedClipId)) : nullptr;
    if (!clip || !clip->hasUrl()) {
        if (m_trackingApply) m_trackingApply->setEnabled(false);
        return;
    }
    const int selected = m_selectedClipId;
    m_trackingFrame->setText(QStringLiteral("Подготовка стоп-кадра…"));
    StudioJobs::thumbnail(clip, model->getClipIn(selected), this, [this, request, selected](QImage image) {
        if (request != m_frameRequest || selected != m_selectedClipId || m_page != 2) return;
        m_trackingImage = image;
        if (image.isNull()) m_trackingFrame->setText(QStringLiteral("Не удалось показать стоп-кадр этого клипа"));
        else paintTrackingFrame();
    });
    if (m_trackingApply) m_trackingApply->setEnabled(false);
    bool missingTrack = false;
    if (m_effect && m_effect->getParam(QStringLiteral("tracking")).toDouble() > .5) {
        const QString stored = m_effect->getParam(QStringLiteral("track_path"));
        const QUrl url(stored);
        const QString path = url.isLocalFile() ? url.toLocalFile() : stored;
        missingTrack = path.isEmpty() || !QFileInfo::exists(path);
    }
    m_status->setText(missingTrack
        ? QStringLiteral("Данные трекинга недоступны. Нажмите на лицо и примените трекинг заново.")
        : QStringLiteral("Нажмите на лицо в стоп-кадре, выберите приближение и примените трекинг."));
}

void StudioPanel::paintTrackingFrame()
{
    if (!m_trackingFrame || m_trackingImage.isNull()) return;
    const int width = std::clamp(m_scroll->viewport()->width() - 36, 240, 720);
    QImage image = m_trackingImage.scaled(QSize(width, 360), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (m_trackingPoint.x() >= 0 && m_trackingPoint.y() >= 0) {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(60, 190, 240), 3);
        painter.setPen(pen);
        const QPointF point(m_trackingPoint.x() * image.width(), m_trackingPoint.y() * image.height());
        painter.drawEllipse(point, 9, 9);
        painter.drawLine(point + QPointF(-16, 0), point + QPointF(16, 0));
        painter.drawLine(point + QPointF(0, -16), point + QPointF(0, 16));
    }
    m_trackingFrame->setPixmap(QPixmap::fromImage(image));
}

void StudioPanel::runHeadTracking()
{
    if (m_trackingImage.isNull() || m_trackingPoint.x() < 0 || m_trackingPoint.y() < 0) {
        m_status->setText(QStringLiteral("Сначала нажмите на лицо в стоп-кадре."));
        return;
    }
    auto timeline = pCore && pCore->window() ? pCore->window()->getCurrentTimeline() : nullptr;
    auto model = timeline ? timeline->model() : nullptr;
    const int clipId = m_selectedClipId;
    auto clip = model && clipId >= 0 ? pCore->bin()->getBinClip(model->getClipBinId(clipId)) : nullptr;
    if (!clip || !QFileInfo::exists(clip->url())) {
        m_status->setText(QStringLiteral("Исходный файл клипа недоступен. Выберите существующий файл или восстановите медиа."));
        return;
    }
    const double speed = model->getClipSpeed(clipId);
    if (std::abs(speed - 1.0) > 1e-6) {
        m_status->setText(QStringLiteral("Трекинг требует обычную скорость клипа. Верните скорость 100% и повторите анализ."));
        return;
    }
    const QString program = StudioResources::executable(QStringLiteral("studio-head-tracker"));
    if (program.isEmpty()) {
        m_status->setText(QStringLiteral("Модуль анализа головы не найден в этой сборке."));
        return;
    }
    const QPointF point = m_trackingPoint;
    const double zoom = m_pendingTrackingZoom;
    if (!m_stack || StudioJobs::busy(m_tracker)) return;
    const double fps = pCore->getCurrentFps();
    const double start = model->getClipIn(clipId) / fps;
    const double duration = model->getClipPlaytime(clipId) / fps;
    // ponytail: a click seeds a head-sized box; add face detection only if real footage proves this heuristic too brittle.
    constexpr double boxWidth = .20;
    constexpr double boxHeight = .30;
    const QRectF normalized(std::clamp(point.x() - boxWidth / 2, 0.0, 1.0 - boxWidth),
                            std::clamp(point.y() - boxHeight / 2, 0.0, 1.0 - boxHeight), boxWidth, boxHeight);
    auto document = pCore ? pCore->currentDoc() : nullptr;
    const QString documentId = document ? document->getDocumentProperty(QStringLiteral("documentid")).trimmed() : QString();
    bool validDocumentId = false;
    documentId.toLongLong(&validDocumentId);
    if (!document || document->projectDataFolder().isEmpty() || !validDocumentId) {
        m_status->setText(QStringLiteral("Не удалось определить папку проекта. Сохраните проект и повторите анализ."));
        return;
    }
    QDir tracks(QDir(document->projectDataFolder()).absoluteFilePath(QStringLiteral("studio-tracks/%1").arg(documentId)));
    if (!tracks.mkpath(QStringLiteral("."))) {
        m_status->setText(QStringLiteral("Не удалось создать каталог для треков головы."));
        return;
    }
    m_trackingOutput = tracks.absoluteFilePath(QStringLiteral("head_%1.scam").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    m_trackingEffect = m_effect;
    m_trackingStack = m_stack;
    m_trackingClipId = clipId;
    m_trackingZoom = storedValue(m_controls[QStringLiteral("zoom")].spec, zoom);
    if (m_tracker) m_tracker->deleteLater();
    m_tracker = new QProcess(this);
    m_tracker->setProgram(program);
    m_tracker->setArguments({clip->url(), m_trackingOutput, QString::number(start, 'g', 17), QString::number(duration, 'g', 17),
                             QString::number(normalized.x(), 'g', 17), QString::number(normalized.y(), 'g', 17),
                             QString::number(normalized.width(), 'g', 17), QString::number(normalized.height(), 'g', 17)});
    connect(m_tracker, &QProcess::readyReadStandardOutput, this, [this] {
        const QString output = QString::fromUtf8(m_tracker->readAllStandardOutput());
        const qsizetype marker = output.lastIndexOf(QStringLiteral("PROGRESS "));
        if (marker >= 0) m_status->setText(QStringLiteral("Анализ головы: %1%").arg(output.mid(marker + 9).section(QLatin1Char('\n'), 0, 0).trimmed()));
    });
    connect(m_tracker, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) { headTrackingFinished(code); });
    connect(m_tracker, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) headTrackingFinished(-1);
    });
    connect(pCore->undoStack().get(), &QUndoStack::indexChanged, m_tracker, [this] {
        if (StudioJobs::busy(m_tracker)) cancelAnalyses();
    });
    if (m_headCancel) m_headCancel->show();
    if (m_trackingApply) m_trackingApply->setEnabled(false);
    m_status->setText(QStringLiteral("Анализ головы запущен. Можно продолжать работу в проекте."));
    StudioJobs::start(m_tracker);
}

void StudioPanel::headTrackingFinished(int exitCode)
{
    if (m_trackingOutput.isEmpty()) return;
    const QString output = std::exchange(m_trackingOutput, QString());
    if (m_page != 2) {
        QFile::remove(output);
        QFile::remove(output + QStringLiteral(".tmp"));
        return;
    }
    QString error = m_tracker ? QString::fromUtf8(m_tracker->readAllStandardError()).trimmed() : QString();
    if (error.isEmpty() && m_tracker && exitCode != 0) error = m_tracker->errorString();
    if (m_headCancel) m_headCancel->hide();
    if (m_trackingApply) m_trackingApply->setEnabled(!m_trackingImage.isNull() && m_trackingPoint.x() >= 0);
    auto effect = m_trackingEffect.lock();
    auto stack = m_trackingStack.lock();
    auto timeline = pCore && pCore->window() ? pCore->window()->getCurrentTimeline() : nullptr;
    auto model = timeline ? timeline->model() : nullptr;
    QFile file(output);
    if (exitCode != 0 || !stack || !model || !model->isClip(m_trackingClipId)
        || model->getClipEffectStackModel(m_trackingClipId) != stack
        || !file.open(QIODevice::ReadOnly) || file.readLine().trimmed() != QByteArray("SUNIMO_CAMERA_TRACK_V1")) {
        file.close();
        QFile::remove(output);
        QFile::remove(output + QStringLiteral(".tmp"));
        m_status->setText(error.isEmpty() ? QStringLiteral("Анализ головы не завершён. Трек не применён.")
                                          : QStringLiteral("Не удалось создать трек: %1").arg(error));
        return;
    }
    file.close();
    Fun undo = [] { return true; };
    Fun redo = [] { return true; };
    if (!effect) {
        if (!stack->appendEffectWithUndo(QStringLiteral("studio_camera"), undo, redo).first) {
            QFile::remove(output);
            m_status->setText(QStringLiteral("Не удалось добавить камеру для готового трека."));
            return;
        }
        const auto effects = effectsById(stack, QStringLiteral("studio_camera"));
        effect = effects.isEmpty() ? nullptr : effects.back();
        if (!effect || !moveCameraBeforeCard(stack, undo, redo)) {
            undo(); QFile::remove(output);
            m_status->setText(QStringLiteral("Не удалось безопасно разместить камеру."));
            return;
        }
    }
    appendParameterChange(effect, {QStringLiteral("tracking"), QStringLiteral("track_path"), QStringLiteral("track_offset"), QStringLiteral("zoom")},
                          {QStringLiteral("1"), output, QStringLiteral("0"), m_trackingZoom}, undo, redo);
    pCore->pushUndo(undo, redo, QStringLiteral("Применить трекинг головы"));
    refreshSelection();
    m_status->setText(QStringLiteral("Трек головы создан и применён. Ctrl+Z отменяет применение одним действием."));
}

bool StudioPanel::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_backgroundFrame || watched == m_scroll->viewport()) && event->type() == QEvent::Resize && m_page == 7) paintBackgroundFrame();
    if (watched == m_backgroundFrame && event->type() == QEvent::MouseButtonRelease && m_backgroundMethod == 0
        && !m_backgroundImage.isNull()) {
        auto mouse = static_cast<QMouseEvent *>(event);
        const QPixmap pixmap = m_backgroundFrame->pixmap();
        const QRect imageRect((m_backgroundFrame->width() - pixmap.width()) / 2,
                              (m_backgroundFrame->height() - pixmap.height()) / 2,
                              pixmap.width(), pixmap.height());
        if (mouse->button() == Qt::LeftButton && imageRect.contains(mouse->position().toPoint())) {
            const int cx = std::clamp(int((mouse->position().x() - imageRect.left()) * m_backgroundImage.width() / imageRect.width()),
                                      0, m_backgroundImage.width() - 1);
            const int cy = std::clamp(int((mouse->position().y() - imageRect.top()) * m_backgroundImage.height() / imageRect.height()),
                                      0, m_backgroundImage.height() - 1);
            QVector<int> red, green, blue;
            for (int y = std::max(0, cy - 3); y <= std::min(m_backgroundImage.height() - 1, cy + 3); ++y) {
                for (int x = std::max(0, cx - 3); x <= std::min(m_backgroundImage.width() - 1, cx + 3); ++x) {
                    const QColor sample = m_backgroundImage.pixelColor(x, y);
                    if (sample.alpha() < 128) continue;
                    red << sample.red(); green << sample.green(); blue << sample.blue();
                }
            }
            if (!red.isEmpty()) {
                auto median = [](QVector<int> values) {
                    auto middle = values.begin() + values.size() / 2;
                    std::nth_element(values.begin(), middle, values.end());
                    return *middle;
                };
                m_backgroundKey = QColor(median(red), median(green), median(blue));
                const QStringList names{QStringLiteral("key_r"), QStringLiteral("key_g"), QStringLiteral("key_b")};
                const QStringList after{QString::number(m_backgroundKey.redF(), 'g', 17),
                                        QString::number(m_backgroundKey.greenF(), 'g', 17),
                                        QString::number(m_backgroundKey.blueF(), 'g', 17)};
                if (m_effect) {
                    const QStringList before{m_effect->getParam(names[0]), m_effect->getParam(names[1]), m_effect->getParam(names[2])};
                    if (before != after) pCore->pushUndo(new StudioCommand(m_effect, names, before, after));
                } else {
                    for (int i = 0; i < names.size(); ++i) m_pendingBackgroundValues.insert(names[i], after[i].toDouble());
                }
                paintBackgroundFrame();
                m_status->setText(QStringLiteral("Цвет фона выбран. Нажмите «Убрать фон»."));
            }
            return true;
        }
    }
    if (watched == m_trackingFrame && event->type() == QEvent::MouseButtonRelease && !m_trackingImage.isNull()) {
        auto mouse = static_cast<QMouseEvent *>(event);
        const QPixmap pixmap = m_trackingFrame->pixmap();
        const QRect imageRect((m_trackingFrame->width() - pixmap.width()) / 2,
                              (m_trackingFrame->height() - pixmap.height()) / 2,
                              pixmap.width(), pixmap.height());
        if (mouse->button() == Qt::LeftButton && imageRect.contains(mouse->position().toPoint())) {
            m_trackingPoint = QPointF(double(mouse->position().x() - imageRect.left()) / imageRect.width(),
                                      double(mouse->position().y() - imageRect.top()) / imageRect.height());
            paintTrackingFrame();
            if (m_trackingApply)
                m_trackingApply->setEnabled(currentTarget() && supported() && !StudioJobs::busy(m_tracker));
            m_status->setText(QStringLiteral("Лицо выбрано. Проверьте приближение и нажмите «Применить трекинг»."));
            return true;
        }
    }
    auto button = qobject_cast<QToolButton *>(watched);
    if (button && event->type() == QEvent::Enter) { m_hoverPreview = button; m_hoverTimer->start(); }
    if (button && event->type() == QEvent::Leave) {
        if (m_hoverPreview == button) { m_hoverTimer->stop(); m_hoverPreview.clear(); }
        if (m_preview == button) stopPreview();
    }
    if (button && event->type() == QEvent::KeyPress) {
        auto key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Space) {
            if (m_preview == button) stopPreview(); else startPreview(button);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
void StudioPanel::cancelAnalyses()
{
    if (StudioJobs::busy(m_tracker)) {
        m_trackingStack.reset();
        StudioJobs::cancel(m_tracker);
    }
    if (m_backgroundClipId >= 0) {
        m_backgroundCanceled = true;
        if (m_backgroundProducer) m_backgroundProducer->kill();
        if (m_backgroundAnalyzer) {
            StudioJobs::cancel(m_backgroundAnalyzer);
            QPointer<QProcess> process = m_backgroundAnalyzer;
            QTimer::singleShot(1000, this, [process] {
                if (process && process->state() != QProcess::NotRunning) process->kill();
            });
        }
    }
}

void StudioPanel::hideEvent(QHideEvent *event) { stopPreview(); finishDrag(); stopFraming(); cancelAnalyses(); if (m_audioController) m_audioController->cancel(); if (m_textPage) m_textPage->deactivate(); if (m_subtitlePage) m_subtitlePage->deactivate(); QWidget::hideEvent(event); }
void StudioPanel::showEvent(QShowEvent *event) { QWidget::showEvent(event); refreshSelection(); }
