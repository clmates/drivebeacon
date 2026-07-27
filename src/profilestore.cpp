// SPDX-License-Identifier: GPL-3.0-only

#include "profilestore.h"

#include <QDir>
#include <QStandardPaths>
#include <QSettings>

QSettings ProfileStore::settings()
{
    const QString configFile = QDir(
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
        .filePath(QStringLiteral("clmates/drivebeacon.conf"));
    return QSettings(configFile, QSettings::IniFormat);
}

ProfileStore::ProfileStore(QObject *parent)
    : QObject(parent)
{
}

QString ProfileStore::normalizedName(const QString &name)
{
    QString normalized = name.trimmed();
    normalized.replace(QLatin1Char('/'), QLatin1Char('_'));
    normalized.replace(QLatin1Char('\\'), QLatin1Char('_'));
    return normalized.isEmpty() ? QStringLiteral("default") : normalized;
}

QStringList ProfileStore::profileNames() const
{
    QSettings settings = ProfileStore::settings();
    settings.beginGroup(QStringLiteral("profiles"));
    return settings.childGroups();
}

QString ProfileStore::activeProfileName() const
{
    QSettings settings = ProfileStore::settings();
    return normalizedName(settings.value(QStringLiteral("profiles/active"),
                                         QStringLiteral("default"))
                              .toString());
}

SyncProfile ProfileStore::load(const QString &requestedName) const
{
    QSettings settings = ProfileStore::settings();
    const QString name = normalizedName(requestedName.isEmpty() ? activeProfileName()
                                                                  : requestedName);
    settings.beginGroup(QStringLiteral("profiles/%1").arg(name));

    SyncProfile profile;
    profile.name = name;
    profile.name = settings.value(QStringLiteral("name"), profile.name).toString();
    profile.backend = syncBackendFromName(
        settings.value(QStringLiteral("backend"), syncBackendName(profile.backend)).toString());
    profile.accountId = settings.value(QStringLiteral("accountId")).toString();
    profile.graphClientId = settings.value(QStringLiteral("graphClientId")).toString();
    profile.remoteDriveId = settings.value(QStringLiteral("remoteDriveId")).toString();
    profile.localDirectory = settings.value(
        QStringLiteral("localDirectory"), QDir::home().filePath(QStringLiteral("OneDrive")))
                                 .toString();
    // The mount path is independent from the private cache. Fall back to the
    // historical directory only for profiles created before FUSE mounts were
    // configurable, so an upgrade never silently changes the visible path.
    profile.mountDirectory = settings.value(
        QStringLiteral("mountDirectory"), profile.localDirectory).toString();
    // Historical profiles stored KeepLocal or RemoteOnly globally. Normalize
    // them immediately: materialization now belongs to per-path policies.
    profile.availability = LocalAvailability::OnDemand;
    // Missing values remain enabled for backward compatibility with existing
    // profiles created before per-account pause state was introduced.
    profile.syncEnabled = settings.value(QStringLiteral("syncEnabled"), true).toBool();
    profile.remoteCheckIntervalSeconds = qBound(10, settings.value(
        QStringLiteral("remoteCheckIntervalSeconds"), profile.remoteCheckIntervalSeconds).toInt(), 3600);
    profile.concurrentDownloads = qBound(1, settings.value(
        QStringLiteral("concurrentDownloads"), profile.concurrentDownloads).toInt(), 8);
    profile.concurrentUploads = qBound(1, settings.value(
        QStringLiteral("concurrentUploads"), profile.concurrentUploads).toInt(), 8);
    profile.concurrentLargeTransfers = qBound(1, settings.value(
        QStringLiteral("concurrentLargeTransfers"), profile.concurrentLargeTransfers).toInt(), 4);
    profile.graphDeltaLink = settings.value(QStringLiteral("graphDeltaLink")).toString();
    profile.graphLocalSignatures = settings.value(QStringLiteral("graphLocalSignatures"))
                                       .toStringList();
    // The remote baseline is intentionally persisted as opaque records. The
    // Graph client owns their tab-separated ID/path/eTag format and also reads
    // the older two-field ID/path format for backward compatibility.
    profile.graphRemotePaths = settings.value(QStringLiteral("graphRemotePaths")).toStringList();
    profile.graphPlaceholderPaths = settings.value(QStringLiteral("graphPlaceholderPaths"))
                                       .toStringList();
    profile.graphPathPolicies = settings.value(QStringLiteral("graphPathPolicies"))
                                    .toStringList();
    profile.includedFolders = settings.value(QStringLiteral("includedFolders")).toStringList();
    profile.excludedFolders = settings.value(QStringLiteral("excludedFolders")).toStringList();
    profile.graphSyncedIncludedFolders = settings.value(
        QStringLiteral("graphSyncedIncludedFolders")).toStringList();
    profile.graphSyncedExcludedFolders = settings.value(
        QStringLiteral("graphSyncedExcludedFolders")).toStringList();
    // Older dialog versions could append the same folder on every save. Keep
    // the in-memory policy canonical so duplicate entries cannot trigger
    // unnecessary folder refreshes or obscure the user's actual selection.
    profile.includedFolders.removeDuplicates();
    profile.excludedFolders.removeDuplicates();
    profile.graphSyncedIncludedFolders.removeDuplicates();
    profile.graphSyncedExcludedFolders.removeDuplicates();
    return profile;
}

void ProfileStore::save(const SyncProfile &profile)
{
    QSettings settings = ProfileStore::settings();
    const QString name = normalizedName(profile.name);
    settings.beginGroup(QStringLiteral("profiles/%1").arg(name));
    settings.setValue(QStringLiteral("name"), name);
    settings.setValue(QStringLiteral("backend"), syncBackendName(profile.backend));
    settings.setValue(QStringLiteral("accountId"), profile.accountId);
    settings.setValue(QStringLiteral("graphClientId"), profile.graphClientId);
    settings.setValue(QStringLiteral("remoteDriveId"), profile.remoteDriveId);
    settings.setValue(QStringLiteral("localDirectory"), profile.localDirectory);
    settings.setValue(QStringLiteral("mountDirectory"), profile.mountDirectory);
    // Keep the key for older clients, but never persist a profile-wide mode.
    settings.setValue(QStringLiteral("availability"), QStringLiteral("on-demand"));
    settings.setValue(QStringLiteral("syncEnabled"), profile.syncEnabled);
    settings.setValue(QStringLiteral("remoteCheckIntervalSeconds"),
                      qBound(10, profile.remoteCheckIntervalSeconds, 3600));
    settings.setValue(QStringLiteral("concurrentDownloads"), qBound(1, profile.concurrentDownloads, 8));
    settings.setValue(QStringLiteral("concurrentUploads"), qBound(1, profile.concurrentUploads, 8));
    settings.setValue(QStringLiteral("concurrentLargeTransfers"),
                      qBound(1, profile.concurrentLargeTransfers, 4));
    settings.setValue(QStringLiteral("graphDeltaLink"), profile.graphDeltaLink);
    settings.setValue(QStringLiteral("graphLocalSignatures"), profile.graphLocalSignatures);
    settings.setValue(QStringLiteral("graphRemotePaths"), profile.graphRemotePaths);
    settings.setValue(QStringLiteral("graphPlaceholderPaths"), profile.graphPlaceholderPaths);
    settings.setValue(QStringLiteral("graphPathPolicies"), profile.graphPathPolicies);
    settings.setValue(QStringLiteral("includedFolders"), profile.includedFolders);
    settings.setValue(QStringLiteral("excludedFolders"), profile.excludedFolders);
    settings.setValue(QStringLiteral("graphSyncedIncludedFolders"),
                      profile.graphSyncedIncludedFolders);
    settings.setValue(QStringLiteral("graphSyncedExcludedFolders"),
                      profile.graphSyncedExcludedFolders);
    settings.endGroup();
    settings.sync();
}

bool ProfileStore::globalSyncEnabled() const
{
    QSettings settings = ProfileStore::settings();
    return settings.value(QStringLiteral("profiles/globalSyncEnabled"), true).toBool();
}

void ProfileStore::setGlobalSyncEnabled(bool enabled)
{
    QSettings settings = ProfileStore::settings();
    settings.setValue(QStringLiteral("profiles/globalSyncEnabled"), enabled);
    settings.sync();
}

void ProfileStore::setActiveProfileName(const QString &name)
{
    QSettings settings = ProfileStore::settings();
    settings.setValue(QStringLiteral("profiles/active"), normalizedName(name));
    settings.sync();
}
