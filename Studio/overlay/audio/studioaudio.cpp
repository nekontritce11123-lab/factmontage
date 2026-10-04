// SPDX-License-Identifier: GPL-3.0-only
#include "../core/studioresources.hpp"
#include "studioaudio.hpp"
#include "studiomanagedaudio.hpp"
#include "../studiohelpers.hpp"
using StudioHelpers::effectsById;
using StudioHelpers::appendParameterChange;

#include "bin/clipcreator.hpp"
#include "bin/model/markerlistmodel.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "definitions.h"
#include "doc/kdenlivedoc.h"
#include "doc/docundostack.hpp"
#include "effects/effectstack/model/effectitemmodel.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "mainwindow.h"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelinemodel.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "timeline2/model/trackmodel.hpp"
#include "timeline2/view/timelinewidget.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QReadWriteLock>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <mlt++/MltConsumer.h>
#include <mlt++/MltField.h>
#include <mlt++/MltPlaylist.h>
#include <mlt++/MltProducer.h>
#include <mlt++/MltTractor.h>
#include <mlt++/MltTransition.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>
#include <utility>

namespace {
QString sortedIds(const QSet<int> &ids)
{
    QList<int> values(ids.begin(), ids.end());
    std::sort(values.begin(), values.end());
    QStringList text;
    for (int id : values) text << QString::number(id);
    return text.join(QLatin1Char(','));
}

QSet<int> parsedIds(const QString &text)
{
    QSet<int> result;
    for (const auto &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int id = part.toInt(&ok);
        if (ok) result.insert(id);
    }
    return result;
}

QString fileSha256(const QString &path);

QByteArray settingsJson(StudioAudioController::Action action, const StudioAudioController::Settings &settings)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("action"), int(action)},
                                     {QStringLiteral("strength"), settings.strength},
                                     {QStringLiteral("presence"), settings.presence},
                                     {QStringLiteral("priority"), settings.priority},
                                     {QStringLiteral("target"), settings.targetLufs},
                                     {QStringLiteral("noise"), settings.noise},
                                     {QStringLiteral("levelTrack"), settings.levelTrack}}).toJson(QJsonDocument::Compact);
}

QMap<QString, QString> resultProperties(const QString &audio, const QString &report, const QString &hash, const QString &jobId,
                                        StudioAudioController::Action action, const StudioAudioController::Settings &settings,
                                        const QSet<int> &sources, const QSet<int> &voice, const QSet<int> &music)
{
    return {{QStringLiteral("resource"), audio},
            {QStringLiteral("studio:audio:id"), jobId},
            {QStringLiteral("studio:audio:role"), QStringLiteral("result")},
            {QStringLiteral("studio:audio:sources"), sortedIds(sources)},
            {QStringLiteral("studio:audio:voice"), sortedIds(voice)},
            {QStringLiteral("studio:audio:music"), sortedIds(music)},
            {QStringLiteral("studio:audio:action"), QString::number(int(action))},
            {QStringLiteral("studio:audio:settings"), QString::fromUtf8(settingsJson(action, settings))},
            {QStringLiteral("studio:audio:report"), report},
            {QStringLiteral("studio:audio:sha256"), hash}};
}

QMap<QString, QString> resolveAudioAssetProperties(QMap<QString, QString> properties, bool remember = false)
{
    const auto document = pCore ? pCore->currentDoc() : nullptr;
    if (!document) return properties;
    auto paths = document->property("_studioAudioAssetPaths").toMap();
    const QDir root(document->documentRoot());
    for (const auto &key : {QStringLiteral("resource"), QStringLiteral("studio:audio:report")}) {
        const QString stored = properties.value(key);
        if (stored.isEmpty()) continue;
        const QUrl url(stored);
        QString absolute = url.isLocalFile() ? url.toLocalFile() : stored;
        if (QDir::isRelativePath(absolute)) absolute = root.absoluteFilePath(absolute);
        QString resolved = paths.value(stored).toString();
        if (resolved.isEmpty()) resolved = paths.value(absolute).toString();
        if (resolved.isEmpty()) resolved = absolute;
        properties.insert(key, resolved);
        if (remember) {
            paths.insert(stored, resolved);
            paths.insert(absolute, resolved);
        }
    }
    if (remember) document->setProperty("_studioAudioAssetPaths", paths);
    return properties;
}

struct RecoveryData {
    bool broken = false;
    bool recoverable = false;
    QString message;
    StudioAudioController::Action action = StudioAudioController::Action::Voice;
    StudioAudioController::Settings settings;
    QSet<int> voice;
    QSet<int> music;
    QString jobId;
};

RecoveryData recoveryData(const std::shared_ptr<TimelineModel> &model)
{
    RecoveryData result;
    if (!model) return result;
    const auto selection = model->getCurrentSelection();
    if (selection.size() != 1) return result;
    const int resultId = *selection.cbegin();
    if (!model->isClip(resultId)) return result;
    const auto clip = pCore && pCore->projectItemModel() ? pCore->projectItemModel()->getClipByBinID(model->getClipBinId(resultId)) : nullptr;
    if (!clip || clip->getProducerProperty(QStringLiteral("studio:audio:role")) != QLatin1String("result")) return result;
    const QString resource = clip->getProducerProperty(QStringLiteral("resource"));
    const QString expectedHash = clip->getProducerProperty(QStringLiteral("studio:audio:sha256"));
    const bool missing = !QFileInfo::exists(resource);
    if (!missing && !expectedHash.isEmpty() && fileSha256(resource).compare(expectedHash, Qt::CaseInsensitive) == 0) return result;
    result.broken = true;
    result.message = missing ? QStringLiteral("Обработанный WAV не найден.") : QStringLiteral("Обработанный WAV повреждён.");
    const int action = clip->getProducerProperty(QStringLiteral("studio:audio:action")).toInt();
    QJsonParseError parseError;
    const auto saved = QJsonDocument::fromJson(clip->getProducerProperty(QStringLiteral("studio:audio:settings")).toUtf8(), &parseError).object();
    result.jobId = clip->getProducerProperty(QStringLiteral("studio:audio:id"));
    for (int trackId : model->getAllTracksIds()) {
        for (int clipId : model->getItemsInRange(trackId, 0, -1, false)) {
            for (const auto &effect : effectsById(model->getClipEffectStack(clipId), QStringLiteral("volume"))) {
                if (effect->getParam(QStringLiteral("studio:audio:role")) != QLatin1String("source-mute")
                    || effect->getParam(QStringLiteral("studio:audio:id")) != result.jobId) continue;
                const QString role = effect->getParam(QStringLiteral("studio:audio:source-role"));
                if (role == QLatin1String("voice")) result.voice.insert(clipId);
                else if (role == QLatin1String("music")) result.music.insert(clipId);
            }
        }
    }
    if (action < int(StudioAudioController::Action::Voice) || action > int(StudioAudioController::Action::Music)
        || parseError.error != QJsonParseError::NoError || saved.isEmpty() || (result.voice | result.music).isEmpty() || result.jobId.isEmpty()) {
        result.message += QStringLiteral(" Данных для автоматического восстановления недостаточно.");
        return result;
    }
    result.action = static_cast<StudioAudioController::Action>(action);
    result.settings.strength = saved.value(QStringLiteral("strength")).toInt(40);
    result.settings.presence = saved.value(QStringLiteral("presence")).toInt(50);
    result.settings.priority = saved.value(QStringLiteral("priority")).toInt(60);
    result.settings.targetLufs = saved.value(QStringLiteral("target")).toDouble(-16.);
    result.settings.noise = saved.value(QStringLiteral("noise")).toString(QStringLiteral("off"));
    result.settings.levelTrack = saved.value(QStringLiteral("levelTrack")).toBool(false);
    result.recoverable = true;
    result.message += QStringLiteral(" Можно создать его заново из сохранённых исходников.");
    return result;
}

bool studioJobForSources(const std::shared_ptr<TimelineModel> &model, const QSet<int> &ids, QString *jobId, QString *error)
{
    int matchedSources = 0;
    int expectedCount = 0;
    QString matchedJob;
    for (int id : ids) {
        int ownMutes = 0;
        for (const auto &effect : effectsById(model->getClipEffectStack(id), QStringLiteral("volume"))) {
            if (effect->getParam(QStringLiteral("studio:audio:role")) != QLatin1String("source-mute")) continue;
            ++ownMutes;
            const QString candidateJob = effect->getParam(QStringLiteral("studio:audio:id"));
            int candidateCount = effect->getParam(QStringLiteral("studio:audio:source-count")).toInt();
            if (candidateCount <= 0) candidateCount = parsedIds(effect->getParam(QStringLiteral("studio:audio:sources"))).size();
            if (candidateJob.isEmpty() || candidateCount <= 0 || (!matchedJob.isEmpty() && matchedJob != candidateJob)
                || (expectedCount > 0 && expectedCount != candidateCount)) {
                if (error) *error = QStringLiteral("Выделение частично пересекается с другим результатом StudioAudio. Сначала отмените его или выберите исходный диапазон целиком.");
                return false;
            }
            matchedJob = candidateJob;
            expectedCount = candidateCount;
        }
        // Identical owned duplicates are repairable in the apply transaction.
        // Different job identities/counts have already been rejected above.
        matchedSources += ownMutes > 0 ? 1 : 0;
    }
    if (matchedSources != 0 && matchedSources != ids.size()) {
        if (error) *error = QStringLiteral("Результат StudioAudio связан не со всем выделением; обработка остановлена без изменений.");
        return false;
    }
    if (!matchedJob.isEmpty()) {
        QSet<int> linked;
        for (int trackId : model->getAllTracksIds()) {
            for (int clipId : model->getItemsInRange(trackId, 0, -1, false)) {
                for (const auto &effect : effectsById(model->getClipEffectStack(clipId), QStringLiteral("volume"))) {
                    if (effect->getParam(QStringLiteral("studio:audio:role")) == QLatin1String("source-mute")
                        && effect->getParam(QStringLiteral("studio:audio:id")) == matchedJob) linked.insert(clipId);
                }
            }
        }
        if (linked != ids || linked.size() != expectedCount) {
            if (error) *error = QStringLiteral("Выберите все исходники связанного результата StudioAudio; частичное обновление запрещено.");
            return false;
        }
    }
    if (jobId) *jobId = matchedJob;
    return true;
}

int resultClipForJob(const std::shared_ptr<TimelineModel> &model, const QString &jobId)
{
    int found = -1;
    for (int trackId : model->getAllTracksIds()) {
        for (int clipId : model->getItemsInRange(trackId, 0, -1, false)) {
            const auto clip = pCore->projectItemModel()->getClipByBinID(model->getClipBinId(clipId));
            if (!clip || clip->getProducerProperty(QStringLiteral("studio:audio:role")) != QLatin1String("result")
                || clip->getProducerProperty(QStringLiteral("studio:audio:id")) != jobId) continue;
            if (found >= 0) return -2;
            found = clipId;
        }
    }
    return found;
}

bool parsePauseCuts(const QJsonArray &values, int start, int end, double fps, QVector<QPoint> &cuts)
{
    QVector<QPoint> parsed;
    for (const auto &value : values) {
        const auto pair = value.toArray();
        if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble()) return false;
        const double from = pair[0].toDouble(), to = pair[1].toDouble();
        if (!std::isfinite(from) || !std::isfinite(to) || from < 0 || to <= from || to > double(end - start) / fps) return false;
        const QPoint cut(start + qRound(from * fps), start + qRound(to * fps));
        if (cut.y() <= cut.x()) return false;
        parsed << cut;
    }
    std::sort(parsed.begin(), parsed.end(), [](const QPoint &a, const QPoint &b) { return a.x() > b.x(); });
    for (int i = 1; i < parsed.size(); ++i) if (parsed[i].y() > parsed[i - 1].x()) return false;
    cuts = parsed;
    return true;
}

bool adjustSubtitlesForCut(const std::shared_ptr<TimelineModel> &model, const QPoint &cut, Fun &undo, Fun &redo, QString *error)
{
    const auto subtitles = model->getSubtitleModel();
    if (!subtitles || subtitles->getAllSubtitles().isEmpty()) return true;
    if (subtitles->isLocked()) {
        if (error) *error = QStringLiteral("Разблокируйте дорожку субтитров перед сокращением пауз.");
        return false;
    }
    struct Change { int id; int layer; int start; int end; int nextStart; int nextEnd; };
    QList<Change> changes;
    QSet<QString> finalStarts;
    const int removed = cut.y() - cut.x();
    for (const auto &entry : subtitles->getAllSubtitles()) {
        const int layer = entry.first.first;
        const int start = entry.first.second.frames(pCore->getCurrentFps());
        const int end = entry.second.endTime().frames(pCore->getCurrentFps());
        Change change{subtitles->getIdForStartPos(layer, entry.first.second), layer, start, end, start, end};
        if (end <= cut.x()) {
            // unchanged
        } else if (start >= cut.y()) {
            change.nextStart -= removed;
            change.nextEnd -= removed;
        } else if (start >= cut.x() && end <= cut.y()) {
            change.nextEnd = change.nextStart;
        } else if (start < cut.x() && end > cut.y()) {
            change.nextEnd -= removed;
        } else if (start < cut.x()) {
            change.nextEnd = cut.x();
        } else {
            change.nextStart = cut.x();
            change.nextEnd -= removed;
        }
        if (change.nextEnd > change.nextStart) {
            const QString key = QStringLiteral("%1:%2").arg(layer).arg(change.nextStart);
            if (finalStarts.contains(key)) {
                if (error) *error = QStringLiteral("После сокращения две реплики субтитров получили бы одинаковое начало. Пауза не изменена.");
                return false;
            }
            finalStarts.insert(key);
        }
        changes << change;
    }
    for (const auto &change : changes) {
        if (change.nextEnd > change.nextStart) continue;
        if (!model->requestItemDeletion(change.id, undo, redo, true)) {
            if (error) *error = QStringLiteral("Не удалось удалить субтитр внутри сокращаемой паузы.");
            return false;
        }
    }
    std::sort(changes.begin(), changes.end(), [](const Change &a, const Change &b) { return a.start < b.start; });
    for (const auto &change : changes) {
        if (change.nextEnd <= change.nextStart) continue;
        const int nextDuration = change.nextEnd - change.nextStart;
        if (nextDuration != change.end - change.start
            && !subtitles->requestResize(change.id, nextDuration, true, undo, redo, true)) {
            if (error) *error = QStringLiteral("Не удалось изменить длительность пересекающего паузу субтитра.");
            return false;
        }
        if (change.nextStart != change.start
            && !model->requestSubtitleMove(change.id, change.layer, change.nextStart, true, true, true, true, undo, redo)) {
            if (error) *error = QStringLiteral("Не удалось синхронно сдвинуть субтитры.");
            return false;
        }
    }
    return true;
}

QString fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

QWidget *sliderRow(const QJsonObject &spec, QSlider **slider, QLabel **value, QWidget *parent)
{
    auto row = new QWidget(parent);
    auto layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto label = new QLabel(spec.value(QStringLiteral("label")).toString(), row);
    label->setWordWrap(true);
    layout->addWidget(label);
    *slider = new QSlider(Qt::Horizontal, row);
    (*slider)->setAccessibleName(label->text());
    (*slider)->setRange(spec.value(QStringLiteral("min")).toInt(), spec.value(QStringLiteral("max")).toInt());
    layout->addWidget(*slider, 1);
    *value = new QLabel(row);
    (*value)->setMinimumWidth(34);
    layout->addWidget(*value);
    QObject::connect(*slider, &QSlider::valueChanged, *value, [out = *value](int number) { out->setText(QStringLiteral("%1 %").arg(number)); });
    (*slider)->setValue(spec.value(QStringLiteral("default")).toInt());
    (*value)->setText(QStringLiteral("%1 %").arg((*slider)->value()));
    return row;
}
}

struct StudioAudioController::Job
{
    ~Job() { QObject::disconnect(revisionConnection); }
    struct Command { QString program; QStringList arguments; QString label; };
    Action action = Action::Voice;
    Settings settings;
    QSet<int> sourceIds;
    QSet<int> voiceIds;
    QSet<int> musicIds;
    QString jobId;
    QString outputId;
    QString token;
    QString outputDir;
    int startFrame = 0;
    int endFrame = 0;
    bool replacing = false;
    bool invalidated = false;
    QMetaObject::Connection revisionConnection;
    std::unique_ptr<QTemporaryDir> temporary;
    QList<Command> commands;
    std::shared_ptr<Fun> rollbackImport;
};

StudioAudioController::StudioAudioController(QObject *parent)
    : QObject(parent)
    , m_process(new QProcess(this))
{
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
        const QByteArray data = m_process->readAllStandardOutput();
        m_processOutput += data;
        m_processOutput = m_processOutput.right(65536);
        m_progressOutput += data;
        int end;
        while ((end = m_progressOutput.indexOf('\n')) >= 0) {
            const auto line = m_progressOutput.left(end);
            m_progressOutput.remove(0, end + 1);
            if (!line.startsWith("PROGRESS\t")) continue;
            const auto fields = line.split('\t');
            if (fields.size() > 1) Q_EMIT progressChanged(fields[1].toInt());
            if (fields.size() > 2) Q_EMIT statusChanged(QString::fromUtf8(fields[2]), false);
        }
        m_progressOutput = m_progressOutput.right(65536);
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &StudioAudioController::processFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_job) {
            fail(QStringLiteral("Не удалось запустить обработчик звука: %1").arg(m_process->errorString()));
        }
    });
}

StudioAudioController::~StudioAudioController()
{
    StudioJobs::dispose(m_process);
    discardOutput();
}

std::shared_ptr<TimelineItemModel> StudioAudioController::timeline() const
{
    auto widget = pCore && pCore->window() ? pCore->window()->getCurrentTimeline() : nullptr;
    return widget && !pCore->closing ? widget->model() : nullptr;
}

QSet<int> StudioAudioController::currentAudioSelection(QString *error) const
{
    return audioSelection(timeline(), error);
}

QSet<int> StudioAudioController::audioSelection(const std::shared_ptr<TimelineModel> &model, QString *error)
{
    QSet<int> result;
    if (!model) {
        if (error) *error = QStringLiteral("Нет открытой последовательности.");
        return result;
    }
    for (int id : model->getCurrentSelection()) {
        if (!model->isClip(id) || !model->clipIsAudio(id)) continue;
        const auto producer = model->getClipProducer(id);
        const int audioStream = producer ? producer->get_int("audio_index") : -1;
        const QByteArray channelProperty = QStringLiteral("meta.media.%1.codec.channels").arg(audioStream).toLatin1();
        if (audioStream >= 0 && producer->get_int(channelProperty.constData()) > 2) {
            if (error) *error = QStringLiteral("Многоканальный звук пока не поддерживается. Выберите моно- или стереофрагменты.");
            return {};
        }
        const int track = model->getItemTrackId(id);
        if (track < 0 || model->trackIsLocked(track)) {
            if (error) *error = QStringLiteral("Выбранный звук находится на заблокированной дорожке.");
            return {};
        }
        result.insert(id);
    }
    if (result.isEmpty() && error) *error = QStringLiteral("Выберите на таймлайне клип со звуком.");
    return result;
}

QSet<int> StudioAudioController::audioTrackSelection(const std::shared_ptr<TimelineModel> &model, QString *error)
{
    const auto selected = audioSelection(model, error);
    if (selected.isEmpty()) return {};
    QSet<int> tracks;
    for (int id : selected) tracks.insert(model->getItemTrackId(id));
    if (tracks.size() != 1) {
        if (error) *error = QStringLiteral("Для обработки дорожки выберите звук только на одной дорожке.");
        return {};
    }
    QSet<int> result;
    for (int id : model->getItemsInRange(*tracks.cbegin(), 0, -1, false)) {
        if (!model->isClip(id) || !model->clipIsAudio(id)) continue;
        const auto producer = model->getClipProducer(id);
        const int stream = producer ? producer->get_int("audio_index") : -1;
        const QByteArray channels = QStringLiteral("meta.media.%1.codec.channels").arg(stream).toLatin1();
        if (stream >= 0 && producer->get_int(channels.constData()) > 2) {
            if (error) *error = QStringLiteral("На дорожке есть многоканальный клип; поддерживаются моно и стерео.");
            return {};
        }
        result.insert(id);
    }
    if (result.isEmpty() && error) *error = QStringLiteral("На выбранной дорожке нет звука.");
    return result;
}

QString StudioAudioController::selectionSummary(bool wholeTrack) const
{
    QString error;
    const auto ids = wholeTrack ? audioTrackSelection(timeline(), &error) : currentAudioSelection(&error);
    return ids.isEmpty() ? error : wholeTrack ? QStringLiteral("На дорожке аудиофрагментов: %1").arg(ids.size())
                                              : QStringLiteral("Выбрано аудиофрагментов: %1").arg(ids.size());
}

bool StudioAudioController::assignCurrentSelection(bool voice, QString *error)
{
    const auto ids = currentAudioSelection(error);
    if (ids.isEmpty()) return false;
    if (voice) {
        m_voiceIds = ids;
        for (int id : ids) m_musicIds.remove(id);
    } else {
        m_musicIds = ids;
        for (int id : ids) m_voiceIds.remove(id);
    }
    Q_EMIT rolesChanged();
    return true;
}

QString StudioAudioController::roleSummary() const
{
    return QStringLiteral("Голос: %1 · музыка: %2").arg(m_voiceIds.size()).arg(m_musicIds.size());
}

QString StudioAudioController::recoverySummary() const
{
    return recoveryData(timeline()).message;
}

bool StudioAudioController::recover(QString *error)
{
    const auto saved = recoveryData(timeline());
    if (!saved.broken) {
        if (error) *error = QStringLiteral("Выберите результат StudioAudio с пропавшим или повреждённым WAV.");
        return false;
    }
    if (!saved.recoverable) {
        if (error) *error = saved.message;
        return false;
    }
    QString linkedJob;
    if (!studioJobForSources(timeline(), saved.voice | saved.music, &linkedJob, error)) return false;
    if (linkedJob != saved.jobId) {
        if (error) *error = QStringLiteral("Связь результата с исходниками изменена; автоматическое восстановление остановлено.");
        return false;
    }
    return startWithSources(saved.action, saved.settings, saved.voice, saved.music, error);
}

QString StudioAudioController::revisionToken(const std::shared_ptr<TimelineModel> &model, const QSet<int> &ids) const
{
    if (!model || !pCore || !pCore->undoStack()) return {};
    QByteArray data = model->uuid().toString(QUuid::WithoutBraces).toUtf8();
    data += ':' + QByteArray::number(pCore->undoStack()->index()) + ':' + sortedIds(ids).toUtf8();
    for (int id : ids) {
        if (!model->isClip(id)) return {};
        data += ':' + QByteArray::number(model->getItemTrackId(id));
        data += ':' + QByteArray::number(model->getItemPosition(id));
        data += ':' + QByteArray::number(model->getItemPlaytime(id));
    }
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool StudioAudioController::writeStem(const std::shared_ptr<TimelineModel> &model, const QSet<int> &ids,
                                      int startFrame, int endFrame, const QString &path, QString *error) const
{
    if (ids.isEmpty() || endFrame <= startFrame) {
        if (error) *error = QStringLiteral("Пустой диапазон звука.");
        return false;
    }
    Mlt::Profile &profile = pCore->getProjectProfile();
    Mlt::Tractor tractor(profile);
    std::unique_ptr<Mlt::Field> field(tractor.field());
    std::vector<std::unique_ptr<Mlt::Playlist>> playlists;
    int index = 0;
    // ponytail: one MLT track per selected clip; consolidate by source track if very large selections make setup measurable.
    for (int id : ids) {
        const auto producer = model->getClipProducer(id);
        if (!producer || !producer->is_valid()) continue;
        auto playlist = std::make_unique<Mlt::Playlist>(profile);
        playlist->set("hide", 1);
        playlist->insert_at(model->getItemPosition(id) - startFrame, producer.get(), 1);
        tractor.set_track(*playlist, index);
        if (index > 0) {
            Mlt::Transition mix(profile, "mix");
            mix.set("sum", 1);
            mix.set("accepts_blanks", 1);
            mix.set("always_active", 1);
            field->plant_transition(mix, 0, index);
        }
        playlists.push_back(std::move(playlist));
        ++index;
    }
    if (index == 0) {
        if (error) *error = QStringLiteral("Не удалось получить звук выбранных клипов.");
        return false;
    }
    tractor.set_in_and_out(0, endFrame - startFrame - 1);
    if (pCore->currentDoc()) tractor.set("kdenlive:projectroot", pCore->currentDoc()->documentRoot().toUtf8().constData());
    QReadLocker lock(&pCore->xmlMutex);
    Mlt::Consumer consumer(profile, "xml", path.toUtf8().constData());
    consumer.set("terminate_on_pause", 1);
    consumer.set("store", "kdenlive");
    consumer.connect(tractor);
    if (consumer.run() != 0 || !StudioManagedAudio::removeStudioMutes(path)) {
        if (error) *error = QStringLiteral("Не удалось подготовить снимок звука.");
        return false;
    }
    return true;
}

bool StudioAudioController::start(Action action, const Settings &settings, bool wholeTrack, QString *error)
{
    QSet<int> voice;
    QSet<int> music;
    if (action == Action::Music) {
        voice = m_voiceIds;
        music = m_musicIds;
        if (voice.isEmpty() || music.isEmpty()) {
            if (error) *error = QStringLiteral("Сначала назначьте выбранные фрагменты голосом и музыкой.");
            return false;
        }
    } else {
        voice = wholeTrack && (action == Action::Voice || action == Action::Loudness)
            ? audioTrackSelection(timeline(), error) : currentAudioSelection(error);
        if (voice.isEmpty()) return false;
        if (action == Action::Loudness) {
            music = voice;
            voice.clear();
        }
    }
    Settings requested = settings;
    requested.levelTrack = action == Action::Voice || action == Action::Loudness;
    return startWithSources(action, requested, voice, music, error);
}

bool StudioAudioController::startWithSources(Action action, const Settings &settings, const QSet<int> &voice, const QSet<int> &music, QString *error)
{
    if (busy()) {
        if (error) *error = QStringLiteral("Обработка уже выполняется.");
        return false;
    }
    const auto model = timeline();
    if (!model || !pCore->currentDoc() || pCore->currentDoc()->url().isEmpty()) {
        if (error) *error = QStringLiteral("Сначала сохраните проект.");
        return false;
    }
    const QSet<int> all = voice | music;
    if (all.isEmpty()) {
        if (error) *error = QStringLiteral("Не найдены сохранённые исходные фрагменты.");
        return false;
    }
    int startFrame = std::numeric_limits<int>::max();
    int endFrame = 0;
    for (int id : all) {
        if (!model->isClip(id)) {
            if (error) *error = QStringLiteral("Один из сохранённых исходников больше нет в этой последовательности.");
            return false;
        }
        const int track = model->getItemTrackId(id);
        if (track < 0 || model->trackIsLocked(track)) {
            if (error) *error = QStringLiteral("Одна из выбранных дорожек заблокирована.");
            return false;
        }
        startFrame = std::min(startFrame, model->getItemPosition(id));
        endFrame = std::max(endFrame, model->getItemPosition(id) + model->getItemPlaytime(id));
    }
    if (action == Action::Pauses) {
        for (int track : model->getAllTracksIds()) {
            if (model->trackIsLocked(track)) {
                if (error) *error = QStringLiteral("Для сокращения пауз разблокируйте все дорожки последовательности.");
                return false;
            }
        }
        const auto subtitles = model->getSubtitleModel();
        if (subtitles && subtitles->isLocked()) {
            if (error) *error = QStringLiteral("Для сокращения пауз разблокируйте дорожку субтитров.");
            return false;
        }
    }
    QString existingJob;
    if (!studioJobForSources(model, all, &existingJob, error)) return false;
    if (action != Action::Pauses && StudioManagedAudio::findTrack(model, error) == -2) return false;
    if (action == Action::Pauses && !existingJob.isEmpty()) {
        if (error) *error = QStringLiteral("Паузы нельзя сокращать поверх уже обработанного подмикса. Сначала отмените его или выберите исходный диапазон.");
        return false;
    }
    auto job = std::make_unique<Job>();
    job->action = action;
    job->settings = settings;
    job->sourceIds = all;
    job->voiceIds = voice;
    job->musicIds = music;
    job->startFrame = startFrame;
    job->endFrame = endFrame;
    job->jobId = existingJob.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : existingJob;
    job->replacing = !existingJob.isEmpty();
    job->outputId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job->token = revisionToken(model, all);
    const QString root = pCore->currentDoc()->projectDataFolder() + QStringLiteral("/studio-audio");
    if (!QDir().mkpath(root)) {
        if (error) *error = QStringLiteral("Не удалось создать каталог данных звука проекта.");
        return false;
    }
    job->outputDir = QDir(root).absoluteFilePath(job->outputId);
    job->temporary = std::make_unique<QTemporaryDir>(QDir::temp().absoluteFilePath(QStringLiteral("studio-audio-XXXXXX")));
    if (!job->temporary->isValid()) {
        if (error) *error = QStringLiteral("Не удалось создать временный каталог.");
        return false;
    }
    QString segmentFile;
    if (settings.levelTrack) {
        if (action != Action::Voice && action != Action::Loudness) {
            if (error) *error = QStringLiteral("Выравнивание всей дорожки доступно для голоса и громкости.");
            return false;
        }
        QVector<QPair<int, int>> ranges;
        for (int id : all) ranges.push_back({model->getItemPosition(id) - startFrame,
                                             model->getItemPosition(id) + model->getItemPlaytime(id) - startFrame});
        std::sort(ranges.begin(), ranges.end());
        QVector<QPair<int, int>> groups;
        for (const auto &range : ranges) {
            if (!groups.isEmpty() && range.first < groups.back().second)
                groups.back().second = std::max(groups.back().second, range.second);
            else groups.push_back(range);
        }
        const double fps = pCore->getCurrentFps();
        if (!std::isfinite(fps) || fps <= 0) {
            if (error) *error = QStringLiteral("Неверная частота кадров проекта.");
            return false;
        }
        segmentFile = job->temporary->filePath(QStringLiteral("level-segments.txt"));
        QSaveFile file(segmentFile);
        if (!file.open(QIODevice::WriteOnly)) {
            if (error) *error = QStringLiteral("Не удалось сохранить границы аудиоклипов.");
            return false;
        }
        for (const auto &group : groups) {
            const QByteArray line = QByteArray::number(group.first / fps, 'g', 17) + ' '
                + QByteArray::number(group.second / fps, 'g', 17) + '\n';
            if (file.write(line) != line.size()) {
                if (error) *error = QStringLiteral("Не удалось записать границы аудиоклипов.");
                return false;
            }
        }
        if (!file.commit()) {
            if (error) *error = QStringLiteral("Не удалось сохранить границы аудиоклипов.");
            return false;
        }
    }
    const QString melt = StudioResources::executable(QStringLiteral("melt"));
    const QString worker = StudioResources::executable(QStringLiteral("studio-audio"));
    if (melt.isEmpty() || worker.isEmpty()) {
        if (error) *error = QStringLiteral("В этой сборке нет обработчика звука или MLT.");
        return false;
    }
    auto addStem = [&](const QSet<int> &ids, const QString &name) {
        if (ids.isEmpty()) return true;
        const QString xml = job->temporary->filePath(name + QStringLiteral(".mlt"));
        const QString wav = job->temporary->filePath(name + QStringLiteral(".wav"));
        if (!writeStem(model, ids, startFrame, endFrame, xml, error)) return false;
        job->commands.push_back({melt, {xml, QStringLiteral("-consumer"), QStringLiteral("avformat:%1").arg(wav),
                                        QStringLiteral("vn=1"), QStringLiteral("acodec=pcm_f32le"), QStringLiteral("ar=48000"),
                                        QStringLiteral("ac=2"), QStringLiteral("f=wav"), QStringLiteral("threads=2"), QStringLiteral("real_time=-1")},
                                 QStringLiteral("Подготовка выбранного звука")});
        return true;
    };
    if (!addStem(voice, QStringLiteral("voice")) || !addStem(music, QStringLiteral("music"))) return false;
    QString mode = QStringLiteral("voice");
    if (action == Action::Loudness) mode = QStringLiteral("music");
    else if (action == Action::Music) mode = QStringLiteral("mix");
    else if (action == Action::Pauses) mode = QStringLiteral("pauses");
    QStringList args{QStringLiteral("--mode"), mode, QStringLiteral("--out-dir"), job->outputDir,
                     QStringLiteral("--snapshot-token"), job->token, QStringLiteral("--strength"), QString::number(settings.strength),
                     QStringLiteral("--presence"), QString::number(settings.presence), QStringLiteral("--priority"), QString::number(settings.priority),
                     QStringLiteral("--noise"), settings.noise, QStringLiteral("--normalize"), QStringLiteral("on"),
                     QStringLiteral("--target"), QString::number(settings.targetLufs), QStringLiteral("--true-peak"), QStringLiteral("-1.5")};
    if (!voice.isEmpty()) args << QStringLiteral("--voice") << job->temporary->filePath(QStringLiteral("voice.wav"));
    if (!music.isEmpty()) args << QStringLiteral("--music") << job->temporary->filePath(QStringLiteral("music.wav"));
    if (!segmentFile.isEmpty()) args << QStringLiteral("--level-segments") << segmentFile;
    job->commands.push_back({worker, args, action == Action::Pauses ? QStringLiteral("Поиск длинных пауз") : QStringLiteral("Обработка звука")});
    m_job = std::move(job);
    m_job->revisionConnection = connect(pCore->undoStack().get(), &QUndoStack::indexChanged, this, [this] {
        if (m_job) m_job->invalidated = true;
    });
    Q_EMIT busyChanged(true);
    Q_EMIT progressChanged(0);
    runNext();
    return true;
}

void StudioAudioController::runNext()
{
    if (!m_job) return;
    if (m_job->commands.isEmpty()) {
        QString error;
        if (!applyResult(&error)) fail(error);
        else if (m_job && m_job->action == Action::Pauses) finish(QStringLiteral("Паузы сокращены."));
        return;
    }
    const auto command = m_job->commands.takeFirst();
    m_processOutput.clear();
    m_progressOutput.clear();
    Q_EMIT statusChanged(command.label, false);
    m_process->setProgram(command.program);
    m_process->setArguments(command.arguments);
    StudioJobs::start(m_process);
}

void StudioAudioController::processFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_job) return;
    if (m_canceling) {
        discardOutput();
        finish(QStringLiteral("Обработка отменена; проект не изменён."));
        Q_EMIT progressChanged(0);
        return;
    }
    if (m_process->property("_sunimoTimedOut").toBool()) {
        fail(QStringLiteral("Превышен лимит обработки звука: 30 минут; проект не изменён."));
        return;
    }
    if (status != QProcess::NormalExit || exitCode != 0) {
        fail(QStringLiteral("Обработка звука завершилась с ошибкой: %1").arg(QString::fromUtf8(m_processOutput.right(1200))));
        return;
    }
    runNext();
}

bool StudioAudioController::applyResult(QString *error)
{
    if (!m_job) return false;
    const auto model = timeline();
    if (!model || m_job->invalidated || revisionToken(model, m_job->sourceIds) != m_job->token) {
        if (error) *error = QStringLiteral("Проект изменился во время анализа; результат не применён.");
        return false;
    }
    QFile file(QDir(m_job->outputDir).absoluteFilePath(QStringLiteral("report.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Обработчик не создал отчёт.");
        return false;
    }
    QJsonParseError parseError;
    const auto report = QJsonDocument::fromJson(file.readAll(), &parseError).object();
    const QString expectedMode = m_job->action == Action::Voice ? QStringLiteral("voice")
        : m_job->action == Action::Music ? QStringLiteral("mix")
        : m_job->action == Action::Pauses ? QStringLiteral("pauses") : QStringLiteral("music");
    if (parseError.error != QJsonParseError::NoError || report.value(QStringLiteral("schema")).toInt() != 1
        || report.value(QStringLiteral("module")).toString() != QLatin1String("StudioAudio")
        || report.value(QStringLiteral("version")).toString() != QLatin1String("0.2.0")
        || report.value(QStringLiteral("snapshot_token")).toString() != m_job->token
        || report.value(QStringLiteral("mode")).toString() != expectedMode) {
        if (error) *error = QStringLiteral("Отчёт обработчика повреждён или относится к другому снимку проекта.");
        return false;
    }
    if (m_job->action != Action::Pauses) {
        const QString audio = QDir(m_job->outputDir).absoluteFilePath(QStringLiteral("result.wav"));
        if (report.value(QStringLiteral("sample_rate")).toInt() != 48000 || report.value(QStringLiteral("channels")).toInt() != 2
            || fileSha256(audio).compare(report.value(QStringLiteral("audio_sha256")).toString(), Qt::CaseInsensitive) != 0) {
            if (error) *error = QStringLiteral("Готовый WAV повреждён или имеет неподдерживаемый формат.");
            return false;
        }
    }
    return m_job->action == Action::Pauses ? applyPauseCuts(report, error) : applyAudioFile(report, error);
}

bool StudioAudioController::applyPauseCuts(const QJsonObject &report, QString *error)
{
    const auto model = timeline();
    QVector<int> tracks;
    for (int track : model->getAllTracksIds()) {
        if (model->trackIsLocked(track)) {
            if (error) *error = QStringLiteral("Дорожка была заблокирована до применения; монтаж не изменён.");
            return false;
        }
        tracks << track;
    }
    QVector<QPoint> cuts;
    if (!parsePauseCuts(report.value(QStringLiteral("pause_cuts")).toArray(), m_job->startFrame, m_job->endFrame, pCore->getCurrentFps(), cuts)) {
        if (error) *error = QStringLiteral("Отчёт содержит некорректные или пересекающиеся паузы; проект не изменён.");
        return false;
    }
    if (cuts.isEmpty()) {
        if (error) *error = QStringLiteral("Длинные внутренние паузы не найдены; проект не изменён.");
        return false;
    }
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    for (const auto &cut : cuts) {
        if (!TimelineFunctions::extractZoneWithUndo(model, tracks, cut, false, -1, {}, undo, redo)) {
            undo();
            if (error) *error = QStringLiteral("Не удалось безопасно сократить паузы; изменения отменены.");
            return false;
        }
        if (!adjustSubtitlesForCut(model, cut, undo, redo, error)) {
            undo();
            return false;
        }
        const auto guides = model->getGuideModel();
        const auto remove = guides->getMarkersInRange(cut.x(), cut.y());
        for (const auto &guide : remove) {
            if (!guides->removeMarker(guide.time(), undo, redo)) {
                undo();
                if (error) *error = QStringLiteral("Не удалось синхронизировать маркеры; изменения отменены.");
                return false;
            }
        }
        const auto move = guides->getMarkersInRange(cut.y(), -1);
        if (!move.isEmpty() && !guides->moveMarkers(move, GenTime(cut.y(), pCore->getCurrentFps()), GenTime(cut.x(), pCore->getCurrentFps()), undo, redo)) {
            undo();
            if (error) *error = QStringLiteral("Не удалось сдвинуть маркеры; изменения отменены.");
            return false;
        }
    }
    pCore->pushUndo(undo, redo, QStringLiteral("Сократить паузы студии"));
    QDir(m_job->outputDir).removeRecursively();
    return true;
}

bool StudioAudioController::applyAudioFile(const QJsonObject &report, QString *error)
{
    const auto model = timeline();
    const QString audio = QDir(m_job->outputDir).absoluteFilePath(QStringLiteral("result.wav"));
    if (!QFileInfo(audio).isFile()) {
        if (error) *error = QStringLiteral("Готовый WAV отсутствует.");
        return false;
    }
    const QString reportPath = QDir(m_job->outputDir).absoluteFilePath(QStringLiteral("report.json"));
    const auto properties = resultProperties(audio, reportPath, report.value(QStringLiteral("audio_sha256")).toString(), m_job->jobId,
                                             m_job->action, m_job->settings, m_job->sourceIds, m_job->voiceIds, m_job->musicIds);
    QStringList warnings;
    for (const auto &value : report.value(QStringLiteral("warnings")).toArray()) warnings << value.toString();
    const QString done = warnings.isEmpty() ? QStringLiteral("Звук обработан.")
                                             : QStringLiteral("Звук обработан. %1").arg(warnings.join(QLatin1Char(' ')));
    if (m_job->replacing) {
        const int resultId = resultClipForJob(model, m_job->jobId);
        if (resultId < 0) {
            if (error) *error = resultId == -2 ? QStringLiteral("Найдено несколько результатов StudioAudio для одних исходников; обновление остановлено.")
                                               : QStringLiteral("Связанный результат StudioAudio не найден.");
            return false;
        }
        const QString binId = model->getClipBinId(resultId);
        const auto clip = pCore->projectItemModel()->getClipByBinID(binId);
        if (!clip) {
            if (error) *error = QStringLiteral("Не удалось найти служебный WAV в корзине проекта.");
            return false;
        }
        QMap<QString, QString> previous;
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) previous.insert(it.key(), clip->getProducerProperty(it.key()));
        previous = resolveAudioAssetProperties(previous, true);
        Fun undo = [clip, previous]() { clip->setProperties(resolveAudioAssetProperties(previous), true); return true; };
        Fun redo = [clip, properties]() { clip->setProperties(resolveAudioAssetProperties(properties), true); return true; };
        if (!redo()) {
            if (error) *error = QStringLiteral("Не удалось обновить служебный WAV.");
            return false;
        }
        const QString sourceText = sortedIds(m_job->sourceIds);
        for (int sourceId : m_job->sourceIds) {
            auto mute = StudioManagedAudio::ensureMute(model->getClipEffectStack(sourceId), m_job->jobId, false, undo, redo, error);
            if (!mute) { undo(); return false; }
            appendParameterChange(mute, {QStringLiteral("level")}, {QStringLiteral("-100")}, undo, redo);
            appendParameterChange(mute, {QStringLiteral("studio:audio:sources"), QStringLiteral("studio:audio:source-role"),
                                         QStringLiteral("studio:audio:source-count")},
                                  {sourceText, m_job->voiceIds.contains(sourceId) ? QStringLiteral("voice") : QStringLiteral("music"),
                                   QString::number(m_job->sourceIds.size())}, undo, redo);
        }
        resolveAudioAssetProperties(properties, true);
        pCore->pushUndo(undo, redo, QStringLiteral("Обновить обработанный звук"));
        finish(done);
        return true;
    }
    auto undo = std::make_shared<Fun>([]() { return true; });
    auto redo = std::make_shared<Fun>([]() { return true; });
    auto rollback = std::make_shared<Fun>([undo] { return (*undo)(); });
    m_job->rollbackImport = rollback;
    const auto revert = [rollback] {
        if (*rollback) { Fun action = std::exchange(*rollback, Fun{}); action(); }
    };
    auto jobId = m_job->jobId;
    auto token = m_job->token;
    auto ids = m_job->sourceIds;
    auto voiceIds = m_job->voiceIds;
    auto startFrame = m_job->startFrame;
    const QPointer<StudioAudioController> owner(this);
    const QString outputId = m_job->outputId;
    auto callback = [this, owner, outputId, model, undo, redo, revert, rollback, jobId, token, ids, voiceIds, startFrame, properties, done](const QString &binId) {
        if (!owner || !m_job || m_job->outputId != outputId) {
            revert();
            return;
        }
        if (m_job->invalidated || timeline() != model || revisionToken(model, ids) != token) {
            revert();
            fail(QStringLiteral("Проект изменился до подключения результата; изменения отменены."));
            return;
        }
        const auto binModel = pCore->projectItemModel();
        const auto binClip = binModel->getClipByBinID(binId);
        if (!binClip) {
            revert();
            fail(QStringLiteral("Не удалось сохранить связь результата с исходниками."));
            return;
        }
        auto metadata = properties;
        metadata.remove(QStringLiteral("resource"));
        binClip->setProperties(resolveAudioAssetProperties(metadata));
        const Fun metadataRedo = *redo;
        *redo = [binModel, binId, properties, metadataRedo]() {
            if (!metadataRedo()) return false;
            const auto clip = binModel->getClipByBinID(binId);
            if (!clip) return false;
            auto resolved = resolveAudioAssetProperties(properties);
            if (resolved.value(QStringLiteral("resource")) == clip->getProducerProperty(QStringLiteral("resource")))
                resolved.remove(QStringLiteral("resource"));
            clip->setProperties(resolved, true);
            return true;
        };
        QString ownershipError;
        const int trackId = StudioManagedAudio::ensureTrack(model, *undo, *redo, &ownershipError);
        if (trackId < 0) {
            revert();
            fail(ownershipError);
            return;
        }
        int resultId = -1;
        if (!model->requestClipInsertion(QStringLiteral("A%1").arg(binId), trackId, startFrame, resultId, false, true, false, *undo, *redo)) {
            revert();
            fail(QStringLiteral("Не удалось добавить обработанный звук."));
            return;
        }
        const QString sourceText = sortedIds(ids);
        auto resultProducer = model->getClipProducer(resultId);
        const auto resolvedMetadata = resolveAudioAssetProperties(metadata);
        for (auto it = resolvedMetadata.constBegin(); it != resolvedMetadata.constEnd(); ++it)
            resultProducer->set(it.key().toUtf8().constData(), it.value().toUtf8().constData());
        const Fun previousRedo = *redo;
        *redo = [model, resultId, previousRedo, metadata]() {
            if (!previousRedo()) return false;
            const auto producer = model->getClipProducer(resultId);
            if (!producer) return false;
            const auto resolved = resolveAudioAssetProperties(metadata);
            for (auto it = resolved.constBegin(); it != resolved.constEnd(); ++it)
                producer->set(it.key().toUtf8().constData(), it.value().toUtf8().constData());
            return true;
        };
        for (int sourceId : ids) {
            auto stack = model->getClipEffectStack(sourceId);
            QString muteError;
            auto mute = StudioManagedAudio::ensureMute(stack, jobId, true, *undo, *redo, &muteError);
            if (!mute) {
                revert();
                fail(muteError);
                return;
            }
            const QString sourceRole = voiceIds.contains(sourceId) ? QStringLiteral("voice") : QStringLiteral("music");
            appendParameterChange(mute, {QStringLiteral("level"), QStringLiteral("studio:audio:id"), QStringLiteral("studio:audio:role"),
                                         QStringLiteral("studio:audio:sources"), QStringLiteral("studio:audio:source-role"),
                                         QStringLiteral("studio:audio:source-count")},
                                  {QStringLiteral("-100"), jobId, QStringLiteral("source-mute"), sourceText, sourceRole, QString::number(ids.size())}, *undo, *redo);
        }
        *rollback = {};
        resolveAudioAssetProperties(properties, true);
        pCore->pushUndo(*undo, *redo, QStringLiteral("Обработать звук в монтажной студии"));
        finish(done);
    };
    auto root = pCore->projectItemModel()->getRootFolder();
    const QString binId = ClipCreator::createClipFromFile(audio, root->clipId(), pCore->projectItemModel(), *undo, *redo, callback, false);
    if (binId == QLatin1String("-1")) {
        revert();
        if (error) *error = QStringLiteral("Не удалось добавить WAV в проект.");
        return false;
    }
    return true;
}

void StudioAudioController::cancel()
{
    if (!m_job || m_canceling) return;
    m_canceling = true;
    m_job->commands.clear();
    if (StudioJobs::busy(m_process)) {
        StudioJobs::cancel(m_process);
        const QString outputId = m_job->outputId;
        QTimer::singleShot(500, this, [this, outputId] {
            if (m_job && m_job->outputId == outputId && m_canceling && m_process->state() != QProcess::NotRunning) m_process->kill();
        });
        return;
    }
    discardOutput();
    finish(QStringLiteral("Обработка отменена; проект не изменён."));
    Q_EMIT progressChanged(0);
}

void StudioAudioController::selectionChanged()
{
    if (busy()) cancel();
}

bool StudioAudioController::busy() const
{
    return bool(m_job);
}

void StudioAudioController::discardOutput()
{
    if (m_job && m_job->rollbackImport && *m_job->rollbackImport) {
        Fun rollback = std::exchange(*m_job->rollbackImport, Fun{});
        rollback();
    }
    if (!m_job || m_job->outputDir.isEmpty() || !pCore || !pCore->currentDoc()) return;
    const QString root = QDir(pCore->currentDoc()->projectDataFolder() + QStringLiteral("/studio-audio")).absolutePath();
    const QFileInfo target(m_job->outputDir);
    if (target.absolutePath() == root && target.fileName() == m_job->outputId) QDir(m_job->outputDir).removeRecursively();
}

void StudioAudioController::fail(const QString &message)
{
    discardOutput();
    m_job.reset();
    m_canceling = false;
    Q_EMIT busyChanged(false);
    Q_EMIT progressChanged(0);
    Q_EMIT statusChanged(message, true);
}

void StudioAudioController::finish(const QString &message)
{
    m_job.reset();
    m_canceling = false;
    Q_EMIT busyChanged(false);
    Q_EMIT progressChanged(100);
    Q_EMIT statusChanged(message, false);
}

StudioAudioPage::StudioAudioPage(StudioAudioController *controller, QWidget *parent)
    : QWidget(parent)
    , m_controller(controller)
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    QFile contract(StudioResources::dataFile(QStringLiteral("studio-audio/parameters.json")));
    if (contract.open(QIODevice::ReadOnly)) m_schema = QJsonDocument::fromJson(contract.readAll()).object();
    QMap<QString, QJsonObject> parameters;
    for (const auto &value : m_schema.value(QStringLiteral("parameters")).toArray()) {
        const auto spec = value.toObject();
        parameters.insert(spec.value(QStringLiteral("id")).toString(), spec);
    }
    m_actions = new QTabBar(this);
    m_actions->setExpanding(true);
    m_actions->setUsesScrollButtons(true);
    for (const auto &value : m_schema.value(QStringLiteral("actions")).toArray())
        m_actions->addTab(value.toObject().value(QStringLiteral("label")).toString());
    layout->addWidget(m_actions);
    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);
    m_presets = new QTabBar(this);
    m_presets->addTab(QStringLiteral("Готовые"));
    m_presets->addTab(QStringLiteral("Мои"));
    layout->addWidget(m_presets);
    m_preset = new QComboBox(this);
    layout->addWidget(m_preset);
    m_scope = new QComboBox(this);
    m_scope->addItem(QStringLiteral("Только выбранные клипы"));
    m_scope->addItem(QStringLiteral("Вся выбранная дорожка"));
    layout->addWidget(m_scope);
    m_savePreset = new QPushButton(QStringLiteral("Сохранить текущие настройки"), this);
    m_savePreset->hide();
    layout->addWidget(m_savePreset);
    m_voiceOptions = new QWidget(this);
    auto voiceLayout = new QVBoxLayout(m_voiceOptions);
    voiceLayout->setContentsMargins(0, 0, 0, 0);
    voiceLayout->addWidget(sliderRow(parameters.value(QStringLiteral("strength")), &m_strength, &m_strengthValue, m_voiceOptions));
    auto noiseLabel = new QLabel(QStringLiteral("Шумоподавление"), m_voiceOptions);
    m_noise = new QComboBox(m_voiceOptions);
    m_noise->setObjectName(QStringLiteral("studioNoise"));
    noiseLabel->setBuddy(m_noise);
    voiceLayout->addWidget(noiseLabel);
    for (const auto &value : m_schema.value(QStringLiteral("noise")).toArray()) {
        const auto option = value.toObject();
        const QString id = option.value(QStringLiteral("id")).toString();
        if (id == QLatin1String("rnnoise")) continue;
        m_noise->addItem(option.value(QStringLiteral("label")).toString(), id);
    }
    voiceLayout->addWidget(m_noise);
    layout->addWidget(m_voiceOptions);
    m_loudnessOptions = new QWidget(this);
    auto loudnessLayout = new QVBoxLayout(m_loudnessOptions);
    loudnessLayout->setContentsMargins(0, 0, 0, 0);
    loudnessLayout->addWidget(new QLabel(QStringLiteral("Готовая громкость"), m_loudnessOptions));
    m_target = new QComboBox(m_loudnessOptions);
    for (const auto &value : m_schema.value(QStringLiteral("targets")).toArray()) {
        const auto target = value.toObject();
        m_target->addItem(QStringLiteral("%1: %2 LUFS").arg(target.value(QStringLiteral("label")).toString())
                              .arg(target.value(QStringLiteral("lufs")).toDouble()), target.value(QStringLiteral("lufs")).toDouble());
    }
    loudnessLayout->addWidget(m_target, 1);
    layout->addWidget(m_loudnessOptions);
    m_musicOptions = new QWidget(this);
    auto musicLayout = new QVBoxLayout(m_musicOptions);
    musicLayout->setContentsMargins(0, 0, 0, 0);
    auto roles = new QVBoxLayout;
    m_voiceRole = new QPushButton(QStringLiteral("Выбранное: голос"), m_musicOptions);
    m_musicRole = new QPushButton(QStringLiteral("Выбранное: музыка"), m_musicOptions);
    roles->addWidget(m_voiceRole);
    roles->addWidget(m_musicRole);
    musicLayout->addLayout(roles);
    m_roles = new QLabel(m_musicOptions);
    musicLayout->addWidget(m_roles);
    musicLayout->addWidget(sliderRow(parameters.value(QStringLiteral("presence")), &m_presence, &m_presenceValue, m_musicOptions));
    musicLayout->addWidget(sliderRow(parameters.value(QStringLiteral("priority")), &m_priority, &m_priorityValue, m_musicOptions));
    layout->addWidget(m_musicOptions);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_recover = new QPushButton(QStringLiteral("Обработать заново"), this);
    m_recover->hide();
    layout->addWidget(m_recover);
    m_progress = new QProgressBar(this);
    m_progress->hide();
    layout->addWidget(m_progress);
    m_main = new QPushButton(this);
    m_main->setProperty("studioPrimary", true);
    layout->addWidget(m_main);
    m_cancel = new QPushButton(QStringLiteral("Отмена"), this);
    m_cancel->hide();
    layout->addWidget(m_cancel);
    // Keep the same task -> target -> action -> presets -> settings order as the main panel.
    auto target = new QLabel(this);
    target->setObjectName(QStringLiteral("studioTarget"));
    target->setWordWrap(true);
    target->setText(m_controller->selectionSummary());
    layout->insertWidget(2, target);
    layout->removeWidget(m_scope);
    layout->insertWidget(3, m_scope);
    int actionRow = 4;
    for (QWidget *widget : QList<QWidget *>{m_status, m_recover, m_progress, m_main, m_cancel}) {
        layout->removeWidget(widget);
        layout->insertWidget(actionRow++, widget);
    }
    connect(m_actions, &QTabBar::currentChanged, this, &StudioAudioPage::updateMode);
    connect(m_presets, &QTabBar::currentChanged, this, &StudioAudioPage::updateMode);
    connect(m_preset, &QComboBox::currentIndexChanged, this, &StudioAudioPage::applyPreset);
    connect(m_savePreset, &QPushButton::clicked, this, &StudioAudioPage::savePreset);
    connect(m_voiceRole, &QPushButton::clicked, this, [this] { QString error; if (!m_controller->assignCurrentSelection(true, &error)) m_status->setText(error); });
    connect(m_musicRole, &QPushButton::clicked, this, [this] { QString error; if (!m_controller->assignCurrentSelection(false, &error)) m_status->setText(error); });
    connect(m_main, &QPushButton::clicked, this, [this] { QString error; if (!m_controller->start(action(), settings(), m_scope->isVisible() && m_scope->currentIndex() == 1, &error)) m_status->setText(error); });
    connect(m_scope, &QComboBox::currentIndexChanged, this, &StudioAudioPage::refreshSelection);
    connect(m_recover, &QPushButton::clicked, this, [this] { QString error; if (!m_controller->recover(&error)) m_status->setText(error); });
    connect(m_cancel, &QPushButton::clicked, m_controller, &StudioAudioController::cancel);
    connect(m_controller, &StudioAudioController::rolesChanged, this, [this] { m_roles->setText(m_controller->roleSummary()); });
    connect(m_controller, &StudioAudioController::statusChanged, this, [this](const QString &text, bool error) {
        m_status->setText(text);
        m_status->setStyleSheet(error ? QStringLiteral("color: palette(bright-text);") : QString());
    });
    connect(m_controller, &StudioAudioController::progressChanged, m_progress, &QProgressBar::setValue);
    connect(m_controller, &StudioAudioController::busyChanged, this, [this](bool busy) {
        m_progress->setVisible(busy);
        m_cancel->setVisible(busy);
        m_main->setEnabled(!busy && m_schemaValid);
        m_recover->setEnabled(!busy);
        m_actions->setEnabled(!busy);
        m_scope->setEnabled(!busy);
    });
    m_schemaValid = m_actions->count() == 4 && parameters.size() == 3 && m_target->count() > 0 && m_noise->count() == 2
        && m_schema.value(QStringLiteral("version")).toString() == QString::fromLatin1(studio_audio::defaults::result_version);
    m_main->setEnabled(m_schemaValid);
    updateMode();
}

StudioAudioController::Action StudioAudioPage::action() const
{
    return static_cast<StudioAudioController::Action>(m_actions->currentIndex());
}

StudioAudioController::Settings StudioAudioPage::settings() const
{
    StudioAudioController::Settings result;
    result.strength = m_strength->value();
    result.presence = m_presence->value();
    result.priority = m_priority->value();
    result.targetLufs = m_target->currentData().toDouble();
    result.noise = m_noise->currentData().toString();
    return result;
}

void StudioAudioPage::updateMode()
{
    const auto current = action();
    m_voiceOptions->setVisible(current == StudioAudioController::Action::Voice);
    m_loudnessOptions->setVisible(current == StudioAudioController::Action::Loudness);
    m_musicOptions->setVisible(current == StudioAudioController::Action::Music);
    m_scope->setVisible(current == StudioAudioController::Action::Voice || current == StudioAudioController::Action::Loudness);
    m_savePreset->setVisible(m_presets->currentIndex() == 1);
    const auto actions = m_schema.value(QStringLiteral("actions")).toArray();
    const auto actionSpec = actions.isEmpty() ? QJsonObject() : actions.at(qBound(0, int(current), int(actions.size()) - 1)).toObject();
    m_main->setText(actionSpec.value(QStringLiteral("button")).toString());
    if (current == StudioAudioController::Action::Voice) {
        m_hint->setText(QStringLiteral("Очищает и выравнивает выбранный голос без технических ручек."));
    } else if (current == StudioAudioController::Action::Loudness) {
        m_hint->setText(QStringLiteral("Выравнивает громкость клипов выбранной дорожки и проверяет общий уровень."));
    } else if (current == StudioAudioController::Action::Music) {
        m_hint->setText(QStringLiteral("Сначала назначьте голос и музыку, затем примените баланс."));
    } else {
        m_hint->setText(QStringLiteral("Сокращает внутренние паузы длиннее 650 мс до безопасных 300 мс."));
    }
    QSignalBlocker presetSignals(m_preset);
    m_preset->clear();
    if (m_presets->currentIndex() == 1) {
        const auto saved = QJsonDocument::fromJson(QSettings().value(QStringLiteral("StudioAudio/presets")).toByteArray()).array();
        for (const auto &value : saved) {
            const auto preset = value.toObject();
            if (preset.value(QStringLiteral("action")).toInt() == int(current))
                m_preset->addItem(preset.value(QStringLiteral("name")).toString(), QJsonDocument(preset).toJson(QJsonDocument::Compact));
        }
    } else if (current == StudioAudioController::Action::Loudness) {
        for (const auto &value : m_schema.value(QStringLiteral("targets")).toArray()) {
            const auto target = value.toObject();
            m_preset->addItem(target.value(QStringLiteral("label")).toString(),
                QJsonDocument(QJsonObject{{QStringLiteral("target"), target.value(QStringLiteral("lufs"))}}).toJson(QJsonDocument::Compact));
        }
    } else {
        for (const auto &value : m_schema.value(QStringLiteral("presets")).toArray()) {
            const auto preset = value.toObject();
            if (preset.value(QStringLiteral("mode")) == actionSpec.value(QStringLiteral("worker_mode")))
                m_preset->addItem(preset.value(QStringLiteral("label")).toString(), QJsonDocument(preset).toJson(QJsonDocument::Compact));
        }
    }
    presetSignals.unblock();
    applyPreset(m_preset->currentIndex());
    m_roles->setText(m_controller->roleSummary());
    refreshSelection();
}

void StudioAudioPage::applyPreset(int index)
{
    if (index < 0) return;
    const auto preset = QJsonDocument::fromJson(m_preset->itemData(index).toByteArray()).object();
    const StudioAudioController::Settings defaults;
    if (action() == StudioAudioController::Action::Voice)
        m_strength->setValue(preset.value(QStringLiteral("strength")).toInt(defaults.strength));
    const int noiseIndex = m_noise->findData(preset.value(QStringLiteral("noise")).toString(defaults.noise));
    m_noise->setCurrentIndex(noiseIndex < 0 ? 0 : noiseIndex);
    if (action() == StudioAudioController::Action::Music) {
        m_presence->setValue(preset.value(QStringLiteral("presence")).toInt(defaults.presence));
        m_priority->setValue(preset.value(QStringLiteral("priority")).toInt(defaults.priority));
    }
    if (preset.contains(QStringLiteral("target_lufs"))) m_target->setCurrentIndex(m_target->findData(preset.value(QStringLiteral("target_lufs")).toDouble()));
    if (preset.contains(QStringLiteral("target"))) m_target->setCurrentIndex(m_target->findData(preset.value(QStringLiteral("target")).toDouble()));
}

void StudioAudioPage::savePreset()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Мой шаблон"), QStringLiteral("Название"), QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    auto document = QJsonDocument::fromJson(QSettings().value(QStringLiteral("StudioAudio/presets")).toByteArray());
    QJsonArray saved = document.isArray() ? document.array() : QJsonArray();
    const auto current = settings();
    QJsonObject preset{{QStringLiteral("name"), name}, {QStringLiteral("action"), int(action())},
                       {QStringLiteral("strength"), current.strength}, {QStringLiteral("presence"), current.presence},
                       {QStringLiteral("priority"), current.priority}, {QStringLiteral("target"), current.targetLufs},
                       {QStringLiteral("noise"), current.noise}};
    for (int i = saved.size() - 1; i >= 0; --i) {
        const auto previous = saved[i].toObject();
        if (previous.value(QStringLiteral("action")).toInt() == int(action()) && previous.value(QStringLiteral("name")).toString() == name)
            saved.removeAt(i);
    }
    saved.append(preset);
    QSettings().setValue(QStringLiteral("StudioAudio/presets"), QJsonDocument(saved).toJson(QJsonDocument::Compact));
    updateMode();
    m_preset->setCurrentIndex(m_preset->findText(name));
}

void StudioAudioPage::activate()
{
    refreshSelection();
}

void StudioAudioPage::deactivate()
{
    m_controller->cancel();
}

void StudioAudioPage::refreshSelection()
{
    if (!m_schemaValid) {
        m_status->setText(QStringLiteral("Описание настроек звука отсутствует или повреждено. Переустановите пакет студии."));
        m_recover->hide();
        return;
    }
    const bool wholeTrack = m_scope->isVisible() && m_scope->currentIndex() == 1;
    if (auto target = findChild<QLabel *>(QStringLiteral("studioTarget"))) target->setText(m_controller->selectionSummary(wholeTrack));
    const QString recovery = m_controller->recoverySummary();
    m_recover->setVisible(!recovery.isEmpty());
    if (!m_controller->busy()) m_status->setText(recovery.isEmpty() ? m_controller->selectionSummary(wholeTrack) : recovery);
}
