// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "core.h"
#include "effects/effectstack/model/effectitemmodel.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "undohelper.hpp"
#include <mlt++/MltFilter.h>

namespace StudioHelpers {
inline void setBackgroundClipIn(const std::shared_ptr<EffectItemModel> &effect, int clipIn)
{
    if (effect->filter().get_int("_sbg_clip_in") == clipIn) return;
    effect->filter().set("_sbg_clip_in", clipIn);
    // Kdenlive renders child filters too, not only the panel's master.
    Q_EMIT effect->updateChildren({QStringLiteral("_sbg_clip_in")});
    if (pCore) pCore->invalidateItem(effect->getOwnerId());
}

inline void setBackgroundMaskPath(const std::shared_ptr<EffectItemModel> &effect, const QString &path)
{
    if (QString::fromUtf8(effect->filter().get("_sbg_mask_path")) == path) return;
    effect->filter().set("_sbg_mask_path", path.isEmpty() ? nullptr : path.toUtf8().constData());
    Q_EMIT effect->updateChildren({QStringLiteral("_sbg_mask_path")});
    if (pCore) {
        pCore->refreshProjectItem(effect->getOwnerId());
        pCore->invalidateItem(effect->getOwnerId());
    }
}

inline QList<std::shared_ptr<EffectItemModel>> effectsById(const std::shared_ptr<EffectStackModel> &stack, const QString &assetId)
{
    QList<std::shared_ptr<EffectItemModel>> result;
    if (!stack) return result;
    std::function<void(std::shared_ptr<TreeItem>)> collect = [&](const std::shared_ptr<TreeItem> &item) {
        if (auto effect = std::dynamic_pointer_cast<EffectItemModel>(item); effect && effect->getAssetId() == assetId) result << effect;
        for (int i = 0; i < item->childCount(); ++i) collect(item->child(i));
    };
    collect(stack->getRoot());
    return result;
}

inline void appendParameterChange(const std::shared_ptr<AssetParameterModel> &effect, const QStringList &names, const QStringList &values,
                           Fun &undo, Fun &redo)
{
    QStringList before;
    for (const auto &name : names) before << effect->getParam(name);
    if (before == values) return;
    auto apply = [effect, names](const QStringList &next) {
        for (int i = 0; i < names.size(); ++i) effect->setParameter(names[i], next[i], true);
        return true;
    };
    Fun localUndo = [apply, before]() { return apply(before); };
    Fun localRedo = [apply, values]() { return apply(values); };
    localRedo();
    Fun previousUndo = undo;
    undo = [localUndo, previousUndo]() {
        const bool first = localUndo();
        const bool second = previousUndo();
        return first && second;
    };
    Fun previousRedo = redo;
    redo = [previousRedo, localRedo]() {
        const bool first = previousRedo();
        const bool second = localRedo();
        return first && second;
    };
}

inline void appendMaskPathChange(const std::shared_ptr<EffectItemModel> &effect, const QString &path, Fun &undo, Fun &redo)
{
    const QString previous = QString::fromUtf8(effect->filter().get("_sbg_mask_path"));
    const auto apply = [effect](const QString &value) {
        setBackgroundMaskPath(effect, value);
        return true;
    };
    const Fun parameterUndo = undo, parameterRedo = redo;
    undo = [parameterUndo, apply, previous] {
        const bool result = parameterUndo();
        apply(previous);
        return result;
    };
    redo = [parameterRedo, apply, path] { return parameterRedo() && apply(path); };
    apply(path);
}
}
