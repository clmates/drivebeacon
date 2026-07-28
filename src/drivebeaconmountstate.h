// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>

/** Persisted Graph state needed by Dolphin's lightweight integrations. */
struct DriveBeaconMountState
{
    QString profileName;
    QString mountDirectory;
    QStringList placeholderPaths;
    QStringList localSignatures;
    /** Relative path policies used to derive inherited folder state. */
    QStringList pathPolicies;
};

/**
 * Reads Graph mount state without contacting the service or opening files.
 *
 * Dolphin loads both plugins independently, so this shared reader keeps their
 * profile filtering, mount-path fallback, and persisted-state interpretation
 * identical. The service remains authoritative for synchronization actions.
 */
inline QList<DriveBeaconMountState> driveBeaconMountStates()
{
    const QString configFile = QDir(
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
                                   .filePath(QStringLiteral("clmates/drivebeacon.conf"));
    QSettings settings(configFile, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("profiles"));
    QList<DriveBeaconMountState> profiles;
    for (const QString &name : settings.childGroups()) {
        settings.beginGroup(name);
        if (settings.value(QStringLiteral("backend")).toString() != QLatin1String("graph")) {
            settings.endGroup();
            continue;
        }

        const QString mountDirectory = settings.value(
            QStringLiteral("mountDirectory"),
            settings.value(QStringLiteral("localDirectory"))).toString();
        if (!mountDirectory.isEmpty()) {
            profiles.append({name,
                             QDir::cleanPath(QFileInfo(mountDirectory).absoluteFilePath()),
                             settings.value(QStringLiteral("graphPlaceholderPaths"))
                                 .toStringList(),
                             settings.value(QStringLiteral("graphLocalSignatures"))
                                 .toStringList(),
                             settings.value(QStringLiteral("graphPathPolicies"))
                                 .toStringList()});
        }
        settings.endGroup();
    }
    return profiles;
}

/** Returns the nearest persisted policy, allowing folder rules to inherit. */
inline QString driveBeaconPathPolicy(const DriveBeaconMountState &profile,
                                     QString path)
{
    path = QDir::cleanPath(path);
    while (!path.isEmpty() && path != QLatin1String(".")) {
        for (const QString &record : profile.pathPolicies) {
            const int separator = record.indexOf(QLatin1Char('\t'));
            if (separator > 0 && record.left(separator) == path) {
                return record.sliced(separator + 1);
            }
        }
        const int separator = path.lastIndexOf(QLatin1Char('/'));
        if (separator < 0) {
            break;
        }
        path.truncate(separator);
    }
    return {};
}

/** Converts a local URL into a path relative to one configured mount. */
inline QString driveBeaconRelativePath(const QString &mountDirectory, const QUrl &url)
{
    const QString localPath = QDir::cleanPath(QFileInfo(url.toLocalFile()).absoluteFilePath());
    const QDir mount(QDir::cleanPath(QFileInfo(mountDirectory).absoluteFilePath()));
    const QString relative = QDir::cleanPath(mount.relativeFilePath(localPath));
    if (relative.isEmpty() || relative == QLatin1String(".")
        || relative == QLatin1String("..") || relative.startsWith(QStringLiteral("../"))) {
        return {};
    }
    return relative;
}
