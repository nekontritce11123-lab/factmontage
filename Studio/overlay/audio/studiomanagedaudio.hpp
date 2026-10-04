// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../studiohelpers.hpp"
#include "../studiojobs.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "timeline2/model/timelineitemmodel.hpp"
#include "macros.hpp"
#include <QDomDocument>
#include <QFile>
#include <QSaveFile>

namespace StudioManagedAudio {
// Keep user volume effects; only this application's source mute is absent from analysis audio.
inline bool removeStudioMutes(const QString &path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return false;
    QDomDocument document;
    if (!document.setContent(&input)) return false;
    input.close();
    KdenliveDoc::useOriginals(document);
    const auto filters = document.elementsByTagName(QStringLiteral("filter"));
    QList<QDomNode> remove;
    for (int i = 0; i < filters.count(); ++i) {
        const auto filter = filters.at(i).toElement();
        const auto properties = filter.elementsByTagName(QStringLiteral("property"));
        for (int p = 0; p < properties.count(); ++p) {
            const auto property = properties.at(p).toElement();
            if (property.attribute(QStringLiteral("name")) == QLatin1String("studio:audio:role")
                && property.text() == QLatin1String("source-mute")) {
                remove << filter;
                break;
            }
        }
    }
    for (const auto &node : remove) node.parentNode().removeChild(node);
    QSaveFile output(path);
    const QByteArray xml = document.toByteArray(2);
    if (!output.open(QIODevice::WriteOnly) || output.write(xml) != xml.size()) return false;
    return output.commit();
}

// Never identify an owned track by its translated/user-editable display name.
inline int findTrack(const std::shared_ptr<TimelineItemModel> &model, QString *error,
                     const QString &marker = QStringLiteral("kdenlive:studio_audio_track"))
{
    int found = -1;
    for (int id : model->getTracksIds(true)) {
        bool owned = model->getTrackProperty(id, marker).toInt() == 1;
        if (!owned && marker == QLatin1String("kdenlive:studio_audio_track")) for (int item : model->getItemsInRange(id, 0, -1, false)) {
            if (!model->isClip(item)) continue;
            const auto clip = pCore->projectItemModel()->getClipByBinID(model->getClipBinId(item));
            if (clip && clip->getProducerProperty(QStringLiteral("studio:audio:role")) == QLatin1String("result")
                && !clip->getProducerProperty(QStringLiteral("studio:audio:id")).isEmpty()) { owned = true; break; }
        }
        if (!owned) continue;
        if (found >= 0) {
            if (error) *error = QStringLiteral("Найдено несколько служебных аудиодорожек. Новая не будет создана. Сначала объедините результаты: автоматическое удаление может затронуть ваши правки.");
            return -2;
        }
        found = id;
    }
    if (found >= 0 && model->trackIsLocked(found)) {
        if (error) *error = QStringLiteral("Служебная аудиодорожка заблокирована. Разблокируйте её; новая дорожка не создаётся.");
        return -2;
    }
    return found;
}

inline int ensureTrack(const std::shared_ptr<TimelineItemModel> &model, Fun &undo, Fun &redo, QString *error,
                       const QString &marker = QStringLiteral("kdenlive:studio_audio_track"),
                       const QString &title = QStringLiteral("FactMontage: Звук"))
{
    int track = findTrack(model, error, marker);
    if (track == -2) return -1;
    if (track == -1 && !model->requestTrackInsertion(-1, track, title, true, undo, redo, false)) {
        if (error) *error = QStringLiteral("Не удалось создать служебную аудиодорожку.");
        return -1;
    }
    const QString previous = model->getTrackProperty(track, marker).toString();
    if (previous != QLatin1String("1")) {
        const auto set = [weakModel = std::weak_ptr<TimelineItemModel>(model), track, marker](const QString &value) {
            const auto model = weakModel.lock();
            if (!model || !model->isTrack(track)) return false;
            model->setTrackProperty(track, marker, value);
            return true;
        };
        Fun operation = [set] { return set(QStringLiteral("1")); };
        Fun reverse = [set, previous] { return set(previous); };
        if (!operation()) return -1;
        UPDATE_UNDO_REDO_NOLOCK(operation, reverse, undo, redo);
    }
    return track;
}

inline std::shared_ptr<EffectItemModel> ensureMute(const std::shared_ptr<EffectStackModel> &stack,
                                                  const QString &job, bool allowCreate,
                                                  Fun &undo, Fun &redo, QString *error)
{
    if (!stack || job.isEmpty()) {
        if (error) *error = QStringLiteral("Исходный клип или идентификатор обработки недоступен.");
        return {};
    }
    QList<std::shared_ptr<EffectItemModel>> owned;
    const auto before = StudioHelpers::effectsById(stack, QStringLiteral("volume"));
    for (const auto &effect : before) {
        if (effect->getParam(QStringLiteral("studio:audio:role")) != QLatin1String("source-mute")) continue;
        if (effect->getParam(QStringLiteral("studio:audio:id")) != job) {
            if (error) *error = QStringLiteral("Исходник принадлежит другому результату; изменения отменены.");
            return {};
        }
        owned << effect;
    }
    if (owned.isEmpty()) {
        if (!allowCreate || !stack->appendEffectWithUndo(QStringLiteral("volume"), undo, redo).first) {
            if (error) *error = QStringLiteral("Не удалось найти или создать собственное заглушение источника.");
            return {};
        }
        for (const auto &effect : StudioHelpers::effectsById(stack, QStringLiteral("volume")))
            if (!before.contains(effect)) { owned << effect; break; }
    }
    if (owned.isEmpty()) return {};
    // Remove only duplicates with BOTH our role and this exact job identity.
    // Normal user volume filters and other jobs are never removed.
    for (int i = 1; i < owned.size(); ++i) {
        QString effectName;
        stack->removeEffectWithUndo(owned[i], effectName, undo, redo);
    }
    return owned.front();
}
}
