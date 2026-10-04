// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

namespace StudioResources {
inline bool safeRelative(const QString &name)
{
    return !name.isEmpty() && !QDir::isAbsolutePath(name) && !name.split(QLatin1Char('/')).contains(QStringLiteral(".."));
}
inline QStringList prefixes()
{
    QStringList result;
    const QString override = qEnvironmentVariable("STUDIO_PREFIX");
    if (!override.isEmpty()) result << QDir(override).absolutePath();
    result << QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral(".."));
    result.removeDuplicates();
    return result;
}
inline QString executable(const QString &name)
{
    if (!safeRelative(name) || name.contains(QLatin1Char('/'))) return {};
    for (const auto &prefix : prefixes()) for (const auto &sub : {QStringLiteral("libexec"), QStringLiteral("bin")}) {
        const QFileInfo file(QDir(prefix).filePath(sub + QLatin1Char('/') + name));
        if (file.isFile() && file.isExecutable()) return file.absoluteFilePath();
    }
    const QString system = QStandardPaths::findExecutable(name);
    if (!system.isEmpty()) return system;
    for (const auto &sub : {QStringLiteral("/app/libexec/"), QStringLiteral("/app/bin/")}) {
        const QFileInfo file(sub + name);
        if (file.isFile() && file.isExecutable()) return file.absoluteFilePath();
    }
    return {};
}
inline QString dataFile(const QString &name)
{
    if (!safeRelative(name)) return {};
    QStringList folders;
    const QString override = qEnvironmentVariable("STUDIO_DATA_DIR");
    if (!override.isEmpty()) folders << override;
    for (const auto &prefix : prefixes()) folders << QDir(prefix).filePath(QStringLiteral("share"));
    folders << QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation) << QStringLiteral("/app/share");
    for (const auto &folder : folders) {
        const QFileInfo file(QDir(folder).filePath(name));
        if (file.isFile() && file.isReadable()) return file.absoluteFilePath();
    }
    return {};
}
}
