// SPDX-License-Identifier: GPL-3.0-only
#include "studiotext.hpp"
#include "../core/studioresources.hpp"
#include "../studiohelpers.hpp"
#include "scenecompiler.hpp"
#include "styles.hpp"

#include "bin/projectfolder.h"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "definitions.h"
#include "doc/kdenlivedoc.h"
#include "mainwindow.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/view/timelinewidget.h"
#include "xml/xml.hpp"

#include <QComboBox>
#include <QCompleter>
#include <QCheckBox>
#include <QColorDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDomDocument>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QPixmap>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

namespace {
constexpr auto textEffectId = "sunimo_text_studio";

QString sceneReadError(const QString &path)
{
    if (path.isEmpty()) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("Файл надписи недоступен: %1. Восстановите файлы проекта.").arg(QFileInfo(path).fileName());
    if (SunimoTextQt::metadata(path).isEmpty())
        return QStringLiteral("Файл надписи не содержит читаемых настроек: %1. Обновление недоступно.").arg(QFileInfo(path).fileName());
    return {};
}

std::pair<std::shared_ptr<TimelineItemModel>, int> selectedVideo()
{
    auto window = pCore ? pCore->window() : nullptr;
    auto widget = window && !pCore->closing ? window->getCurrentTimeline() : nullptr;
    auto timeline = widget ? widget->model() : nullptr;
    if (!window && pCore && !pCore->closing && pCore->currentDoc() && !pCore->currentDoc()->closing) {
        auto active = pCore->projectManager()->getTimeline();
        if (active && pCore->currentDoc()->getTimeline(active->uuid(), true) == active) timeline = active;
    }
    if (!timeline) return {nullptr, -1};
    int selected = -1;
    for (int id : timeline->getCurrentSelection()) {
        if (!timeline->isClip(id) || timeline->clipIsAudio(id)) continue;
        if (selected >= 0) return {timeline, -1};
        selected = id;
    }
    return {timeline, selected};
}

struct PendingText {
    Fun undo = [] { return true; };
    Fun redo = [] { return true; };
};
}

bool StudioText::createTitle(const std::shared_ptr<TimelineItemModel> &timeline, int baseClip, int frames,
                             const QString &name, const QString &scene, const std::function<void(bool, const QString &)> &finished)
{
    if (!timeline || !timeline->isClip(baseClip) || frames < 1 || !QFile::exists(scene)) return false;
    const int baseTrack = timeline->getClipTrackId(baseClip);
    if (baseTrack < 0 || timeline->trackIsLocked(baseTrack)) return false;
    const auto &profile = pCore->getProjectProfile();
    QDomDocument description;
    auto source = description.createElement(QStringLiteral("producer"));
    source.setAttribute(QStringLiteral("type"), int(ClipType::Text));
    source.setAttribute(QStringLiteral("in"), 0);
    source.setAttribute(QStringLiteral("length"), frames);
    description.appendChild(source);
    Xml::setXmlProperty(source, QStringLiteral("mlt_service"), QStringLiteral("kdenlivetitle"));
    Xml::setXmlProperty(source, QStringLiteral("kdenlive:clipname"), name);
    Xml::setXmlProperty(source, QStringLiteral("xmldata"),
                        QStringLiteral("<kdenlivetitle width=\"%1\" height=\"%2\" LC_NUMERIC=\"C\"><background color=\"0,0,0,0\"/></kdenlivetitle>")
                            .arg(profile.width()).arg(profile.height()));
    const int position = timeline->getItemPosition(baseClip);
    const auto document = pCore->currentDoc();
    auto pending = std::make_shared<PendingText>();
    QString binId;
    auto bin = pCore->projectItemModel();
    return bin->requestAddBinClip(binId, source, bin->getRootFolder()->clipId(), pending->undo, pending->redo,
        [pending, timeline, baseClip, position, frames, document, scene, finished](const QString &readyId) {
            const auto fail = [&] (const QString &reason) {
                if (pending->undo()) QFile::remove(scene);
                finished(false, reason);
            };
            if (!pCore || pCore->currentDoc() != document || !timeline->isClip(baseClip)
                || timeline->getItemPosition(baseClip) != position) {
                fail(QStringLiteral("Монтаж изменился во время добавления надписи.")); return;
            }
            const int baseTrack = timeline->getClipTrackId(baseClip);
            int target = -1;
            for (int id : timeline->getTracksIds(false))
                if (timeline->getTrackPosition(id) > timeline->getTrackPosition(baseTrack) && !timeline->trackIsLocked(id)
                    && timeline->trackIsAvailable(id, position, frames, -1)) {
                    target = id; break;
                }
            if (target < 0 && !timeline->requestTrackInsertion(-1, target, QStringLiteral("Надписи"), false, pending->undo, pending->redo)) {
                fail(QStringLiteral("Не удалось создать дорожку надписей.")); return;
            }
            int titleId = -1;
            if (!timeline->requestClipInsertion(readyId, target, position, titleId, false, true, false, pending->undo, pending->redo)) {
                fail(QStringLiteral("Нет места для надписи на верхней дорожке.")); return;
            }
            auto stack = timeline->getClipEffectStack(titleId);
            if (!stack || !stack->appendEffectWithUndo(QString::fromLatin1(textEffectId), pending->undo, pending->redo).first) {
                fail(QStringLiteral("Не удалось применить движок надписей.")); return;
            }
            const auto textEffects = StudioHelpers::effectsById(stack, QString::fromLatin1(textEffectId));
            if (textEffects.size() != 1) { fail(QStringLiteral("Не удалось найти добавленную надпись.")); return; }
            StudioHelpers::appendParameterChange(textEffects.front(), {QStringLiteral("0")}, {scene}, pending->undo, pending->redo);
            pCore->pushUndo(pending->undo, pending->redo, QStringLiteral("Добавить надпись"));
            timeline->requestSetSelection({titleId});
            finished(true, QStringLiteral("Надпись добавлена на отдельную дорожку."));
        });
}

bool StudioText::updateTitle(const std::shared_ptr<TimelineItemModel> &timeline, int clipId,
                             const std::shared_ptr<EffectItemModel> &effect, int frames, const QString &scene)
{
    if (!timeline || !timeline->isClip(clipId) || !effect || effect->getAssetId() != QLatin1String(textEffectId)
        || frames < 1 || !QFile::exists(scene)) return false;
    const int track = timeline->getClipTrackId(clipId);
    if (track < 0 || timeline->trackIsLocked(track)) return false;
    Fun undo = [] { return true; }, redo = [] { return true; };
    auto binClip = pCore->projectItemModel()->getClipByBinID(timeline->getClipBinId(clipId));
    if (binClip && binClip->clipType() == ClipType::Text && frames != timeline->getClipPlaytime(clipId)) {
        int requested = frames;
        if (!timeline->requestItemResize(clipId, requested, true, false, undo, redo) || requested != frames) {
            undo();
            return false;
        }
    }
    StudioHelpers::appendParameterChange(effect, {QStringLiteral("0")}, {scene}, undo, redo);
    pCore->pushUndo(undo, redo, QStringLiteral("Изменить надпись"));
    return true;
}

StudioTextPage::StudioTextPage(QHash<QString, QJsonObject> *drafts, QWidget *parent)
    : QWidget(parent), m_drafts(drafts)
{
    setObjectName(QStringLiteral("studioTextPage"));
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_target = new QLabel(this);
    m_target->setWordWrap(true);
    layout->addWidget(m_target);
    m_styleTabs = new QTabBar(this);
    m_styleTabs->addTab(QStringLiteral("Готовые"));
    m_styleTabs->addTab(QStringLiteral("Мои стили"));
    layout->addWidget(m_styleTabs);
    m_style = new QComboBox(this);
    m_style->setObjectName(QStringLiteral("studioTextStyle"));
    m_style->addItem(QStringLiteral("Ручные настройки"), 0);
    for (const auto &style : SunimoTextQt::styles)
        m_style->addItem(QStringLiteral("%1 · %2").arg(QString::fromUtf8(style.family), QString::fromUtf8(style.name)), style.id);
    m_style->addItem(QStringLiteral("Только движение"), -1);
    m_style->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    layout->addWidget(m_style);
    auto customRow = new QWidget(this);
    auto customLayout = new QGridLayout(customRow);
    customLayout->setContentsMargins(0, 0, 0, 0);
    m_customList = new QComboBox(this);
    m_customList->setObjectName(QStringLiteral("studioTextCustomStyles"));
    m_customList->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto saveStyle = new QPushButton(QStringLiteral("Сохранить"), this);
    auto deleteStyle = new QPushButton(QStringLiteral("Удалить"), this);
    saveStyle->setObjectName(QStringLiteral("studioTextSaveStyle"));
    deleteStyle->setObjectName(QStringLiteral("studioTextDeleteStyle"));
    customLayout->addWidget(m_customList, 0, 0, 1, 2);
    customLayout->addWidget(saveStyle, 1, 0); customLayout->addWidget(deleteStyle, 1, 1);
    layout->addWidget(customRow);
    customRow->hide();
    connect(m_styleTabs, &QTabBar::currentChanged, this, [this, customRow](int tab) {
        m_style->setVisible(tab == 0); customRow->setVisible(tab == 1);
    });
    loadCustomStyles();
    connect(saveStyle, &QPushButton::clicked, this, [this] {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Мой стиль"), QStringLiteral("Название"),
                                                   QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted || name.isEmpty() || name.size() > 80) return;
        QJsonObject values = settings();
        values.remove(QStringLiteral("text")); values.remove(QStringLiteral("duration"));
        values.remove(QStringLiteral("fpsNum")); values.remove(QStringLiteral("fpsDen"));
        for (int i = 0; i < m_customStyles.size(); ++i) if (m_customStyles[i].toObject().value(QStringLiteral("name")).toString() == name) {
            m_customStyles.removeAt(i); break;
        }
        m_customStyles.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("values"), values}});
        saveCustomStyles();
        m_customList->setCurrentText(name);
    });
    connect(deleteStyle, &QPushButton::clicked, this, [this] {
        const int index = m_customList->currentIndex();
        if (index < 0 || index >= m_customStyles.size()) return;
        m_customStyles.removeAt(index); saveCustomStyles();
    });
    connect(m_customList, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (index < 0 || index >= m_customStyles.size()) return;
        QJsonObject values = settings();
        const auto saved = m_customStyles[index].toObject().value(QStringLiteral("values")).toObject();
        for (auto it = saved.begin(); it != saved.end(); ++it) values.insert(it.key(), it.value());
        loadSettings(values); queuePreview();
    });
    auto textLabel = new QLabel(QStringLiteral("Текст надписи"), this);
    layout->addWidget(textLabel);
    m_text = new QPlainTextEdit(this);
    m_text->setObjectName(QStringLiteral("studioTextInput"));
    m_text->setPlaceholderText(QStringLiteral("Напишите надпись"));
    m_text->setMinimumHeight(78);
    m_text->setMaximumHeight(110);
    textLabel->setBuddy(m_text);
    layout->addWidget(m_text);
    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("studioTextPreview"));
    m_preview->setText(QStringLiteral("Предпросмотр надписи"));
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumHeight(120);
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_preview->installEventFilter(this);
    layout->addWidget(m_preview);
    m_seek = new QSlider(Qt::Horizontal, this);
    m_seek->setObjectName(QStringLiteral("studioTextSeek"));
    m_seek->setRange(0, 1000);
    layout->addWidget(m_seek);
    auto previewControls = new QGridLayout;
    m_play = new QPushButton(QStringLiteral("Играть"), this);
    m_play->setObjectName(QStringLiteral("studioTextPlay"));
    auto stop = new QPushButton(QStringLiteral("Стоп"), this);
    auto middle = new QPushButton(QStringLiteral("Середина"), this);
    previewControls->addWidget(m_play, 0, 0); previewControls->addWidget(stop, 0, 1);
    previewControls->addWidget(middle, 1, 0, 1, 2);
    layout->addLayout(previewControls);
    m_previewTimer = new QTimer(this);
    m_previewTimer->setInterval(80);
    m_previewUpdate = new QTimer(this);
    m_previewUpdate->setSingleShot(true);
    m_previewUpdate->setInterval(120);
    connect(m_previewUpdate, &QTimer::timeout, this, &StudioTextPage::renderPreview);
    connect(m_previewTimer, &QTimer::timeout, this, [this] {
        const int next = m_seek->value() + qRound(80. / (m_duration->value() * 1000) * 1000);
        if (next >= 1000) { m_seek->setValue(1000); stopPreview(); }
        else m_seek->setValue(next);
    });
    connect(m_seek, &QSlider::valueChanged, this, [this] {
        if (m_previewTimer->isActive()) renderPreview(); else queuePreview();
    });
    connect(m_play, &QPushButton::clicked, this, [this] {
        if (m_previewTimer->isActive()) stopPreview();
        else { if (m_seek->value() >= 1000) m_seek->setValue(0); m_previewTimer->start(); m_play->setText(QStringLiteral("Пауза")); }
    });
    connect(stop, &QPushButton::clicked, this, [this] { stopPreview(); m_seek->setValue(0); });
    connect(middle, &QPushButton::clicked, m_seek, [this] { m_seek->setValue(500); });
    m_status = new QLabel(this); m_status->setWordWrap(true); layout->addWidget(m_status);
    const auto section = [this, layout](const QString &title, bool open) {
        auto toggle = new QToolButton(this); toggle->setText(title); toggle->setCheckable(true); toggle->setChecked(open);
        layout->addWidget(toggle);
        auto body = new QWidget(this);
        auto form = new QFormLayout(body);
        form->setContentsMargins(0, 0, 0, 0);
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        layout->addWidget(body); body->setVisible(open);
        connect(toggle, &QToolButton::toggled, body, &QWidget::setVisible);
        return form;
    };
    auto form = section(QStringLiteral("Оформление"), true);
    m_font = new QFontComboBox(this);
    m_font->setObjectName(QStringLiteral("studioTextFont"));
    m_font->setAccessibleName(QStringLiteral("Шрифт надписи"));
    m_font->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    form->addRow(QStringLiteral("Шрифт"), m_font);
    m_size = new QSpinBox(this); m_size->setRange(12, 320); m_size->setValue(64); m_size->setSuffix(QStringLiteral(" px"));
    m_size->setObjectName(QStringLiteral("studioTextSize"));
    form->addRow(QStringLiteral("Размер текста"), m_size);
    m_bold = new QCheckBox(QStringLiteral("Жирный"), this); m_bold->setChecked(true);
    m_bold->setObjectName(QStringLiteral("studioTextBold"));
    form->addRow(QString(), m_bold);
    m_italic = new QCheckBox(QStringLiteral("Курсив"), this);
    m_italic->setObjectName(QStringLiteral("studioTextItalic"));
    form->addRow(QString(), m_italic);
    m_color = new QPushButton(this);
    m_color->setText(QStringLiteral("Выбрать цвет (%1)").arg(m_textColor.name(QColor::HexArgb)));
    form->addRow(QStringLiteral("Цвет"), m_color);
    connect(m_color, &QPushButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(m_textColor, this, QStringLiteral("Цвет надписи"), QColorDialog::ShowAlphaChannel);
        if (!chosen.isValid()) return;
        m_textColor = chosen;
        m_color->setText(QStringLiteral("Выбрать цвет (%1)").arg(m_textColor.name(QColor::HexArgb)));
    });
    m_outlineEnabled = new QCheckBox(QStringLiteral("Обводка"), this);
    m_outlineEnabled->setObjectName(QStringLiteral("studioTextOutlineEnabled"));
    form->addRow(QString(), m_outlineEnabled);
    m_outlineWidth = new QSpinBox(this);
    m_outlineWidth->setRange(0, 20);
    m_outlineWidth->setValue(3);
    m_outlineWidth->setSuffix(QStringLiteral(" px"));
    m_outlineWidth->setObjectName(QStringLiteral("studioTextOutlineWidth"));
    form->addRow(QStringLiteral("Толщина обводки"), m_outlineWidth);
    m_outlineColor = new QPushButton(this);
    m_outlineColor->setText(QStringLiteral("Выбрать цвет (%1)").arg(m_outlineColorValue.name(QColor::HexArgb)));
    m_outlineColor->setObjectName(QStringLiteral("studioTextOutlineColor"));
    form->addRow(QStringLiteral("Цвет обводки"), m_outlineColor);
    connect(m_outlineColor, &QPushButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(m_outlineColorValue, this, QStringLiteral("Цвет обводки"), QColorDialog::ShowAlphaChannel);
        if (!chosen.isValid()) return;
        m_outlineColorValue = chosen;
        m_outlineColor->setText(QStringLiteral("Выбрать цвет (%1)").arg(m_outlineColorValue.name(QColor::HexArgb)));
    });
    connect(m_outlineEnabled, &QCheckBox::toggled, this, [form, this](bool enabled) {
        form->setRowVisible(m_outlineWidth, enabled);
        form->setRowVisible(m_outlineColor, enabled);
    });
    form->setRowVisible(m_outlineWidth, false);
    form->setRowVisible(m_outlineColor, false);
    m_opacity = new QSpinBox(this); m_opacity->setRange(0, 100); m_opacity->setValue(100); m_opacity->setSuffix(QStringLiteral(" %"));
    m_opacity->setObjectName(QStringLiteral("studioTextOpacity"));
    form->addRow(QStringLiteral("Непрозрачность"), m_opacity);
    auto addColorButton = [this, form](const QString &label, const QString &title, QColor *value) {
        auto button = new QPushButton(QStringLiteral("Выбрать цвет (%1)").arg(value->name(QColor::HexArgb)), this);
        form->addRow(label, button);
        connect(button, &QPushButton::clicked, this, [this, button, title, value] {
            const QColor chosen = QColorDialog::getColor(*value, this, title, QColorDialog::ShowAlphaChannel);
            if (!chosen.isValid()) return;
            *value = chosen;
            button->setText(QStringLiteral("Выбрать цвет (%1)").arg(value->name(QColor::HexArgb)));
        });
        return button;
    };
    m_shadow = new QSpinBox(this); m_shadow->setRange(0, 20); m_shadow->setSuffix(QStringLiteral(" px"));
    m_shadow->setObjectName(QStringLiteral("studioTextShadow"));
    form->addRow(QStringLiteral("Тень"), m_shadow);
    m_shadowColorButton = addColorButton(QStringLiteral("Цвет тени"), QStringLiteral("Цвет тени"), &m_shadowColorValue);
    m_shadowColorButton->setObjectName(QStringLiteral("studioTextShadowColor"));
    connect(m_shadow, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [form, this](int value) { form->setRowVisible(m_shadowColorButton, value > 0); });
    form->setRowVisible(m_shadowColorButton, false);
    m_backgroundEnabled = new QCheckBox(QStringLiteral("Плашка"), this);
    m_backgroundEnabled->setObjectName(QStringLiteral("studioTextBackgroundEnabled"));
    form->addRow(QString(), m_backgroundEnabled);
    m_backgroundColorButton = addColorButton(QStringLiteral("Цвет плашки"), QStringLiteral("Цвет плашки"), &m_backgroundColorValue);
    m_backgroundColorButton->setObjectName(QStringLiteral("studioTextBackgroundColor"));
    connect(m_backgroundEnabled, &QCheckBox::toggled, this,
            [form, this](bool enabled) { form->setRowVisible(m_backgroundColorButton, enabled); });
    form->setRowVisible(m_backgroundColorButton, false);
    form = section(QStringLiteral("Положение"), false);
    m_width = new QSpinBox(this); m_width->setRange(10, 96); m_width->setValue(84); m_width->setSuffix(QStringLiteral(" %"));
    m_width->setObjectName(QStringLiteral("studioTextWidth"));
    form->addRow(QStringLiteral("Ширина текста"), m_width);
    m_alignment = new QComboBox(this);
    m_alignment->addItem(QStringLiteral("Слева"), 0);
    m_alignment->addItem(QStringLiteral("По центру"), 1);
    m_alignment->addItem(QStringLiteral("Справа"), 2);
    m_alignment->setCurrentIndex(1);
    m_alignment->setObjectName(QStringLiteral("studioTextAlignment"));
    form->addRow(QStringLiteral("Выравнивание"), m_alignment);
    m_letterSpacing = new QSpinBox(this); m_letterSpacing->setRange(-10, 40); m_letterSpacing->setSuffix(QStringLiteral(" px"));
    m_letterSpacing->setObjectName(QStringLiteral("studioTextLetterSpacing"));
    form->addRow(QStringLiteral("Межбуквенный интервал"), m_letterSpacing);
    m_wordSpacing = new QSpinBox(this); m_wordSpacing->setRange(0, 80); m_wordSpacing->setSuffix(QStringLiteral(" px"));
    m_wordSpacing->setObjectName(QStringLiteral("studioTextWordSpacing"));
    form->addRow(QStringLiteral("Межсловный интервал"), m_wordSpacing);
    m_lineSpacing = new QSpinBox(this); m_lineSpacing->setRange(80, 200); m_lineSpacing->setValue(108); m_lineSpacing->setSuffix(QStringLiteral(" %"));
    m_lineSpacing->setObjectName(QStringLiteral("studioTextLineSpacing"));
    form->addRow(QStringLiteral("Межстрочный интервал"), m_lineSpacing);
    m_x = new QSpinBox(this); m_x->setRange(0, 100); m_x->setValue(50); m_x->setSuffix(QStringLiteral(" %"));
    m_x->setObjectName(QStringLiteral("studioTextX"));
    form->addRow(QStringLiteral("Положение слева"), m_x);
    m_y = new QSpinBox(this); m_y->setRange(0, 100); m_y->setValue(50); m_y->setSuffix(QStringLiteral(" %"));
    m_y->setObjectName(QStringLiteral("studioTextY"));
    form->addRow(QStringLiteral("Положение сверху"), m_y);
    m_duration = new QDoubleSpinBox(this); m_duration->setRange(.1, 120); m_duration->setValue(4); m_duration->setSuffix(QStringLiteral(" с"));
    form->addRow(QStringLiteral("Длина надписи"), m_duration);
    auto animationForm = section(QStringLiteral("Анимация"), false);
    m_inDuration = new QDoubleSpinBox(this); m_inDuration->setRange(0, 6); m_inDuration->setValue(.7); m_inDuration->setSuffix(QStringLiteral(" с"));
    animationForm->addRow(QStringLiteral("Появление"), m_inDuration);
    m_outDuration = new QDoubleSpinBox(this); m_outDuration->setRange(0, 6); m_outDuration->setValue(.7); m_outDuration->setSuffix(QStringLiteral(" с"));
    animationForm->addRow(QStringLiteral("Исчезновение"), m_outDuration);
    m_entrance = new QComboBox(this); m_entrance->setEditable(true); m_entrance->setInsertPolicy(QComboBox::NoInsert);
    m_entrance->setObjectName(QStringLiteral("studioTextEntrance"));
    m_entrance->setAccessibleName(QStringLiteral("Вариант появления"));
    m_entrance->addItem(QStringLiteral("Без появления"), 0);
    animationForm->addRow(QStringLiteral("Вариант появления"), m_entrance);
    m_life = new QComboBox(this); m_life->setEditable(true); m_life->setInsertPolicy(QComboBox::NoInsert);
    m_life->setObjectName(QStringLiteral("studioTextLife"));
    m_life->setAccessibleName(QStringLiteral("Движение после появления"));
    m_life->addItem(QStringLiteral("Авто · по появлению"), 0);
    m_life->addItem(QStringLiteral("Без движения"), -1);
    animationForm->addRow(QStringLiteral("Движение"), m_life);
    m_group = new QComboBox(this);
    m_group->setObjectName(QStringLiteral("studioTextGroup"));
    m_group->addItem(QStringLiteral("Буквы"), 0);
    m_group->addItem(QStringLiteral("Слова"), 1);
    m_group->addItem(QStringLiteral("Строки"), 2);
    m_group->addItem(QStringLiteral("Вся фраза"), 3);
    m_group->setCurrentIndex(3);
    animationForm->addRow(QStringLiteral("Движутся"), m_group);
    m_order = new QComboBox(this);
    m_order->setObjectName(QStringLiteral("studioTextOrder"));
    for (const QString &name : {QStringLiteral("По порядку"), QStringLiteral("В обратном порядке"),
                                QStringLiteral("От центра"), QStringLiteral("К центру"),
                                QStringLiteral("Случайно"), QStringLiteral("Через один")})
        m_order->addItem(name);
    animationForm->addRow(QStringLiteral("Очередность"), m_order);
    m_lag = new QSpinBox(this); m_lag->setRange(0, 90); m_lag->setSuffix(QStringLiteral(" %"));
    m_lag->setObjectName(QStringLiteral("studioTextLag"));
    animationForm->addRow(QStringLiteral("Задержка"), m_lag);
    m_motionAmount = new QSpinBox(this); m_motionAmount->setRange(0, 100); m_motionAmount->setValue(35);
    m_motionAmount->setSuffix(QStringLiteral(" %"));
    m_motionAmount->setObjectName(QStringLiteral("studioTextMotionAmount"));
    animationForm->addRow(QStringLiteral("Сила движения"), m_motionAmount);
    m_motionSpeed = new QDoubleSpinBox(this); m_motionSpeed->setRange(.2, 2); m_motionSpeed->setSingleStep(.1);
    m_motionSpeed->setValue(1); m_motionSpeed->setSuffix(QStringLiteral("×"));
    m_motionSpeed->setObjectName(QStringLiteral("studioTextMotionSpeed"));
    animationForm->addRow(QStringLiteral("Темп движения"), m_motionSpeed);
    m_exit = new QComboBox(this); m_exit->setEditable(true); m_exit->setInsertPolicy(QComboBox::NoInsert);
    m_exit->setObjectName(QStringLiteral("studioTextExit"));
    m_exit->setAccessibleName(QStringLiteral("Вариант исчезновения"));
    m_exit->addItem(QStringLiteral("Обратное появление"), -1);
    m_exit->addItem(QStringLiteral("Без исчезновения"), 0);
    animationForm->addRow(QStringLiteral("Вариант исчезновения"), m_exit);
    QFile catalog(StudioResources::dataFile(QStringLiteral("studio-text/catalog.json")));
    if (catalog.open(QIODevice::ReadOnly)) m_catalog = QJsonDocument::fromJson(catalog.readAll()).array();
    for (const auto &item : m_catalog) {
        const auto recipe = item.toObject();
        const int id = recipe.value(QStringLiteral("id")).toInt();
        const QString name = recipe.value(QStringLiteral("name")).toString();
        if (id < 1 || id > 100 || name.isEmpty()) continue;
        m_entrance->addItem(name, id);
        m_life->addItem(recipe.value(QStringLiteral("life")).toObject().value(QStringLiteral("name")).toString(name), id);
        m_exit->addItem(name, id);
    }
    for (auto combo : {m_entrance, m_life, m_exit}) {
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        combo->setMinimumContentsLength(10);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->completer()->setCaseSensitivity(Qt::CaseInsensitive);
        combo->completer()->setFilterMode(Qt::MatchContains);
    }
    if (m_entrance->count() != 101) {
        m_entrance->addItem(QStringLiteral("Каталог анимаций отсутствует"));
        for (auto combo : {m_entrance, m_life, m_exit}) combo->setEnabled(false);
        m_style->setEnabled(false);
        m_status->setText(QStringLiteral("В этой сборке не найден каталог анимаций надписей. Переустановите сборку монтажной студии."));
    }
    m_entrance->setCurrentIndex(m_entrance->findData(2));
    m_life->setCurrentIndex(0);
    m_exit->setCurrentIndex(0);
    connect(m_style, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        const int id = m_style->currentData().toInt();
        if (id == 0) return;
        QJsonObject values = settings();
        if (id < 0) {
            values.insert(QStringLiteral("styleId"), -1);
            values.insert(QStringLiteral("inPreset"), 0);
            values.insert(QStringLiteral("outPreset"), 0);
            values.insert(QStringLiteral("lifeSource"), 72);
            values.insert(QStringLiteral("lifeAmount"), .8);
            values.insert(QStringLiteral("group"), 0);
        } else values = SunimoTextQt::applyStyle(values, SunimoTextQt::styles[size_t(id - 1)]);
        loadSettings(values);
    });
    m_apply = new QPushButton(QStringLiteral("Добавить надпись"), this);
    m_apply->setObjectName(QStringLiteral("studioTextApply"));
    m_apply->setProperty("studioPrimary", true);
    layout->addWidget(m_apply);
    connect(m_text, &QPlainTextEdit::textChanged, this, [this] {
        updateApplyState();
        if (!m_sceneError.isEmpty()) { m_status->setText(m_sceneError); return; }
        if (m_text->toPlainText().trimmed().isEmpty()) m_status->setText(QStringLiteral("Введите текст надписи."));
        else if (m_status->text() == QLatin1String("Введите текст надписи.")) m_status->clear();
        queuePreview();
    });
    for (auto spin : findChildren<QSpinBox *>())
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, &StudioTextPage::queuePreview);
    for (auto spin : findChildren<QDoubleSpinBox *>())
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &StudioTextPage::queuePreview);
    for (auto combo : findChildren<QComboBox *>())
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &StudioTextPage::queuePreview);
    connect(m_font, &QFontComboBox::currentFontChanged, this, &StudioTextPage::queuePreview);
    for (auto check : findChildren<QCheckBox *>())
        connect(check, &QCheckBox::toggled, this, &StudioTextPage::queuePreview);
    for (auto color : {m_color, m_outlineColor, m_shadowColorButton, m_backgroundColorButton})
        connect(color, &QPushButton::clicked, this, &StudioTextPage::queuePreview);
    connect(m_apply, &QPushButton::clicked, this, &StudioTextPage::apply);
    refreshSelection();
}

StudioTextPage::~StudioTextPage()
{
    stopPreview();
    if (m_native && m_destroy) m_destroy(m_native);
    if (m_drafts && !m_targetKey.isEmpty() && m_sceneError.isEmpty()) m_drafts->insert(m_targetKey, settings());
}

void StudioTextPage::deactivate() { stopPreview(); }

void StudioTextPage::loadCustomStyles()
{
    m_customStyles = QJsonDocument::fromJson(QSettings().value(QStringLiteral("StudioText/customStyles")).toByteArray()).array();
    m_customList->clear();
    for (const auto &style : m_customStyles) {
        const QString name = style.toObject().value(QStringLiteral("name")).toString();
        if (!name.isEmpty()) m_customList->addItem(name);
    }
}

void StudioTextPage::saveCustomStyles()
{
    QSettings().setValue(QStringLiteral("StudioText/customStyles"), QJsonDocument(m_customStyles).toJson(QJsonDocument::Compact));
    loadCustomStyles();
}

QByteArray StudioTextPage::compiledDraft(const QJsonObject &values, QSize size)
{
    if (!m_cachedScene.isEmpty() && m_cachedSettings == values && m_cachedSize == size) return m_cachedScene;
    const QJsonObject raster = SunimoTextQt::rasterSettings(values);
    if (m_cachedScene.isEmpty() || m_cachedRaster != raster || m_cachedSize != size)
        m_cachedScene = SunimoTextQt::compile(values, size);
    else m_cachedScene = SunimoTextQt::retime(m_cachedScene, values);
    m_cachedRaster = raster;
    m_cachedSettings = values;
    m_cachedSize = size;
    return m_cachedScene;
}

void StudioTextPage::stopPreview()
{
    m_previewTimer->stop();
    m_previewUpdate->stop();
    m_play->setText(QStringLiteral("Играть"));
}

void StudioTextPage::queuePreview()
{
    if (!isVisible()) return;
    m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
    m_preview->setText(QStringLiteral("Обновление предпросмотра…"));
    m_previewUpdate->start();
}

void StudioTextPage::renderPreview()
{
    if (!isVisible()) { stopPreview(); return; }
    if (!m_sceneError.isEmpty()) {
        stopPreview(); m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
        m_preview->setText(m_sceneError); return;
    }
    if (m_text->toPlainText().trimmed().isEmpty()) {
        m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
        m_preview->setText(QStringLiteral("Введите текст для предпросмотра")); return;
    }
    const auto &profile = pCore->getProjectProfile();
    const QSize reference(profile.width(), profile.height());
    QByteArray bytes;
    try { bytes = compiledDraft(settings(), reference); }
    catch (const std::exception &error) {
        m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
        m_preview->setText(QString::fromUtf8(error.what())); return;
    }
    if (!m_nativeChecked) {
        m_nativeChecked = true;
        QStringList paths;
        const QString override = qEnvironmentVariable("STUDIO_TEXT_LIBRARY");
        if (!override.isEmpty()) paths << override;
        for (const auto &prefix : StudioResources::prefixes())
            paths << QDir(prefix).filePath(QStringLiteral("lib/frei0r-1/sunimo_text_studio.so"));
        paths << QStringLiteral("/app/lib/frei0r-1/sunimo_text_studio.so");
        for (const auto &path : paths) {
            if (!QFileInfo(path).isFile()) continue;
            m_nativeLibrary.setFileName(path);
            if (!m_nativeLibrary.load()) continue;
            auto create = reinterpret_cast<void *(*)()>(m_nativeLibrary.resolve("smt_create"));
            m_destroy = reinterpret_cast<void (*)(void *)>(m_nativeLibrary.resolve("smt_destroy"));
            m_load = reinterpret_cast<int (*)(void *, const void *, size_t)>(m_nativeLibrary.resolve("smt_load"));
            m_render = reinterpret_cast<int (*)(void *, double, unsigned, unsigned, const void *, void *)>(m_nativeLibrary.resolve("smt_render"));
            m_lastError = reinterpret_cast<const char *(*)(void *)>(m_nativeLibrary.resolve("smt_last_error"));
            if (create && m_destroy && m_load && m_render && m_lastError) m_native = create();
            if (m_native) break;
            m_nativeLibrary.unload();
        }
    }
    if (!m_native) {
        m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
        m_preview->setText(QStringLiteral("Движок предпросмотра надписей не найден.")); return;
    }
    if (m_loadedScene != bytes) {
        if (!m_load(m_native, bytes.constData(), size_t(bytes.size()))) {
            m_loadedScene.clear(); m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
            m_preview->setText(QString::fromUtf8(m_lastError(m_native))); return;
        }
        m_loadedScene = bytes;
    }
    const QSize output(qMax(64, reference.width() / 2), qMax(64, reference.height() / 2));
    QImage frame(output, QImage::Format_RGBA8888);
    const double seconds = m_seek->value() * m_duration->value() / 1000.;
    if (frame.isNull() || !m_render(m_native, seconds, unsigned(output.width()), unsigned(output.height()), nullptr, frame.bits())) {
        m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
        m_preview->setText(QString::fromUtf8(m_lastError(m_native))); return;
    }
    m_previewImage = frame;
    m_preview->setProperty("studioRenderedFrames", ++m_renderCount);
    displayPreview();
}

void StudioTextPage::displayPreview()
{
    if (m_previewImage.isNull()) return;
    QImage composite(m_previewImage.size(), QImage::Format_RGB32);
    composite.fill(QColor(QStringLiteral("#182638")));
    QPainter painter(&composite);
    painter.drawImage(0, 0, m_previewImage);
    m_preview->setPixmap(QPixmap::fromImage(composite).scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

bool StudioTextPage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_preview && event->type() == QEvent::Resize) displayPreview();
    return QWidget::eventFilter(watched, event);
}

void StudioTextPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event); queuePreview();
}

void StudioTextPage::hideEvent(QHideEvent *event)
{
    stopPreview(); QWidget::hideEvent(event);
}

QJsonObject StudioTextPage::settings() const
{
    QJsonObject result{{QStringLiteral("text"), m_text->toPlainText()}, {QStringLiteral("size"), m_size->value()},
                       {QStringLiteral("font"), m_font->currentFont().family()}, {QStringLiteral("bold"), m_bold->isChecked()},
                       {QStringLiteral("italic"), m_italic->isChecked()},
                       {QStringLiteral("color"), m_textColor.name(QColor::HexArgb)},
                       {QStringLiteral("outlineEnabled"), m_outlineEnabled->isChecked()},
                       {QStringLiteral("outlineWidth"), m_outlineWidth->value()},
                       {QStringLiteral("outlineColor"), m_outlineColorValue.name(QColor::HexArgb)},
                       {QStringLiteral("opacity"), m_opacity->value()},
                       {QStringLiteral("shadow"), m_shadow->value()},
                       {QStringLiteral("shadowColor"), m_shadowColorValue.name(QColor::HexArgb)},
                       {QStringLiteral("backgroundEnabled"), m_backgroundEnabled->isChecked()},
                       {QStringLiteral("backgroundColor"), m_backgroundColorValue.name(QColor::HexArgb)},
                       {QStringLiteral("alignment"), m_alignment->currentData().toInt()},
                       {QStringLiteral("letterSpacing"), m_letterSpacing->value()},
                       {QStringLiteral("wordSpacing"), m_wordSpacing->value()},
                       {QStringLiteral("lineSpacing"), m_lineSpacing->value()},
                       {QStringLiteral("width"), m_width->value()}, {QStringLiteral("x"), m_x->value()}, {QStringLiteral("y"), m_y->value()},
                       {QStringLiteral("duration"), m_duration->value()}, {QStringLiteral("inDuration"), m_inDuration->value()},
                       {QStringLiteral("outDuration"), m_outDuration->value()},
                       {QStringLiteral("inPreset"), m_entrance->currentData().toInt()},
                       {QStringLiteral("outPreset"), m_exit->currentData().toInt()},
                       {QStringLiteral("group"), m_group->currentData().toInt()},
                       {QStringLiteral("order"), m_order->currentIndex()},
                       {QStringLiteral("lag"), m_lag->value() / 100.},
                       {QStringLiteral("lifeSpeed"), m_motionSpeed->value()}};
    result.insert(QStringLiteral("styleId"), m_style->currentData().toInt());
    const int life = m_life->currentData().toInt();
    result.insert(QStringLiteral("lifeSource"), life < 0 ? 0 : life);
    result.insert(QStringLiteral("lifeAmount"), life < 0 ? 0 : m_motionAmount->value() / 100.);
    const auto &profile = pCore->getProjectProfile();
    result.insert(QStringLiteral("fpsNum"), profile.frame_rate_num());
    result.insert(QStringLiteral("fpsDen"), profile.frame_rate_den());
    return result;
}

void StudioTextPage::loadSettings(const QJsonObject &data)
{
    m_text->setPlainText(data.value(QStringLiteral("text")).toString());
    m_font->setCurrentFont(QFont(data.value(QStringLiteral("font")).toString(QStringLiteral("sans-serif"))));
    m_bold->setChecked(data.value(QStringLiteral("bold")).toBool(true));
    m_italic->setChecked(data.value(QStringLiteral("italic")).toBool(false));
    const QColor loaded(data.value(QStringLiteral("color")).toString(QStringLiteral("#fff9ef")));
    m_textColor = loaded.isValid() ? loaded : QColor(QStringLiteral("#fff9ef"));
    m_color->setText(QStringLiteral("Выбрать цвет (%1)").arg(m_textColor.name(QColor::HexArgb)));
    m_outlineEnabled->setChecked(data.value(QStringLiteral("outlineEnabled")).toBool(false));
    m_outlineWidth->setValue(data.value(QStringLiteral("outlineWidth")).toInt(3));
    const QColor loadedOutline(data.value(QStringLiteral("outlineColor")).toString(QStringLiteral("#ff000000")));
    m_outlineColorValue = loadedOutline.isValid() ? loadedOutline : QColor(QStringLiteral("#ff000000"));
    m_outlineColor->setText(QStringLiteral("Выбрать цвет (%1)").arg(m_outlineColorValue.name(QColor::HexArgb)));
    m_opacity->setValue(data.value(QStringLiteral("opacity")).toInt(100));
    m_shadow->setValue(data.value(QStringLiteral("shadow")).toInt(0));
    const QColor loadedShadow(data.value(QStringLiteral("shadowColor")).toString(QStringLiteral("#80000000")));
    m_shadowColorValue = loadedShadow.isValid() ? loadedShadow : QColor(QStringLiteral("#80000000"));
    m_shadowColorButton->setText(QStringLiteral("Выбрать цвет (%1)").arg(m_shadowColorValue.name(QColor::HexArgb)));
    m_backgroundEnabled->setChecked(data.value(QStringLiteral("backgroundEnabled")).toBool(false));
    const QColor loadedBackground(data.value(QStringLiteral("backgroundColor")).toString(QStringLiteral("#66000000")));
    m_backgroundColorValue = loadedBackground.isValid() ? loadedBackground : QColor(QStringLiteral("#66000000"));
    m_backgroundColorButton->setText(QStringLiteral("Выбрать цвет (%1)").arg(m_backgroundColorValue.name(QColor::HexArgb)));
    m_alignment->setCurrentIndex(qMax(0, m_alignment->findData(data.value(QStringLiteral("alignment")).toInt(1))));
    m_letterSpacing->setValue(data.value(QStringLiteral("letterSpacing")).toInt(0));
    m_wordSpacing->setValue(data.value(QStringLiteral("wordSpacing")).toInt(0));
    m_lineSpacing->setValue(data.value(QStringLiteral("lineSpacing")).toInt(108));
    m_size->setValue(data.value(QStringLiteral("size")).toInt(64));
    m_width->setValue(data.value(QStringLiteral("width")).toInt(84));
    m_x->setValue(data.value(QStringLiteral("x")).toInt(50));
    m_y->setValue(data.value(QStringLiteral("y")).toInt(50));
    m_duration->setValue(data.value(QStringLiteral("duration")).toDouble(4));
    m_inDuration->setValue(data.value(QStringLiteral("inDuration")).toDouble(.7));
    m_outDuration->setValue(data.value(QStringLiteral("outDuration")).toDouble(.7));
    m_entrance->setCurrentIndex(qMax(0, m_entrance->findData(data.value(QStringLiteral("inPreset")).toInt(2))));
    const int life = data.value(QStringLiteral("lifeAmount")).toDouble(.35) == 0
                         && data.value(QStringLiteral("lifeSource")).toInt(0) == 0 ? -1
                         : data.value(QStringLiteral("lifeSource")).toInt(0);
    m_life->setCurrentIndex(qMax(0, m_life->findData(life)));
    m_group->setCurrentIndex(qMax(0, m_group->findData(data.value(QStringLiteral("group")).toInt(3))));
    m_order->setCurrentIndex(qBound(0, data.value(QStringLiteral("order")).toInt(0), 5));
    m_lag->setValue(qRound(data.value(QStringLiteral("lag")).toDouble(0) * 100));
    m_motionAmount->setValue(qRound(data.value(QStringLiteral("lifeAmount")).toDouble(.35) * 100));
    m_motionSpeed->setValue(data.value(QStringLiteral("lifeSpeed")).toDouble(1));
    m_exit->setCurrentIndex(qMax(0, m_exit->findData(data.value(QStringLiteral("outPreset")).toInt(-1))));
    const QSignalBlocker blocker(m_style);
    m_style->setCurrentIndex(qMax(0, m_style->findData(data.value(QStringLiteral("styleId")).toInt(0))));
    updateApplyState();
}

QString StudioTextPage::selectionKey(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, const QString &scene) const
{
    if (!timeline || clipId < 0 || !pCore->currentDoc()) return {};
    return QStringLiteral("%1|%2|%3|%4")
        .arg(pCore->currentDoc()->url().toString(QUrl::FullyEncoded), timeline->uuid().toString(QUuid::WithoutBraces))
        .arg(clipId).arg(scene);
}

void StudioTextPage::updateApplyState()
{
    const QString text = m_text->toPlainText();
    m_apply->setEnabled(m_sceneError.isEmpty() && m_selectedClip >= 0 && m_entrance->count() == 101 && !text.trimmed().isEmpty() && text.size() <= 8192);
}

void StudioTextPage::refreshSelection()
{
    const auto [timeline, clipId] = selectedVideo();
    auto effects = clipId >= 0 ? StudioHelpers::effectsById(timeline->getClipEffectStack(clipId), QString::fromLatin1(textEffectId))
                               : QList<std::shared_ptr<EffectItemModel>>{};
    const QString path = effects.isEmpty() ? QString() : effects.front()->getParam(QStringLiteral("0"));
    const QString key = selectionKey(timeline, clipId, path);
    const QString readError = sceneReadError(path);
    const bool changed = key != m_targetKey || readError != m_sceneError;
    if (changed) {
        stopPreview();
        m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
        m_preview->setText(QStringLiteral("Обновление предпросмотра…"));
        if (m_drafts && !m_targetKey.isEmpty() && m_sceneError.isEmpty()) m_drafts->insert(m_targetKey, settings());
        if (!m_sceneError.isEmpty()) m_status->clear();
        m_sceneError = readError;
        m_selectedClip = clipId;
        m_scene = path;
        m_targetKey = key;
        if (m_drafts && m_drafts->contains(key)) loadSettings(m_drafts->value(key));
        else loadSettings(path.isEmpty() ? QJsonObject{} : SunimoTextQt::metadata(path));
    }
    auto binClip = clipId >= 0 ? pCore->projectItemModel()->getClipByBinID(timeline->getClipBinId(clipId)) : nullptr;
    const int clipFrames = binClip && binClip->clipType() == ClipType::Text ? timeline->getClipPlaytime(clipId) : -1;
    if (clipFrames > 0 && (changed || clipFrames != m_clipFrames))
        m_duration->setValue(clipFrames / pCore->getCurrentFps());
    m_clipFrames = clipFrames;
    m_target->setText(!m_sceneError.isEmpty() ? m_sceneError
                    : clipId < 0 ? QStringLiteral("Выберите один видеоклип для размещения надписи.")
                    : path.isEmpty() ? QStringLiteral("Надпись появится отдельным клипом над выбранным видео.")
                                     : QStringLiteral("Выбрана надпись: можно изменить текст и движение."));
    m_apply->setText(path.isEmpty() ? QStringLiteral("Добавить надпись") : QStringLiteral("Обновить надпись"));
    updateApplyState();
    if (!m_sceneError.isEmpty()) m_status->setText(m_sceneError);
}

void StudioTextPage::apply()
{
    if (!m_sceneError.isEmpty()) { m_status->setText(m_sceneError); return; }
    const QString text = m_text->toPlainText();
    if (text.trimmed().isEmpty() || text.size() > 8192) {
        m_status->setText(text.trimmed().isEmpty() ? QStringLiteral("Введите текст надписи.")
                                                : QStringLiteral("Надпись слишком длинная: максимум 8192 символа."));
        m_text->setFocus();
        return;
    }
    if (!m_entrance->currentData().isValid() || !m_life->currentData().isValid() || !m_exit->currentData().isValid()) {
        m_status->setText(QStringLiteral("Выберите вариант из списка после поиска.")); return;
    }
    const auto [timeline, clipId] = selectedVideo();
    if (!timeline || clipId < 0 || !pCore->currentDoc() || pCore->currentDoc()->url().isEmpty()) {
        m_status->setText(QStringLiteral("Выберите один видеоклип и сохраните проект.")); return;
    }
    const auto effects = StudioHelpers::effectsById(timeline->getClipEffectStack(clipId), QString::fromLatin1(textEffectId));
    if (effects.size() > 1) { m_status->setText(QStringLiteral("На клипе несколько надписей: оставьте одну для правки.")); return; }
    const QString readError = sceneReadError(effects.isEmpty() ? QString() : effects.front()->getParam(QStringLiteral("0")));
    if (!readError.isEmpty()) {
        if (m_drafts && !m_targetKey.isEmpty()) m_drafts->insert(m_targetKey, settings());
        m_sceneError = readError; stopPreview(); updateApplyState(); renderPreview(); m_status->setText(readError); return;
    }
    QJsonObject values = settings();
    const int frames = qMax(1, qRound(m_duration->value() * pCore->getCurrentFps()));
    values.insert(QStringLiteral("duration"), frames / pCore->getCurrentFps());
    QByteArray bytes;
    try {
        const auto &profile = pCore->getProjectProfile();
        bytes = compiledDraft(values, {profile.width(), profile.height()});
    } catch (const std::exception &error) { m_status->setText(QString::fromUtf8(error.what())); return; }
    const QString folder = QDir(pCore->currentDoc()->projectDataFolder()).absoluteFilePath(QStringLiteral("studio-text"));
    if (!QDir().mkpath(folder)) { m_status->setText(QStringLiteral("Не удалось создать каталог надписей проекта.")); return; }
    const QString scene = QDir(folder).absoluteFilePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".stxt"));
    QString error;
    if (!SunimoTextQt::save(scene, bytes, &error)) { m_status->setText(error); return; }
    if (!effects.isEmpty()) {
        if (!StudioText::updateTitle(timeline, clipId, effects.front(), frames, scene)) {
            QFile::remove(scene); m_status->setText(QStringLiteral("Не удалось обновить надпись.")); return;
        }
        if (m_drafts) m_drafts->remove(m_targetKey);
        m_scene = scene;
        m_targetKey = selectionKey(timeline, clipId, scene);
        if (m_drafts && !m_targetKey.isEmpty()) m_drafts->insert(m_targetKey, settings());
        m_status->setText(QStringLiteral("Надпись обновлена. Изменение можно отменить."));
        return;
    }
    QPointer<StudioTextPage> page(this);
    const bool added = StudioText::createTitle(timeline, clipId, frames,
        QStringLiteral("Надпись: %1").arg(m_text->toPlainText().simplified().left(30)), scene,
        [page](bool success, const QString &message) {
            if (!page) return;
            page->m_status->setText(message);
            if (success) {
                if (page->m_drafts) page->m_drafts->remove(page->m_targetKey);
                page->m_targetKey.clear();
                page->refreshSelection();
            }
        });
    if (!added) { QFile::remove(scene); m_status->setText(QStringLiteral("Не удалось добавить надпись в проект.")); }
    else m_status->setText(QStringLiteral("Добавление надписи…"));
}
