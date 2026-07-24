// SPDX-License-Identifier: GPL-3.0-only

#include "profilestore.h"

#include <QDir>
#include <QSettings>

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
    QSettings settings;
    settings.beginGroup(QStringLiteral("profiles"));
    return settings.childGroups();
}

QString ProfileStore::activeProfileName() const
{
    QSettings settings;
    return normalizedName(settings.value(QStringLiteral("profiles/active"),
                                         QStringLiteral("default"))
                              .toString());
}

SyncProfile ProfileStore::load(const QString &requestedName) const
{
    QSettings settings;
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
    profile.availability = localAvailabilityFromName(
        settings.value(QStringLiteral("availability"), localAvailabilityName(profile.availability))
            .toString());
    profile.remoteCheckIntervalSeconds = qBound(10, settings.value(
        QStringLiteral("remoteCheckIntervalSeconds"), profile.remoteCheckIntervalSeconds).toInt(), 3600);
    profile.graphDeltaLink = settings.value(QStringLiteral("graphDeltaLink")).toString();
    profile.graphLocalSignatures = settings.value(QStringLiteral("graphLocalSignatures"))
                                       .toStringList();
    // The remote baseline is intentionally persisted as opaque records. The
    // Graph client owns their tab-separated ID/path/eTag format and also reads
    // the older two-field ID/path format for backward compatibility.
    profile.graphRemotePaths = settings.value(QStringLiteral("graphRemotePaths")).toStringList();
    profile.includedFolders = settings.value(QStringLiteral("includedFolders")).toStringList();
    profile.excludedFolders = settings.value(QStringLiteral("excludedFolders")).toStringList();
    return profile;
}

void ProfileStore::save(const SyncProfile &profile)
{
    QSettings settings;
    const QString name = normalizedName(profile.name);
    settings.beginGroup(QStringLiteral("profiles/%1").arg(name));
    settings.setValue(QStringLiteral("name"), name);
    settings.setValue(QStringLiteral("backend"), syncBackendName(profile.backend));
    settings.setValue(QStringLiteral("accountId"), profile.accountId);
    settings.setValue(QStringLiteral("graphClientId"), profile.graphClientId);
    settings.setValue(QStringLiteral("remoteDriveId"), profile.remoteDriveId);
    settings.setValue(QStringLiteral("localDirectory"), profile.localDirectory);
    settings.setValue(QStringLiteral("availability"), localAvailabilityName(profile.availability));
    settings.setValue(QStringLiteral("remoteCheckIntervalSeconds"),
                      qBound(10, profile.remoteCheckIntervalSeconds, 3600));
    settings.setValue(QStringLiteral("graphDeltaLink"), profile.graphDeltaLink);
    settings.setValue(QStringLiteral("graphLocalSignatures"), profile.graphLocalSignatures);
    settings.setValue(QStringLiteral("graphRemotePaths"), profile.graphRemotePaths);
    settings.setValue(QStringLiteral("includedFolders"), profile.includedFolders);
    settings.setValue(QStringLiteral("excludedFolders"), profile.excludedFolders);
    settings.endGroup();
    settings.sync();
}

void ProfileStore::setActiveProfileName(const QString &name)
{
    QSettings settings;
    settings.setValue(QStringLiteral("profiles/active"), normalizedName(name));
    settings.sync();
}
