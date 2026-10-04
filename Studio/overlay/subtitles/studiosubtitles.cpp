// SPDX-License-Identifier: GPL-3.0-only
#include "../core/studioresources.hpp"
#include "../audio/studiomanagedaudio.hpp"
#include "studiosubtitles.hpp"

#include "bin/model/subtitlemodel.hpp"
#include "core.h"
#include "definitions.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "mainwindow.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelinemodel.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/view/timelinecontroller.h"
#include "timeline2/view/timelinewidget.h"

#include <QCryptographicHash>
#include <QColorDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QReadWriteLock>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSet>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

#include <mlt++/MltConsumer.h>
#include <mlt++/MltFilter.h>
#include <mlt++/MltFrame.h>
#include <mlt++/MltPlaylist.h>
#include <mlt++/MltProducer.h>
#include <mlt++/MltTractor.h>

namespace {
QStringList subtitleStyleArgs(const QJsonObject &values, int width)
{
    const QStringList animations{QStringLiteral("none"), QStringLiteral("words"), QStringLiteral("letters"), QStringLiteral("pop"),
                                 QStringLiteral("fade"), QStringLiteral("rise"), QStringLiteral("slide"),
                                 QStringLiteral("reveal_words"), QStringLiteral("accent")};
    const int animation = qBound(0, values.value(QStringLiteral("animation")).toInt(), int(animations.size()) - 1);
    return {QStringLiteral("--max-width"), QString::number(width),
            QStringLiteral("--font"), values.value(QStringLiteral("font")).toString(QStringLiteral("DejaVu Sans")),
            QStringLiteral("--font-size"), QString::number(values.value(QStringLiteral("font_size")).toInt(64)),
            QStringLiteral("--bold"), QString::number(values.value(QStringLiteral("bold")).toInt(1)),
            QStringLiteral("--text-color"), values.value(QStringLiteral("text_color")).toString(QStringLiteral("#FFFFFFFF")),
            QStringLiteral("--outline-color"), values.value(QStringLiteral("outline_color")).toString(QStringLiteral("#FF000000")),
            QStringLiteral("--background-color"), values.value(QStringLiteral("background_color")).toString(QStringLiteral("#AA000000")),
            QStringLiteral("--outline"), QString::number(values.value(QStringLiteral("outline")).toInt(3)),
            QStringLiteral("--shadow"), QString::number(values.value(QStringLiteral("shadow")).toInt(0)),
            QStringLiteral("--position"), QString::number(values.value(QStringLiteral("position")).toInt(0)),
            QStringLiteral("--background"), QString::number(values.value(QStringLiteral("background")).toInt(1)),
            QStringLiteral("--safe-margin"), QString::number(values.value(QStringLiteral("safe_margin")).toInt(8)),
            QStringLiteral("--animation"), animations.at(animation),
            QStringLiteral("--min-duration"), QString::number(values.value(QStringLiteral("min_duration")).toInt(650)),
            QStringLiteral("--max-duration"), QString::number(values.value(QStringLiteral("max_duration")).toInt(3200)),
            QStringLiteral("--max-reading-cps"), QString::number(values.value(QStringLiteral("max_reading_cps")).toInt(24))};
}

QString subtitlePlainText(QString text)
{
    text.remove(QRegularExpression(QStringLiteral(R"(\{\\[^}]*\})")));
    text.replace(QStringLiteral("\\N"), QStringLiteral(" "));
    text.replace(QStringLiteral("\\n"), QStringLiteral(" "));
    text.replace(QStringLiteral("\\h"), QStringLiteral(" "));
    return text;
}

QJsonArray savedWordMarks(const QString &effect, const QString &text, qint64 startMs, qint64 endMs,
                          double fps, QString *blockId, bool *stale)
{
    *stale = false;
    const auto fields = effect.split(QLatin1Char(':'));
    if (fields.size() != 6 || fields[0] != QLatin1String("SUNIMO1")) {
        *stale = effect.startsWith(QLatin1String("SUNIMO1:"));
        return {};
    }
    bool ok = false;
    fields[1].toULongLong(&ok);
    if (ok) *blockId = fields[1];
    const qint64 oldStart = fields[2].toLongLong(&ok);
    if (!ok) { *stale = true; return {}; }
    const qint64 oldEnd = fields[3].toLongLong(&ok);
    if (!ok || oldEnd <= oldStart || qAbs((oldEnd - oldStart) - (endMs - startMs)) > qCeil(1000. / fps) + 10) {
        *stale = true; return {};
    }
    const QByteArray hex = fields[4].toLatin1();
    if (hex.size() % 2 || !QRegularExpression(QStringLiteral("^[0-9a-fA-F]*$")).match(fields[4]).hasMatch()
        || QByteArray::fromHex(hex) != text.toUtf8()) { *stale = true; return {}; }
    if (fields[5].isEmpty()) return {};
    const auto tokens = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const auto marks = fields[5].split(QLatin1Char(';'));
    if (tokens.size() != marks.size() || marks.isEmpty()) { *stale = true; return {}; }
    QJsonArray words;
    for (int i = 0; i < marks.size(); ++i) {
        const auto pair = marks[i].split(QLatin1Char('-'));
        if (pair.size() != 2) { *stale = true; return {}; }
        const qint64 first = pair[0].toLongLong(&ok);
        if (!ok) { *stale = true; return {}; }
        const qint64 last = pair[1].toLongLong(&ok);
        if (!ok) { *stale = true; return {}; }
        const qint64 shiftedStart = qMax(startMs, startMs + first - oldStart);
        const qint64 shiftedEnd = qMin(endMs, startMs + last - oldStart);
        if (shiftedStart >= shiftedEnd || shiftedStart >= endMs) { *stale = true; return {}; }
        words.append(QJsonObject{{QStringLiteral("start_ms"), shiftedStart}, {QStringLiteral("end_ms"), shiftedEnd},
                                 {QStringLiteral("text"), tokens[i]}});
    }
    return words;
}

bool isolateManagedSubtitleStyles(QString &ass, const SubtitleModel &subtitles, QString *error)
{
    auto lines = ass.split(QLatin1Char('\n'));
    QHash<QString, QString> definitions;
    QString base;
    const auto malformed = [error] {
        *error = QStringLiteral("Обработчик вернул повреждённые стили субтитров. Проект не изменён.");
        return false;
    };
    for (const auto &line : lines) {
        if (!line.startsWith(QLatin1String("Style: "))) continue;
        const auto fields = line.mid(7).split(QLatin1Char(','));
        if (fields.size() != 23 || !fields[0].startsWith(QLatin1String("Studio-"))) return malformed();
        definitions.insert(fields[0], SubtitleStyle(line).toString(QString()));
        if (!fields[0].endsWith(QLatin1String("-Box"))) {
            if (!base.isEmpty()) return malformed();
            base = fields[0];
        }
    }
    if (base.isEmpty() || definitions.size() != (definitions.contains(base + QStringLiteral("-Box")) ? 2 : 1)) return malformed();
    const auto &existing = subtitles.getAllSubtitleStyles();
    const auto conflicts = [&](const QString &candidate) {
        for (auto it = definitions.cbegin(); it != definitions.cend(); ++it) {
            const QString name = candidate + (it.key() == base ? QString() : QStringLiteral("-Box"));
            const auto found = existing.find(name);
            if (found != existing.end() && found->second.toString(QString()) != it.value()) return true;
        }
        return false;
    };
    QString candidate = base;
    for (int suffix = 1; conflicts(candidate); ++suffix) candidate = base + QLatin1Char('-') + QString::number(suffix);
    if (candidate == base) return true;
    for (auto &line : lines) {
        if (line.startsWith(QLatin1String("Style: "))) {
            const QString name = line.mid(7).section(QLatin1Char(','), 0, 0);
            line.replace(7, name.size(), candidate + (name == base ? QString() : QStringLiteral("-Box")));
        } else if (line.startsWith(QLatin1String("Dialogue:"))) {
            auto fields = line.split(QLatin1Char(','));
            if (fields.size() < 10 || !definitions.contains(fields[3])) return malformed();
            fields[3] = candidate + (fields[3] == base ? QString() : QStringLiteral("-Box"));
            line = fields.join(QLatin1Char(','));
        }
    }
    ass = lines.join(QLatin1Char('\n'));
    return true;
}

QJsonObject restyleInput(const std::shared_ptr<TimelineItemModel> &timeline, bool wholeTrack,
                         int *withoutMarks, int *staleMarks, QString *error)
{
    const auto subtitles = timeline ? timeline->getSubtitleModel() : nullptr;
    if (!subtitles || subtitles->getAllSubIds().empty()) {
        *error = QStringLiteral("На активной дорожке нет субтитров."); return {};
    }
    const auto selected = timeline->getCurrentSelection();
    const auto all = subtitles->getAllSubIds();
    std::vector<int> ids(all.begin(), all.end());
    const double fps = pCore->getCurrentFps();
    std::sort(ids.begin(), ids.end(), [&subtitles](int a, int b) {
        return subtitles->getStartPosForId(a) < subtitles->getStartPosForId(b);
    });
    QSet<int> used;
    QJsonArray blocks;
    *withoutMarks = *staleMarks = 0;
    for (int id : ids) {
        if (used.contains(id) || (!wholeTrack && !selected.count(id))) continue;
        const int layer = subtitles->getLayerForId(id);
        const auto range = subtitles->getInOut(id);
        const int start = range.first;
        const int end = range.second;
        const QString style = subtitles->getStyleName(id);
        int mainId = id, boxId = -1;
        if (layer == 0 && style.endsWith(QLatin1String("-Box"))) {
            mainId = subtitles->getIdForStartPos(1, GenTime(start, fps));
            boxId = id;
        } else if (layer == 1) {
            boxId = subtitles->getIdForStartPos(0, GenTime(start, fps));
        } else if (layer != 0) {
            *error = QStringLiteral("Переоформление поддерживает только два слоя субтитров."); return {};
        }
        if (boxId >= 0) {
            if (mainId < 0 || mainId == boxId || subtitles->getStyleName(boxId) != subtitles->getStyleName(mainId) + QStringLiteral("-Box")
                || subtitles->getInOut(boxId) != subtitles->getInOut(mainId)) {
                *error = QStringLiteral("У субтитра два разных слоя. Исправь пару в редакторе перед переоформлением."); return {};
            }
        } else if (subtitles->getIdForStartPos(1, GenTime(start, fps)) >= 0) {
            *error = QStringLiteral("На том же кадре есть другой слой субтитров. Исправь наложение в редакторе."); return {};
        }
        QString text = subtitlePlainText(subtitles->getText(mainId));
        if (boxId >= 0 && subtitlePlainText(subtitles->getText(boxId)) != text) {
            const QString effect = subtitles->getEffects(mainId);
            const auto fields = effect.split(QLatin1Char(':'));
            const QByteArray hex = fields.size() == 6 ? fields[4].toLatin1() : QByteArray();
            const QByteArray original = QByteArray::fromHex(hex);
            const QString boxText = subtitlePlainText(subtitles->getText(boxId));
            const bool managed = fields.size() == 6 && fields[0] == QLatin1String("SUNIMO1")
                && subtitles->getEffects(boxId) == effect && !original.isEmpty() && original.toHex() == hex.toLower();
            if (!managed || (text.toUtf8() != original && boxText.toUtf8() != original)) {
                *error = QStringLiteral("Текст двух слоёв различается. Исправь его в редакторе перед переоформлением."); return {};
            }
            if (text.toUtf8() == original) text = boxText;
        }
        if (text.isEmpty() || text.toUtf8().size() > 8192 || start >= end) {
            *error = QStringLiteral("Пустой текст или неверные границы субтитра. Проект не изменён."); return {};
        }
        const qint64 startMs = qRound64(start * 1000. / fps);
        const qint64 endMs = qRound64(end * 1000. / fps);
        QString blockId = QString::number(mainId);
        bool stale = false;
        const auto words = savedWordMarks(subtitles->getEffects(mainId), text, startMs, endMs, fps, &blockId, &stale);
        if (words.isEmpty()) ++*withoutMarks;
        if (stale) ++*staleMarks;
        QJsonArray sourceIds{mainId};
        used.insert(mainId);
        if (boxId >= 0) { sourceIds.append(boxId); used.insert(boxId); }
        blocks.append(QJsonObject{{QStringLiteral("id"), blockId}, {QStringLiteral("start_ms"), startMs},
                                  {QStringLiteral("end_ms"), endMs}, {QStringLiteral("text"), text},
                                  {QStringLiteral("words"), words}, {QStringLiteral("source_ids"), sourceIds},
                                  {QStringLiteral("start_frame"), start}, {QStringLiteral("end_frame"), end}});
    }
    if (blocks.isEmpty()) {
        *error = QStringLiteral("Выбери субтитры на таймлайне или включи «Вся дорожка»."); return {};
    }
    return QJsonObject{{QStringLiteral("schema"), 3}, {QStringLiteral("blocks"), blocks}};
}
}

StudioSubtitlePage::StudioSubtitlePage(QWidget *parent)
    : QWidget(parent)
    , m_process(new QProcess(this))
    , m_extractTimeout(new QTimer(this))
    , m_previewTimer(new QTimer(this))
    , m_previewUpdate(new QTimer(this))
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_target = new QLabel(this);
    m_target->setWordWrap(true);
    QFont targetFont = m_target->font(); targetFont.setBold(true); m_target->setFont(targetFont);
    layout->addWidget(m_target);
    m_width = new QSpinBox(this);
    setupControls(layout);
    m_main = new QPushButton(QStringLiteral("Создать русские субтитры"), this);
    m_main->setProperty("studioPrimary", true); layout->addWidget(m_main);
    m_restyleAll = new QCheckBox(QStringLiteral("Вся дорожка субтитров"), this);
    m_restyleAll->setObjectName(QStringLiteral("studioSubtitleRestyleAll"));
    layout->addWidget(m_restyleAll);
    m_restyle = new QPushButton(QStringLiteral("Применить оформление"), this);
    m_restyle->setObjectName(QStringLiteral("studioSubtitleRestyle"));
    layout->addWidget(m_restyle);
    m_edit = new QPushButton(QStringLiteral("Редактировать субтитры"), this);
    layout->addWidget(m_edit);
    m_cancel = new QPushButton(QStringLiteral("Отменить анализ"), this); m_cancel->hide(); layout->addWidget(m_cancel);
    m_progress = new QProgressBar(this); m_progress->setRange(0, 100); m_progress->hide(); layout->addWidget(m_progress);
    m_status = new QLabel(QStringLiteral("Субтитры всегда в одну строку. После распознавания проверьте слова и время на дорожке."), this);
    m_status->setObjectName(QStringLiteral("studioSubtitleStatus"));
    m_status->setWordWrap(true); m_status->setTextFormat(Qt::PlainText); layout->addWidget(m_status); layout->addStretch();
    if (!m_schemaReady) {
        m_main->setEnabled(false);
        m_status->setText(QStringLiteral("В этой сборке отсутствуют настройки субтитров. Переустановите сборку монтажной студии."));
    }
    connect(m_main, &QPushButton::clicked, this, &StudioSubtitlePage::start);
    connect(m_restyle, &QPushButton::clicked, this, &StudioSubtitlePage::restyle);
    connect(m_restyleAll, &QCheckBox::toggled, this, &StudioSubtitlePage::refreshSelection);
    connect(m_edit, &QPushButton::clicked, this, &StudioSubtitlePage::openEditor);
    m_previewTimer->setInterval(80);
    connect(m_previewTimer, &QTimer::timeout, this, [this] {
        const int next = m_seek->value() + 20;
        if (next >= 1000) { m_seek->setValue(1000); stopPreview(); }
        else m_seek->setValue(next);
    });
    m_previewUpdate->setSingleShot(true);
    m_previewUpdate->setInterval(120);
    connect(m_previewUpdate, &QTimer::timeout, this, &StudioSubtitlePage::renderPreview);
    connect(m_cancel, &QPushButton::clicked, this, [this] { StudioJobs::cancel(m_process); });
    connect(m_process, &QProcess::started, this, [this] {
        if (m_stage == Stage::Extract) m_extractTimeout->start(120000);
        else if (m_stage == Stage::Restyle) m_extractTimeout->start(30000);
    });
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] { m_output += m_process->readAllStandardOutput(); });
    m_extractTimeout->setSingleShot(true);
    connect(m_extractTimeout, &QTimer::timeout, this, [this] {
        if (m_stage != Stage::Extract && m_stage != Stage::Restyle) return;
        m_workerError = m_stage == Stage::Restyle
            ? QStringLiteral("Оформление не ответило за 30 секунд. Проект не изменён.")
            : QStringLiteral("Подготовка звука не продвигается уже две минуты. Обработка остановлена; проект не изменён.");
        m_process->kill();
    });
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        m_stderr += m_process->readAllStandardError();
        for (;;) {
            const int newline = m_stderr.indexOf('\n');
            const int carriage = m_stderr.indexOf('\r');
            const int end = newline < 0 ? carriage : carriage < 0 ? newline : qMin(newline, carriage);
            if (end < 0) break;
            const QByteArray line = m_stderr.left(end).trimmed();
            m_stderr.remove(0, end + 1);
            if (m_stage == Stage::Extract) {
                const int marker = line.indexOf("percentage:");
                if (marker >= 0) {
                    bool ok = false;
                    const int percent = line.mid(marker + 11).trimmed().toInt(&ok);
                    if (ok && percent >= 0 && percent <= 100) {
                        m_progress->setValue(qMax(m_progress->value(), percent * 15 / 100));
                        m_extractTimeout->start(120000);
                    }
                } else if (!line.isEmpty()) {
                    m_extractLog += line + '\n';
                    if (m_extractLog.size() > 4096) m_extractLog.remove(0, m_extractLog.size() - 4096);
                }
                continue;
            }
            if (line.startsWith("ERROR\t")) {
                m_workerError = QString::fromUtf8(line.mid(6));
                continue;
            }
            if (!line.startsWith("PROGRESS\t")) continue;
            const auto parts = line.split('\t');
            bool ok = false;
            const int percent = parts.size() > 1 ? parts[1].toInt(&ok) : 0;
            if (ok && percent > 5 && percent <= 100) {
                m_progress->setRange(0, 100);
                m_progress->setValue(qMax(m_progress->value(), 15 + percent * 80 / 100));
            }
            if (parts.size() > 2) m_status->setText(QString::fromUtf8(parts[2]));
        }
        if (m_stderr.size() > 8192) m_stderr.clear();
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart || m_stage == Stage::Idle) return;
        m_extractTimeout->stop();
        m_stage = Stage::Idle;
        setBusy(false);
        m_status->setText(QStringLiteral("Не удалось запустить обработку субтитров: %1").arg(m_process->errorString()));
        m_temporary.reset(); m_restyleSnapshot.clear();
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &StudioSubtitlePage::finished);
}

StudioSubtitlePage::~StudioSubtitlePage() { stopPreview(); m_extractTimeout->stop(); StudioJobs::dispose(m_process); }

void StudioSubtitlePage::setupControls(QVBoxLayout *layout)
{
    QFile schemaFile(StudioResources::dataFile(QStringLiteral("studio-subtitles/parameters.json")));
    const auto schema = schemaFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(schemaFile.readAll()).object() : QJsonObject{};
    m_parameters = schema.value(QStringLiteral("parameters")).toArray();
    m_presets = schema.value(QStringLiteral("presets")).toArray();
    m_schemaReady = !m_parameters.isEmpty() && !m_presets.isEmpty();

    m_styleTabs = new QTabBar(this);
    m_styleTabs->addTab(QStringLiteral("Готовые"));
    m_styleTabs->addTab(QStringLiteral("Мои стили"));
    layout->addWidget(m_styleTabs);
    m_preset = new QComboBox(this);
    m_preset->setObjectName(QStringLiteral("studioSubtitlePreset"));
    m_preset->addItem(QStringLiteral("Свои настройки"));
    for (const auto &entry : m_presets) m_preset->addItem(entry.toObject().value(QStringLiteral("label")).toString());
    m_preset->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    layout->addWidget(m_preset);
    auto customRow = new QWidget(this);
    auto customLayout = new QGridLayout(customRow);
    customLayout->setContentsMargins(0, 0, 0, 0);
    m_customList = new QComboBox(this);
    m_customList->setObjectName(QStringLiteral("studioSubtitleCustomStyles"));
    m_customList->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto saveStyle = new QPushButton(QStringLiteral("Сохранить"), this);
    auto deleteStyle = new QPushButton(QStringLiteral("Удалить"), this);
    saveStyle->setObjectName(QStringLiteral("studioSubtitleSaveStyle"));
    deleteStyle->setObjectName(QStringLiteral("studioSubtitleDeleteStyle"));
    customLayout->addWidget(m_customList, 0, 0, 1, 2);
    customLayout->addWidget(saveStyle, 1, 0); customLayout->addWidget(deleteStyle, 1, 1);
    layout->addWidget(customRow); customRow->hide();
    connect(m_styleTabs, &QTabBar::currentChanged, this, [this, customRow](int tab) {
        m_preset->setVisible(tab == 0); customRow->setVisible(tab == 1);
    });
    loadCustomStyles();
    connect(saveStyle, &QPushButton::clicked, this, [this] {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Мой стиль субтитров"), QStringLiteral("Название"),
                                                   QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted || name.isEmpty() || name.size() > 80) return;
        for (int i = 0; i < m_customStyles.size(); ++i) if (m_customStyles[i].toObject().value(QStringLiteral("name")).toString() == name) {
            m_customStyles.removeAt(i); break;
        }
        m_customStyles.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("values"), m_style}});
        saveCustomStyles(); m_customList->setCurrentText(name);
    });
    connect(deleteStyle, &QPushButton::clicked, this, [this] {
        const int index = m_customList->currentIndex();
        if (index < 0 || index >= m_customStyles.size()) return;
        m_customStyles.removeAt(index); saveCustomStyles();
    });
    connect(m_customList, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (index >= 0 && index < m_customStyles.size())
            applyStyle(m_customStyles[index].toObject().value(QStringLiteral("values")).toObject());
    });
    m_previewText = new QLineEdit(QStringLiteral("Пример субтитров"), this);
    m_previewText->setMaxLength(4096);
    m_previewText->setObjectName(QStringLiteral("studioSubtitlePreviewText"));
    m_previewText->setAccessibleName(QStringLiteral("Текст предпросмотра субтитров"));
    layout->addWidget(m_previewText);
    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("studioSubtitlePreview"));
    m_preview->setText(QStringLiteral("Предпросмотр субтитров"));
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumHeight(120);
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_preview->installEventFilter(this);
    layout->addWidget(m_preview);
    m_seek = new QSlider(Qt::Horizontal, this);
    m_seek->setObjectName(QStringLiteral("studioSubtitleSeek"));
    m_seek->setRange(0, 1000);
    layout->addWidget(m_seek);
    auto previewControls = new QGridLayout;
    m_play = new QPushButton(QStringLiteral("Играть"), this);
    m_play->setObjectName(QStringLiteral("studioSubtitlePlay"));
    auto stop = new QPushButton(QStringLiteral("Стоп"), this);
    auto middle = new QPushButton(QStringLiteral("Середина"), this);
    previewControls->addWidget(m_play, 0, 0); previewControls->addWidget(stop, 0, 1);
    previewControls->addWidget(middle, 1, 0, 1, 2);
    layout->addLayout(previewControls);
    connect(m_previewText, &QLineEdit::textChanged, this, &StudioSubtitlePage::updatePreview);
    connect(m_seek, &QSlider::valueChanged, this, [this] {
        if (m_previewTimer->isActive()) renderPreview(); else updatePreview();
    });
    connect(m_play, &QPushButton::clicked, this, [this] {
        if (m_previewTimer->isActive()) stopPreview();
        else { if (m_seek->value() >= 1000) m_seek->setValue(0); m_previewTimer->start(); m_play->setText(QStringLiteral("Пауза")); }
    });
    connect(stop, &QPushButton::clicked, this, [this] { stopPreview(); m_seek->setValue(0); });
    connect(middle, &QPushButton::clicked, m_seek, [this] { m_seek->setValue(500); });
    if (!m_schemaReady) {
        m_preset->setEnabled(false);
        m_width->setRange(45, 85);
        m_width->setValue(60);
        m_width->setEnabled(false);
        return;
    }
    m_style = m_presets.first().toObject().value(QStringLiteral("values")).toObject();

    const auto section = [this, layout](const QString &title, bool open) {
        auto toggle = new QToolButton(this);
        toggle->setText(title);
        toggle->setCheckable(true);
        toggle->setChecked(open);
        toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
        layout->addWidget(toggle);
        auto body = new QWidget(this);
        auto form = new QFormLayout(body);
        form->setContentsMargins(0, 0, 0, 0);
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        layout->addWidget(body);
        body->setVisible(open);
        connect(toggle, &QToolButton::toggled, body, &QWidget::setVisible);
        return form;
    };
    auto appearance = section(QStringLiteral("Оформление"), true);
    m_appearanceForm = appearance;
    auto placement = section(QStringLiteral("Положение"), false);
    auto animation = section(QStringLiteral("Анимация"), false);
    for (const auto &entry : m_parameters) {
        const auto spec = entry.toObject();
        const QString id = spec.value(QStringLiteral("id")).toString();
        const QString kind = spec.value(QStringLiteral("kind")).toString();
        const QString label = spec.value(QStringLiteral("label")).toString();
        if (id.isEmpty() || label.isEmpty()) continue;
        QFormLayout *form = id == QLatin1String("animation") ? animation
            : (id == QLatin1String("max_width") || id == QLatin1String("position")
               || id == QLatin1String("safe_margin") || id == QLatin1String("min_duration")
               || id == QLatin1String("max_duration") || id == QLatin1String("max_reading_cps")) ? placement : appearance;
        QWidget *control = nullptr;
        if (kind == QLatin1String("number")) {
            auto spin = id == QLatin1String("max_width") ? m_width : new QSpinBox(this);
            spin->setRange(spec.value(QStringLiteral("min")).toInt(), spec.value(QStringLiteral("max")).toInt());
            spin->setValue(m_style.value(id).toInt(spec.value(QStringLiteral("default")).toInt()));
            spin->setSuffix(QStringLiteral(" ") + spec.value(QStringLiteral("unit")).toString());
            connect(spin, &QSpinBox::valueChanged, this, [this, id](int value) {
                m_style.insert(id, value); m_preset->setCurrentIndex(0); updatePreview();
            });
            control = spin;
        } else if (kind == QLatin1String("choice")) {
            auto combo = new QComboBox(this);
            for (const auto &option : spec.value(QStringLiteral("options")).toArray()) combo->addItem(option.toString());
            combo->setCurrentIndex(m_style.value(id).toInt(spec.value(QStringLiteral("default")).toInt()));
            connect(combo, &QComboBox::currentIndexChanged, this, [this, id](int value) {
                if (value < 0) return;
                m_style.insert(id, value); m_preset->setCurrentIndex(0); updatePreview();
            });
            control = combo;
        } else if (kind == QLatin1String("font")) {
            auto font = new QFontComboBox(this);
            font->setCurrentFont(QFont(m_style.value(id).toString(spec.value(QStringLiteral("default")).toString())));
            connect(font, &QFontComboBox::currentFontChanged, this, [this, id](const QFont &value) {
                m_style.insert(id, value.family()); m_preset->setCurrentIndex(0); updatePreview();
            });
            control = font;
        } else if (kind == QLatin1String("color")) {
            auto button = new QPushButton(this);
            const QColor current(m_style.value(id).toString(spec.value(QStringLiteral("default")).toString()));
            button->setText(current.name(QColor::HexArgb));
            connect(button, &QPushButton::clicked, this, [this, id, button] {
                const QColor chosen = QColorDialog::getColor(QColor(m_style.value(id).toString()), this,
                                                             QStringLiteral("Цвет субтитров"), QColorDialog::ShowAlphaChannel);
                if (!chosen.isValid()) return;
                const QString value = chosen.name(QColor::HexArgb);
                button->setText(value); m_style.insert(id, value); m_preset->setCurrentIndex(0); updatePreview();
            });
            control = button;
        }
        if (!control) continue;
        control->setObjectName(QStringLiteral("studioSubtitle_") + id);
        m_styleControls.insert(id, control);
        form->addRow(label, control);
    }
    connect(m_preset, &QComboBox::activated, this, [this](int index) {
        if (index > 0 && index <= m_presets.size())
            applyStyle(m_presets.at(index - 1).toObject().value(QStringLiteral("values")).toObject());
    });
    m_preset->setCurrentIndex(1);
    updatePreview();
}

void StudioSubtitlePage::applyStyle(const QJsonObject &values)
{
    for (const auto &entry : m_parameters) {
        const auto spec = entry.toObject();
        const QString id = spec.value(QStringLiteral("id")).toString();
        auto control = m_styleControls.value(id);
        if (!control || !values.contains(id)) continue;
        const QSignalBlocker blocked(control);
        const auto value = values.value(id);
        if (auto spin = qobject_cast<QSpinBox *>(control)) spin->setValue(value.toInt());
        else if (auto font = qobject_cast<QFontComboBox *>(control)) font->setCurrentFont(QFont(value.toString()));
        else if (auto combo = qobject_cast<QComboBox *>(control)) combo->setCurrentIndex(value.toInt());
        else if (auto button = qobject_cast<QPushButton *>(control)) button->setText(value.toString());
    }
    m_style = values;
    updatePreview();
}

void StudioSubtitlePage::updatePreview()
{
    if (m_appearanceForm) {
        if (auto control = m_styleControls.value(QStringLiteral("outline_color")))
            m_appearanceForm->setRowVisible(control, m_style.value(QStringLiteral("outline")).toInt() > 0);
        if (auto control = m_styleControls.value(QStringLiteral("background_color")))
            m_appearanceForm->setRowVisible(control, m_style.value(QStringLiteral("background")).toInt() > 0);
    }
    if (isVisible()) {
        m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
        m_preview->setText(QStringLiteral("Обновление предпросмотра…"));
        m_previewUpdate->start();
    }
}

void StudioSubtitlePage::loadCustomStyles()
{
    m_customStyles = QJsonDocument::fromJson(QSettings().value(QStringLiteral("StudioSubtitles/customStyles")).toByteArray()).array();
    m_customList->clear();
    for (const auto &entry : m_customStyles) {
        const QString name = entry.toObject().value(QStringLiteral("name")).toString();
        if (!name.isEmpty()) m_customList->addItem(name);
    }
}

void StudioSubtitlePage::saveCustomStyles()
{
    QSettings().setValue(QStringLiteral("StudioSubtitles/customStyles"), QJsonDocument(m_customStyles).toJson(QJsonDocument::Compact));
    loadCustomStyles();
}

void StudioSubtitlePage::stopPreview()
{
    m_previewTimer->stop(); m_previewUpdate->stop();
    m_play->setText(QStringLiteral("Играть"));
}

void StudioSubtitlePage::renderPreview()
{
    if (!isVisible()) { stopPreview(); return; }
    const QString text = m_previewText->text().trimmed();
    if (text.isEmpty()) {
        m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
        m_preview->setText(QStringLiteral("Введите текст для предпросмотра")); return;
    }
    const auto &profile = pCore->getProjectProfile();
    const QByteArray signature = QJsonDocument(m_style).toJson(QJsonDocument::Compact) + '\n' + text.toUtf8()
        + '\n' + QByteArray::number(profile.width()) + 'x' + QByteArray::number(profile.height())
        + '@' + QByteArray::number(profile.frame_rate_num()) + '/' + QByteArray::number(profile.frame_rate_den());
    if (signature != m_previewSignature || !m_previewProducer) {
        const QString worker = StudioResources::executable(QStringLiteral("studio-subtitle"));
        if (worker.isEmpty()) {
            m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
            m_preview->setText(QStringLiteral("Обработчик субтитров не найден.")); return;
        }
        QProcess process;
        QStringList args{QStringLiteral("--preview-text"), text};
        args.append(subtitleStyleArgs(m_style, m_width->value()));
        process.start(worker, args);
        if (!process.waitForStarted(1000) || !process.waitForFinished(2000)) {
            process.kill(); process.waitForFinished(1000);
            m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
            m_preview->setText(QStringLiteral("Предпросмотр субтитров не ответил вовремя.")); return;
        }
        const QByteArray ass = process.readAllStandardOutput();
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0
            || !ass.startsWith("[Script Info]") || ass.size() > 1024 * 1024) {
            m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
            m_preview->setText(QStringLiteral("Не удалось создать кадр субтитров.")); return;
        }
        if (!m_previewFolder) m_previewFolder = std::make_unique<QTemporaryDir>();
        if (!m_previewFolder->isValid()) {
            m_preview->setText(QStringLiteral("Нет места для временного предпросмотра.")); return;
        }
        const QString path = m_previewFolder->filePath(QStringLiteral("preview.ass"));
        m_previewProducer.reset();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(ass) != ass.size() || !file.commit()) {
            m_preview->setText(QStringLiteral("Не удалось сохранить временный кадр субтитров.")); return;
        }
        auto producer = std::make_unique<Mlt::Producer>(pCore->getProjectProfile(), "color", "#182638");
        Mlt::Filter filter(pCore->getProjectProfile(), "avfilter.subtitles");
        if (!producer->is_valid() || !filter.is_valid()) {
            m_preview->setText(QStringLiteral("Видеофильтр субтитров недоступен.")); return;
        }
        filter.set("av.filename", path.toUtf8().constData());
        producer->attach(filter);
        m_previewProducer = std::move(producer);
        m_previewSignature = signature;
        m_previewAss = path;
    }
    const int frameNumber = qRound(m_seek->value() * 4. * pCore->getCurrentFps() / 1000.);
    m_previewProducer->seek(frameNumber);
    std::unique_ptr<Mlt::Frame> frame(m_previewProducer->get_frame());
    auto format = mlt_image_rgba;
    int width = qMax(64, profile.width() / 2), height = qMax(64, profile.height() / 2);
    const auto pixels = frame ? frame->get_image(format, width, height) : nullptr;
    if (!pixels || width < 1 || height < 1) {
        m_previewImage = QImage(); m_preview->setPixmap(QPixmap());
        m_preview->setText(QStringLiteral("Не удалось отрисовать субтитры.")); return;
    }
    m_previewImage = QImage(pixels, width, height, width * 4, QImage::Format_RGBA8888).copy();
    m_preview->setProperty("studioRenderedFrames", ++m_renderCount);
    displayPreview();
}

void StudioSubtitlePage::displayPreview()
{
    if (!m_previewImage.isNull())
        m_preview->setPixmap(QPixmap::fromImage(m_previewImage).scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

bool StudioSubtitlePage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_preview && event->type() == QEvent::Resize) displayPreview();
    return QWidget::eventFilter(watched, event);
}

void StudioSubtitlePage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updatePreview();
}

void StudioSubtitlePage::hideEvent(QHideEvent *event)
{
    stopPreview();
    QWidget::hideEvent(event);
}

bool StudioSubtitlePage::selectedClip(QString *error)
{
    auto widget = pCore && pCore->window() ? pCore->window()->getCurrentTimeline() : nullptr;
    auto model = widget && !pCore->closing ? widget->model() : nullptr;
    QSet<int> clips;
    if (model) for (int id : model->getCurrentSelection()) if (model->isClip(id)) {
        const int partner = model->getClipSplitPartner(id);
        clips.insert(partner >= 0 && model->clipIsAudio(partner) ? partner : id);
    }
    if (clips.size() != 1) {
        if (error) *error = clips.isEmpty() ? QStringLiteral("Выберите один клип со звуком на таймлайне.")
                                           : QStringLiteral("Для точных таймингов выберите один клип со звуком.");
        return false;
    }
    const int selected = *clips.cbegin();
    const int track = model->getItemTrackId(selected);
    if (track < 0 || model->trackIsLocked(track)) { if (error) *error = QStringLiteral("Дорожка выбранного клипа заблокирована."); return false; }
    m_timeline = model; m_clipId = selected; m_startFrame = model->getItemPosition(m_clipId); m_playtime = model->getItemPlaytime(m_clipId);
    return true;
}

void StudioSubtitlePage::refreshSelection()
{
    if (m_stage != Stage::Idle) return;
    QString error;
    const bool clipSelected = selectedClip(&error);
    const auto model = pCore && pCore->projectManager() ? pCore->projectManager()->getTimeline() : nullptr;
    const auto subtitles = model ? model->getSubtitleModel() : nullptr;
    int selectedSubtitles = 0;
    if (model && subtitles) for (int id : model->getCurrentSelection()) if (subtitles->hasSubtitle(id)) ++selectedSubtitles;
    if (selectedSubtitles) m_target->setText(QStringLiteral("Выбрано субтитров: %1").arg(selectedSubtitles));
    else if (clipSelected) m_target->setText(QStringLiteral("Выбран: %1").arg(m_timeline->getClipName(m_clipId)));
    else m_target->setText(error);
    m_edit->setEnabled(subtitles && !subtitles->getAllSubIds().empty());
    m_restyleAll->setEnabled(m_schemaReady && subtitles && !subtitles->getAllSubIds().empty());
    m_restyle->setEnabled(m_schemaReady && subtitles && !subtitles->isLocked()
                          && (selectedSubtitles || m_restyleAll->isChecked()));
    updatePreview();
}

QString StudioSubtitlePage::revisionToken() const
{
    if (!pCore || !pCore->currentDoc() || !pCore->undoStack() || !m_timeline || !m_timeline->isClip(m_clipId)) return {};
    QByteArray data = pCore->currentDoc()->url().toString(QUrl::FullyEncoded).toUtf8();
    data += ':' + m_timeline->uuid().toString(QUuid::WithoutBraces).toUtf8();
    data += ':' + QByteArray::number(pCore->undoStack()->index());
    data += ':' + QByteArray::number(m_clipId);
    data += ':' + m_timeline->getClipBinId(m_clipId).toUtf8();
    data += ':' + QByteArray::number(m_timeline->getItemTrackId(m_clipId));
    data += ':' + QByteArray::number(m_timeline->getItemPosition(m_clipId));
    data += ':' + QByteArray::number(m_timeline->getItemPlaytime(m_clipId));
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool StudioSubtitlePage::unchangedJob() const
{
    const auto widget = pCore && pCore->window() ? pCore->window()->getCurrentTimeline() : nullptr;
    if (!widget || pCore->currentDoc() != m_document || widget->model() != m_timeline
        || m_timeline->uuid() != m_sequence || !m_timeline->isClip(m_clipId)
        || m_timeline->getClipBinId(m_clipId) != m_binId || revisionToken() != m_revision) return false;
    QSet<int> selected;
    for (int id : m_timeline->getCurrentSelection()) if (m_timeline->isClip(id)) {
        const int partner = m_timeline->getClipSplitPartner(id);
        selected.insert(partner >= 0 && m_timeline->clipIsAudio(partner) ? partner : id);
    }
    return selected.size() == 1 && selected.contains(m_clipId);
}

void StudioSubtitlePage::openEditor()
{
    const auto widget = pCore && pCore->window() ? pCore->window()->getCurrentTimeline() : nullptr;
    const auto timeline = widget ? widget->model() : nullptr;
    const auto subtitles = timeline ? timeline->getSubtitleModel() : nullptr;
    if (!subtitles || subtitles->getAllSubIds().empty()) {
        m_status->setText(QStringLiteral("На текущей дорожке пока нет субтитров."));
        return;
    }
    int chosen = -1;
    int distance = std::numeric_limits<int>::max();
    const int playhead = pCore->getMonitorPosition();
    for (int id : subtitles->getAllSubIds()) {
        const auto range = subtitles->getInOut(id);
        if (subtitles->getLayerForId(id) == 0 && subtitles->getStyleName(id).endsWith(QLatin1String("-Box"))
            && subtitles->getEffects(id).startsWith(QLatin1String("SUNIMO1:"))) {
            const int mainId = subtitles->getIdForStartPos(1, GenTime(range.first, pCore->getCurrentFps()));
            if (mainId >= 0 && subtitles->getInOut(mainId) == range
                && subtitles->getStyleName(id) == subtitles->getStyleName(mainId) + QStringLiteral("-Box")
                && subtitles->getEffects(mainId) == subtitles->getEffects(id)) continue;
        }
        const int delta = playhead < range.first ? range.first - playhead : playhead >= range.second ? playhead - range.second : 0;
        if (delta < distance) { chosen = id; distance = delta; }
    }
    pCore->window()->showSubtitleTrack();
    timeline->requestSetSelection({chosen});
    widget->controller()->showAsset(chosen);
}

bool StudioSubtitlePage::writeAudioSnapshot(const QString &path, QString *error) const
{
    if (!m_timeline || !m_timeline->isClip(m_clipId) || m_playtime <= 0) return false;
    const auto producer = m_timeline->getClipProducer(m_clipId);
    if (!producer || !producer->is_valid()) { if (error) *error = QStringLiteral("Не удалось прочитать звук выбранного клипа."); return false; }
    Mlt::Profile &profile = pCore->getProjectProfile(); Mlt::Tractor tractor(profile); Mlt::Playlist playlist(profile);
    playlist.set("hide", 1); playlist.insert_at(0, producer.get(), 1); tractor.set_track(playlist, 0); tractor.set_in_and_out(0, m_playtime - 1);
    if (pCore->currentDoc()) tractor.set("kdenlive:projectroot", pCore->currentDoc()->documentRoot().toUtf8().constData());
    QReadLocker lock(&pCore->xmlMutex); Mlt::Consumer consumer(profile, "xml", path.toUtf8().constData());
    consumer.set("terminate_on_pause", 1); consumer.set("store", "kdenlive"); consumer.connect(tractor);
    if (consumer.run() != 0 || !StudioManagedAudio::removeStudioMutes(path)) {
        if (error) *error = QStringLiteral("Не удалось подготовить звук клипа.");
        return false;
    }
    return true;
}

void StudioSubtitlePage::restyle()
{
    if (m_stage != Stage::Idle || !m_schemaReady || !pCore || !pCore->currentDoc() || !pCore->undoStack()) return;
    const auto timeline = pCore->projectManager() ? pCore->projectManager()->getTimeline() : nullptr;
    const auto subtitles = timeline ? timeline->getSubtitleModel() : nullptr;
    if (!subtitles || subtitles->isLocked()) {
        m_status->setText(QStringLiteral("Разблокируй дорожку субтитров перед сменой оформления.")); return;
    }
    QString error;
    int withoutMarks = 0, staleMarks = 0;
    const bool wholeTrack = m_restyleAll->isChecked();
    const auto input = restyleInput(timeline, wholeTrack, &withoutMarks, &staleMarks, &error);
    if (input.isEmpty()) { m_status->setText(error); return; }
    const QString worker = StudioResources::executable(QStringLiteral("studio-subtitle"));
    if (worker.isEmpty()) { m_status->setText(QStringLiteral("В сборке нет обработчика субтитров.")); return; }
    m_temporary = std::make_unique<QTemporaryDir>(QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("studio-subtitles-style-XXXXXX")));
    if (!m_temporary->isValid()) { m_status->setText(QStringLiteral("Не удалось создать временный каталог оформления.")); return; }
    m_restyleSnapshot = QJsonDocument(input).toJson(QJsonDocument::Compact);
    QSaveFile file(m_temporary->filePath(QStringLiteral("blocks.json")));
    if (!file.open(QIODevice::WriteOnly) || file.write(m_restyleSnapshot) != m_restyleSnapshot.size() || !file.commit()) {
        m_status->setText(QStringLiteral("Не удалось сохранить временные данные субтитров.")); m_temporary.reset(); return;
    }
    m_document = pCore->currentDoc();
    m_timeline = timeline;
    m_sequence = timeline->uuid();
    m_restyleUndoIndex = pCore->undoStack()->index();
    m_restyleStyle = m_style;
    m_restyleWidth = m_width->value();
    m_restyleWholeTrack = wholeTrack;
    m_restyleWithoutMarks = withoutMarks;
    m_restyleStaleMarks = staleMarks;
    m_stage = Stage::Restyle; m_output.clear(); m_stderr.clear(); m_workerError.clear();
    m_progress->setValue(0); setBusy(true);
    m_status->setText(QStringLiteral("Меняю оформление выбранных субтитров…"));
    QStringList args{QStringLiteral("--render-blocks-json"), m_temporary->filePath(QStringLiteral("blocks.json"))};
    args.append(subtitleStyleArgs(m_restyleStyle, m_restyleWidth));
    StudioJobs::start(m_process, worker, args);
}

void StudioSubtitlePage::finishRestyle()
{
    const auto fail = [this](const QString &message) {
        m_stage = Stage::Idle; setBusy(false); m_status->setText(message);
        m_temporary.reset(); m_restyleSnapshot.clear();
    };
    const auto timeline = pCore && pCore->projectManager() ? pCore->projectManager()->getTimeline() : nullptr;
    const auto subtitles = timeline ? timeline->getSubtitleModel() : nullptr;
    if (!subtitles || subtitles->isLocked() || timeline != m_timeline || pCore->currentDoc() != m_document
        || timeline->uuid() != m_sequence || !pCore->undoStack() || pCore->undoStack()->index() != m_restyleUndoIndex
        || m_style != m_restyleStyle || m_width->value() != m_restyleWidth || m_restyleAll->isChecked() != m_restyleWholeTrack) {
        fail(QStringLiteral("Проект, выделение или стиль изменились во время оформления. Проект не изменён.")); return;
    }
    QString error;
    int withoutMarks = 0, staleMarks = 0;
    const auto current = restyleInput(timeline, m_restyleWholeTrack, &withoutMarks, &staleMarks, &error);
    if (current.isEmpty() || QJsonDocument(current).toJson(QJsonDocument::Compact) != m_restyleSnapshot) {
        fail(QStringLiteral("Субтитры изменились во время оформления. Проект не изменён.")); return;
    }
    const auto blocks = current.value(QStringLiteral("blocks")).toArray();
    const bool paired = m_restyleStyle.value(QStringLiteral("background")).toInt(1) != 0
        && m_restyleStyle.value(QStringLiteral("outline")).toInt(3) > 0;
    const int eventsPerBlock = paired ? 2 : 1;
    const double fps = pCore->getCurrentFps();
    QSet<int> replacedIds;
    for (const auto &value : blocks) for (const auto &source : value.toObject().value(QStringLiteral("source_ids")).toArray())
        replacedIds.insert(source.toInt());
    QSet<QString> newStarts;
    int parsed = 0;
    QString output = QString::fromUtf8(m_output);
    if (m_output.size() > 12 * 1024 * 1024 || !output.contains(QStringLiteral("[Events]"))) {
        fail(QStringLiteral("Обработчик вернул повреждённое оформление. Проект не изменён.")); return;
    }
    if (!isolateManagedSubtitleStyles(output, *subtitles, &error)) { fail(error); return; }
    m_output = output.toUtf8();
    for (const QString &line : output.split(QLatin1Char('\n'))) {
        if (!line.startsWith(QLatin1String("Dialogue:"))) continue;
        if (parsed >= blocks.size() * eventsPerBlock) break;
        const auto block = blocks[parsed / eventsPerBlock].toObject();
        std::pair<int, GenTime> position;
        const SubtitleEvent event(line.trimmed(), fps, 1., &position);
        const int start = position.second.frames(fps);
        const int end = event.endTime().frames(fps);
        const int layer = paired ? parsed % 2 : 0;
        const QString key = QStringLiteral("%1:%2").arg(position.first).arg(start);
        const int occupied = subtitles->getIdForStartPos(position.first, GenTime(start, fps));
        if (position.first != layer || start != block.value(QStringLiteral("start_frame")).toInt()
            || end != block.value(QStringLiteral("end_frame")).toInt() || newStarts.contains(key)
            || (occupied >= 0 && !replacedIds.contains(occupied))
            || (layer == (paired ? 1 : 0) && subtitlePlainText(event.text()) != block.value(QStringLiteral("text")).toString())) {
            fail(QStringLiteral("Оформление смещает кадры или пересекает другие субтитры. Проект не изменён.")); return;
        }
        newStarts.insert(key);
        ++parsed;
    }
    if (parsed != blocks.size() * eventsPerBlock) {
        fail(QStringLiteral("Обработчик вернул неполный набор субтитров. Проект не изменён.")); return;
    }
    QSaveFile file(m_temporary->filePath(QStringLiteral("restyled.ass")));
    if (!file.open(QIODevice::WriteOnly) || file.write(m_output) != m_output.size() || !file.commit()) {
        fail(QStringLiteral("Не удалось подготовить новое оформление. Проект не изменён.")); return;
    }
    const auto beforeIds = subtitles->getAllSubIds();
    auto stack = pCore->undoStack();
    stack->beginMacro(QStringLiteral("Переоформить субтитры"));
    bool applied = true;
    for (int id : replacedIds) {
        if (!subtitles->hasSubtitle(id)) { applied = false; break; }
        const auto range = subtitles->getInOut(id);
        subtitles->deleteSubtitle(subtitles->getLayerForId(id), range.first, range.second, subtitles->getText(id));
        if (subtitles->hasSubtitle(id)) { applied = false; break; }
    }
    if (applied) subtitles->importSubtitle(m_temporary->filePath(QStringLiteral("restyled.ass")), 0, true);
    const auto afterIds = subtitles->getAllSubIds();
    if (applied) {
        for (int id : beforeIds) if (!replacedIds.contains(id) && !afterIds.count(id)) applied = false;
        if (int(afterIds.size()) != int(beforeIds.size()) - replacedIds.size() + parsed) applied = false;
        for (const auto &value : blocks) {
            const auto block = value.toObject();
            const int start = block.value(QStringLiteral("start_frame")).toInt();
            const int mainId = subtitles->getIdForStartPos(paired ? 1 : 0, GenTime(start, fps));
            if (mainId < 0 || subtitlePlainText(subtitles->getText(mainId)) != block.value(QStringLiteral("text")).toString()
                || !subtitles->getEffects(mainId).startsWith(QLatin1String("SUNIMO1:"))) applied = false;
        }
    }
    stack->endMacro();
    if (!applied || stack->index() != m_restyleUndoIndex + 1) {
        if (stack->index() == m_restyleUndoIndex + 1) stack->undo();
        fail(QStringLiteral("Не удалось заменить все блоки. Изменения отменены.")); return;
    }
    const auto first = blocks.first().toObject();
    const int firstId = subtitles->getIdForStartPos(paired ? 1 : 0, GenTime(first.value(QStringLiteral("start_frame")).toInt(), fps));
    if (firstId >= 0) timeline->requestSetSelection({firstId});
    QString message = QStringLiteral("Переоформлено блоков: %1.").arg(blocks.size());
    if (m_restyleStaleMarks) message += QStringLiteral(" У %1 блоков метки слов устарели после правки текста.").arg(m_restyleStaleMarks);
    if (m_restyleWithoutMarks) message += QStringLiteral(" У %1 блоков нет точных меток: движение применено ко всей фразе.").arg(m_restyleWithoutMarks);
    m_stage = Stage::Idle; setBusy(false); m_status->setText(message);
    m_temporary.reset(); m_restyleSnapshot.clear();
}

void StudioSubtitlePage::start()
{
    if (!m_schemaReady) { m_status->setText(QStringLiteral("В сборке отсутствуют настройки субтитров.")); return; }
    QString error;
    if (m_stage != Stage::Idle || !selectedClip(&error)) { if (!error.isEmpty()) m_status->setText(error); return; }
    if (!pCore->currentDoc() || pCore->currentDoc()->url().isEmpty()) { m_status->setText(QStringLiteral("Сначала сохраните проект.")); return; }
    m_document = pCore->currentDoc();
    m_sequence = m_timeline->uuid();
    m_binId = m_timeline->getClipBinId(m_clipId);
    m_revision = revisionToken();
    if (m_revision.isEmpty()) { m_status->setText(QStringLiteral("Не удалось запомнить состояние выбранного клипа.")); return; }
    const QString melt = StudioResources::executable(QStringLiteral("melt"));
    const QString worker = StudioResources::executable(QStringLiteral("studio-subtitle"));
    const QString whisper = StudioResources::executable(QStringLiteral("whisper-cli"));
    const QString model = StudioResources::dataFile(QStringLiteral("studio-subtitles/ggml-small-q5_1.bin"));
    const QString vadModel = StudioResources::dataFile(QStringLiteral("studio-subtitles/ggml-silero-v5.1.2.bin"));
    if (melt.isEmpty() || worker.isEmpty() || whisper.isEmpty() || !QFileInfo::exists(model) || !QFileInfo::exists(vadModel)) {
        m_status->setText(QStringLiteral("В сборке отсутствует локальный обработчик, модель речи или детектор речи.")); return;
    }
    const QString cache = QDir(pCore->currentDoc()->projectDataFolder()).absoluteFilePath(QStringLiteral("studio-subtitles"));
    if (!QDir().mkpath(cache)) { m_status->setText(QStringLiteral("Не удалось создать каталог субтитров проекта.")); return; }
    m_temporary = std::make_unique<QTemporaryDir>(QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("studio-subtitles-XXXXXX")));
    if (!m_temporary->isValid()) { m_status->setText(QStringLiteral("Не удалось создать временный каталог.")); return; }
    const QString xml = m_temporary->filePath(QStringLiteral("clip.mlt"));
    if (!writeAudioSnapshot(xml, &error)) { m_status->setText(error); m_temporary.reset(); return; }
    m_stage = Stage::Extract; m_output.clear(); m_stderr.clear(); m_extractLog.clear(); m_workerError.clear(); setBusy(true);
    m_progress->setRange(0, 100);
    m_status->setText(QStringLiteral("Подготовка звука выбранного клипа…")); m_progress->setValue(0);
    StudioJobs::start(m_process, melt, {xml, QStringLiteral("-progress2"), QStringLiteral("-consumer"), QStringLiteral("avformat:%1").arg(m_temporary->filePath(QStringLiteral("clip.wav"))),
                            QStringLiteral("vn=1"), QStringLiteral("acodec=pcm_s16le"), QStringLiteral("ar=16000"), QStringLiteral("ac=1"), QStringLiteral("f=wav"), QStringLiteral("threads=2"), QStringLiteral("real_time=-1")});
}

void StudioSubtitlePage::runRecognition()
{
    if (!unchangedJob()) {
        m_stage = Stage::Idle; setBusy(false);
        m_status->setText(QStringLiteral("Клип или проект изменился во время подготовки звука. Проект не изменён."));
        m_temporary.reset(); return;
    }
    m_stage = Stage::Recognize; m_output.clear(); m_stderr.clear(); m_workerError.clear();
    m_status->setText(QStringLiteral("Распознавание русской речи на устройстве…")); m_progress->setRange(0, 0);
    const QString cache = QDir(pCore->currentDoc()->projectDataFolder()).absoluteFilePath(QStringLiteral("studio-subtitles"));
    const QString worker = StudioResources::executable(QStringLiteral("studio-subtitle"));
    const QString whisper = StudioResources::executable(QStringLiteral("whisper-cli"));
    QStringList args{QStringLiteral("--audio"), m_temporary->filePath(QStringLiteral("clip.wav")),
                     QStringLiteral("--model"), StudioResources::dataFile(QStringLiteral("studio-subtitles/ggml-small-q5_1.bin")),
                     QStringLiteral("--vad-model"), StudioResources::dataFile(QStringLiteral("studio-subtitles/ggml-silero-v5.1.2.bin")),
                     QStringLiteral("--whisper-cli"), whisper, QStringLiteral("--out-dir"), cache};
    args.append(subtitleStyleArgs(m_style, m_width->value()));
    StudioJobs::start(m_process, worker, args);
}

void StudioSubtitlePage::finished(int exitCode, QProcess::ExitStatus status)
{
    if (m_stage == Stage::Idle) return;
    m_extractTimeout->stop();
    m_output += m_process->readAllStandardOutput();
    if (status != QProcess::NormalExit || exitCode != 0) {
        const bool extracting = m_stage == Stage::Extract;
        m_stage = Stage::Idle; setBusy(false);
        m_status->setText(m_workerError.isEmpty() ? (extracting ? QStringLiteral("Не удалось подготовить звук (код %1). %2 Проект не изменён.")
                                                            .arg(exitCode).arg(QString::fromUtf8(m_extractLog.trimmed().right(300)))
                                                            : QStringLiteral("Обработка отменена или завершилась ошибкой. Проект не изменён."))
                                               : m_workerError);
        m_temporary.reset(); m_restyleSnapshot.clear(); return;
    }
    if (m_stage == Stage::Restyle) { finishRestyle(); return; }
    if (m_stage == Stage::Extract) {
        QFile wav(m_temporary->filePath(QStringLiteral("clip.wav")));
        if (!wav.open(QIODevice::ReadOnly) || wav.size() <= 44 || wav.read(4) != "RIFF"
            || !wav.seek(8) || wav.read(4) != "WAVE") {
            m_stage = Stage::Idle; setBusy(false);
            m_status->setText(QStringLiteral("Подготовка звука не создала корректный WAV. Проект не изменён."));
            m_temporary.reset(); return;
        }
        runRecognition(); return;
    }
    const auto result = QJsonDocument::fromJson(m_output).object(); const QString ass = result.value(QStringLiteral("ass")).toString();
    const QString blocksPath = result.value(QStringLiteral("blocks")).toString();
    if (m_stage != Stage::Recognize || ass.isEmpty() || !QFileInfo::exists(ass) || !unchangedJob() || blocksPath.isEmpty()) {
        m_stage = Stage::Idle; setBusy(false); m_status->setText(QStringLiteral("Клип изменился во время анализа или результат повреждён. Проект не изменён.")); m_temporary.reset(); return;
    }
    QFile blocksFile(blocksPath);
    const auto blocksDocument = blocksFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(blocksFile.readAll()) : QJsonDocument();
    const auto blocks = blocksDocument.object().value(QStringLiteral("blocks"));
    if (blocksDocument.object().value(QStringLiteral("schema")).toInt() != 2 || !blocks.isArray() || blocks.toArray().isEmpty()) {
        m_stage = Stage::Idle; setBusy(false); m_status->setText(QStringLiteral("Временные метки субтитров повреждены. Проект не изменён.")); m_temporary.reset(); return;
    }
    QFile assFile(ass);
    QHash<int, int> layersByStart;
    int parsed = 0;
    QString importAss;
    if (assFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        importAss = QString::fromUtf8(assFile.readAll());
        QTextStream stream(&importAss, QIODevice::ReadOnly);
        while (!stream.atEnd()) {
            const QString line = stream.readLine().simplified();
            if (!line.startsWith(QLatin1String("Dialogue:"))) continue;
            std::pair<int, GenTime> start;
            const SubtitleEvent event(line, pCore->getCurrentFps(), 1., &start);
            const int first = m_startFrame + start.second.frames(pCore->getCurrentFps());
            const int last = m_startFrame + event.endTime().frames(pCore->getCurrentFps());
            if (start.first < 0 || start.first > 1 || first < m_startFrame || first >= m_startFrame + m_playtime
                || last <= first || last > m_startFrame + m_playtime + qRound(pCore->getCurrentFps())
                || (layersByStart.value(first) & (1 << start.first))) { parsed = -1; break; }
            layersByStart[first] |= 1 << start.first;
            ++parsed;
        }
    }
    const bool paired = std::any_of(layersByStart.cbegin(), layersByStart.cend(), [](int layers) { return layers == 3; });
    if (parsed != int(blocks.toArray().size()) * (paired ? 2 : 1)
        || layersByStart.size() != blocks.toArray().size()
        || std::any_of(layersByStart.cbegin(), layersByStart.cend(), [paired](int layers) { return layers != (paired ? 3 : 1); })) {
        m_stage = Stage::Idle; setBusy(false);
        m_status->setText(QStringLiteral("Временные метки субтитров некорректны или совпадают после округления. Проект не изменён."));
        m_temporary.reset(); return;
    }
    int conflicts = 0;
    for (const auto &block : blocks.toArray()) conflicts += block.toObject().value(QStringLiteral("timing_conflict")).toBool();
    auto subtitles = m_timeline->getSubtitleModel();
    if (subtitles && !subtitles->getItemsInRange(-1, m_startFrame, m_startFrame + m_playtime).empty()) {
        m_stage = Stage::Idle; setBusy(false);
        m_status->setText(QStringLiteral("В этом фрагменте уже есть субтитры. Откройте редактор, чтобы изменить или удалить их перед повторным созданием."));
        m_temporary.reset(); return;
    }
    pCore->window()->showSubtitleTrack();
    subtitles = m_timeline->getSubtitleModel();
    if (!subtitles || subtitles->isLocked()) { m_stage = Stage::Idle; setBusy(false); m_status->setText(QStringLiteral("Разблокируйте дорожку субтитров и повторите.")); m_temporary.reset(); return; }
    QString error;
    if (!isolateManagedSubtitleStyles(importAss, *subtitles, &error)) {
        m_stage = Stage::Idle; setBusy(false); m_status->setText(error); m_temporary.reset(); return;
    }
    const QString importPath = m_temporary->filePath(QStringLiteral("import.ass"));
    QSaveFile importFile(importPath);
    const QByteArray importBytes = importAss.toUtf8();
    if (!importFile.open(QIODevice::WriteOnly) || importFile.write(importBytes) != importBytes.size() || !importFile.commit()) {
        m_stage = Stage::Idle; setBusy(false);
        m_status->setText(QStringLiteral("Не удалось подготовить оформление субтитров. Проект не изменён.")); m_temporary.reset(); return;
    }
    const auto beforeIds = subtitles->getAllSubIds();
    const int undoBefore = pCore->undoStack()->index();
    subtitles->importSubtitle(importPath, m_startFrame, true);
    const auto afterIds = subtitles->getAllSubIds();
    const auto added = afterIds.size() - beforeIds.size();
    if (added != std::size_t(parsed)) {
        if (pCore->undoStack()->index() == undoBefore + 1) pCore->undoStack()->undo();
        m_stage = Stage::Idle; setBusy(false);
        m_status->setText(QStringLiteral("Не удалось добавить все субтитры. Изменения отменены."));
        m_temporary.reset(); return;
    }
    m_stage = Stage::Idle; setBusy(false); m_progress->setRange(0, 100); m_progress->setValue(100);
    QString message = added > 0 ? QStringLiteral("Добавлено блоков: %1. Проверьте слова и время; текст правится на дорожке субтитров.").arg(blocks.toArray().size())
                                : QStringLiteral("Речь не добавлена: проверьте пересечения с существующими субтитрами.");
    if (added > 0 && conflicts > 0) message += QStringLiteral(" У %1 блоков пересекаются метки речи: исправьте их время на дорожке.").arg(conflicts);
    m_status->setText(message);
    m_edit->setEnabled(!afterIds.empty());
    if (added > 0) {
        int first = -1;
        for (int id : afterIds) if (!beforeIds.count(id) && subtitles->getLayerForId(id) == (paired ? 1 : 0)
            && (first < 0 || subtitles->getStartPosForId(id) < subtitles->getStartPosForId(first))) first = id;
        if (first >= 0) {
            m_timeline->requestSetSelection({first});
            pCore->window()->getCurrentTimeline()->controller()->showAsset(first);
        }
    }
    m_temporary.reset();
}

void StudioSubtitlePage::setBusy(bool busy)
{
    m_main->setEnabled(!busy && m_schemaReady);
    m_restyle->setEnabled(!busy && m_schemaReady);
    m_restyleAll->setEnabled(!busy && m_schemaReady);
    m_edit->setEnabled(!busy);
    m_preset->setEnabled(!busy && m_schemaReady);
    for (auto control : m_styleControls) control->setEnabled(!busy);
    m_cancel->setVisible(busy); m_progress->setVisible(busy || m_progress->value() == 100);
    if (!busy) refreshSelection();
}

void StudioSubtitlePage::activate() { refreshSelection(); }
void StudioSubtitlePage::deactivate() { stopPreview(); m_extractTimeout->stop(); StudioJobs::cancel(m_process); }
