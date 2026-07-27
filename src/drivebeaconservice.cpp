// SPDX-License-Identifier: GPL-3.0-only

#include "drivebeaconservice.h"

#include <algorithm>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSet>

namespace {

/** Returns the application data root without embedding a vendor namespace. */
QString driveBeaconDataRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("drivebeacon"));
}

/** Returns the private per-profile cache used as the FUSE backing store. */
QString driveBeaconCacheDirectory(const QString &profile)
{
    return QDir(driveBeaconDataRoot()).filePath(QStringLiteral("cache/") + profile);
}

/** Keeps upgrades from leaving the previous service-owned mount active. */
QString legacyMountPoint(const QString &profile)
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("clmates/drivebeacon/mounts/") + profile);
}

}

DriveBeaconService::DriveBeaconService(const QString &profileName, QObject *parent)
    : QObject(parent)
    , m_profileStore(this)
    , m_activeProfileName(profileName.isEmpty() ? m_profileStore.activeProfileName()
                                                 : profileName)
    , m_requestedProfileName(profileName)
{
    reloadProfiles();
}

DriveBeaconService::~DriveBeaconService()
{
    const QStringList profiles = m_fuseProcesses.keys();
    for (const QString &profile : profiles) {
        unmountProfile(profile);
    }
}

void DriveBeaconService::reloadProfiles()
{
    const bool globallyEnabled = m_profileStore.globalSyncEnabled();
    const QStringList configuredProfileNames = m_profileStore.profileNames();
    const QSet<QString> configuredProfiles = QSet<QString>(
        configuredProfileNames.cbegin(), configuredProfileNames.cend());
    for (const QString &name : configuredProfiles) {
        const SyncProfile profile = m_profileStore.load(name);
        if (profile.backend != SyncBackend::MicrosoftGraph) {
            continue;
        }
        if (m_controllers.contains(name)) {
            auto *controller = m_controllers.value(name);
            controller->setGlobalGraphSyncEnabled(globallyEnabled);
            controller->setGraphPathPolicies(profile.graphPathPolicies);
            continue;
        }
        // A paused profile remains configured and keeps its persisted state;
        // its controller still owns authentication, but its Graph polling and
        // transfer schedulers remain stopped until the profile is resumed.
        // The service owns synchronization and therefore redirects backend
        // I/O into a private cache. The profile's mountDirectory remains the
        // user-visible FUSE path and is never used as a backing directory.
        auto *controller = new OneDriveController(
            name, {}, driveBeaconCacheDirectory(name), true, false, this);
        controller->setGlobalGraphSyncEnabled(globallyEnabled);
        m_controllers.insert(name, controller);
        connect(controller, &OneDriveController::graphSyncChanged,
                this, &DriveBeaconService::publishStatus);
        connect(controller, &OneDriveController::graphAuthChanged,
                this, &DriveBeaconService::publishStatus);
        connect(controller, &OneDriveController::logMessage, this,
                [this, name](const QString &message) {
                    // Include the account key so the shared tray history can
                    // distinguish identical paths handled by different users.
                    Q_EMIT activityMessage(QStringLiteral("[%1] %2").arg(name, message));
                });
        connect(controller, &OneDriveController::errorMessageChanged,
                this, &DriveBeaconService::publishStatus);
    }
    if (m_requestedProfileName.isEmpty()) {
        m_activeProfileName = m_profileStore.activeProfileName();
    }
    Q_EMIT statusChanged();
}

OneDriveController *DriveBeaconService::activeController() const
{
    return m_controllers.value(m_activeProfileName, nullptr);
}

QString DriveBeaconService::profileName() const
{
    return activeController() ? activeController()->profileName() : m_activeProfileName;
}

QString DriveBeaconService::backendName() const
{
    return activeController() ? activeController()->backendName() : QString();
}

QStringList DriveBeaconService::graphProfiles() const
{
    QStringList profiles = m_controllers.keys();
    std::sort(profiles.begin(), profiles.end());
    return profiles;
}

QVariantMap DriveBeaconService::profileStatus(const QString &profileName) const
{
    QVariantMap status;
    const QString name = profileName.trimmed();
    auto *controller = m_controllers.value(name, nullptr);
    if (!controller) {
        status.insert(QStringLiteral("error"), QStringLiteral("Profile is not loaded."));
        return status;
    }
    status.insert(QStringLiteral("profileName"), controller->profileName());
    status.insert(QStringLiteral("backendName"), controller->backendName());
    status.insert(QStringLiteral("availability"), controller->availabilityName());
    status.insert(QStringLiteral("authenticated"), controller->graphAuthenticated());
    status.insert(QStringLiteral("syncEnabled"), controller->graphSyncEnabled());
    status.insert(QStringLiteral("syncStatus"), controller->graphSyncStatus());
    status.insert(QStringLiteral("syncProgress"), controller->graphSyncProgress());
    const SyncProfile profile = m_profileStore.load(name);
    // Expose the user-visible path as the legacy `localDirectory` field so
    // existing CLI/tray clients keep showing a useful location. The cache is
    // deliberately separate and is reported explicitly for diagnostics.
    status.insert(QStringLiteral("localDirectory"), profile.mountDirectory);
    status.insert(QStringLiteral("cacheDirectory"), controller->syncDirectory());
    status.insert(QStringLiteral("quotaTotal"), controller->remoteQuotaTotal());
    status.insert(QStringLiteral("quotaUsed"), controller->remoteQuotaUsed());
    status.insert(QStringLiteral("quotaRemaining"), controller->remoteQuotaRemaining());
    status.insert(QStringLiteral("error"), controller->errorMessage());
    const QProcess *fuseProcess = m_fuseProcesses.value(name, nullptr);
    status.insert(QStringLiteral("mounted"), fuseProcess
                     && fuseProcess->state() == QProcess::Running);
    status.insert(QStringLiteral("mountPoint"),
                  profile.mountDirectory);
    return status;
}

QVariantList DriveBeaconService::remoteEntries(const QString &profileName) const
{
    if (auto *controller = m_controllers.value(profileName.trimmed(), nullptr)) {
        return controller->graphRemoteEntries();
    }
    return {};
}

void DriveBeaconService::materializeFile(const QString &profileName,
                                         const QString &relativePath)
{
    if (auto *controller = m_controllers.value(profileName.trimmed(), nullptr)) {
        controller->materializeGraphFile(relativePath);
    }
}

void DriveBeaconService::keepLocalPath(const QString &profileName,
                                       const QString &relativePath)
{
    if (auto *controller = m_controllers.value(profileName.trimmed(), nullptr)) {
        controller->setGraphPathPolicy(relativePath, QStringLiteral("keep-local"));
    }
}

void DriveBeaconService::evictPath(const QString &profileName,
                                   const QString &relativePath)
{
    if (auto *controller = m_controllers.value(profileName.trimmed(), nullptr)) {
        controller->evictGraphPath(relativePath);
    }
}

void DriveBeaconService::mountProfile(const QString &profileName)
{
    const QString name = profileName.trimmed();
    if (name.isEmpty() || m_fuseProcesses.contains(name)) {
        qWarning().noquote() << "DriveBeacon FUSE mount skipped for" << name
                             << "because the profile is empty or already mounted";
        return;
    }
    auto *controller = m_controllers.value(name, nullptr);
    if (!controller || !controller->graphAuthenticated()) {
        qWarning().noquote() << "DriveBeacon FUSE mount rejected for" << name
                             << "because the Graph profile is not loaded or authenticated";
        return;
    }
    const QVariantMap status = profileStatus(name);
    const QString mountPoint = status.value(QStringLiteral("mountPoint")).toString();
    const QString backingDirectory = status.value(QStringLiteral("cacheDirectory")).toString();
    if (mountPoint.isEmpty() || backingDirectory.isEmpty()) {
        qWarning().noquote() << "DriveBeacon FUSE mount rejected for" << name
                             << "because mount/cache paths are invalid"
                             << mountPoint << backingDirectory;
        return;
    }
    if (!QDir().mkpath(mountPoint)) {
        // A previous helper can leave a disconnected FUSE endpoint after a
        // crash or forced termination. Remove only that profile's stale
        // mount, then retry the directory creation before giving up.
        const QString fusermount = QStandardPaths::findExecutable(QStringLiteral("fusermount3"));
        if (!fusermount.isEmpty()) {
            QProcess::execute(fusermount, {QStringLiteral("-u"), mountPoint});
        }
    }
    if (!QDir().mkpath(mountPoint)) {
        qWarning().noquote() << "DriveBeacon FUSE mount rejected for" << name
                             << "because mount/cache paths are invalid"
                             << mountPoint << backingDirectory;
        return;
    }
    QString helper = QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("drivebeacon-fs"));
    if (!QFileInfo::exists(helper)) {
        helper = QStandardPaths::findExecutable(QStringLiteral("drivebeacon-fs"));
    }
    if (helper.isEmpty()) {
        qWarning().noquote() << "DriveBeacon FUSE mount rejected for" << name
                             << "because drivebeacon-fs was not found";
        return;
    }
    auto *process = new QProcess(this);
    // Keep libfuse diagnostics in the service journal. This is especially
    // important for mounts started from systemd, where no terminal receives
    // the helper's stderr and a dead helper otherwise looks like an EIO mount.
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    process->setProgram(helper);
    process->setArguments({QStringLiteral("--profile"), name,
                            QStringLiteral("--backing-directory"), backingDirectory,
                            mountPoint});
    connect(process, &QProcess::stateChanged, this,
            [this](QProcess::ProcessState) { Q_EMIT statusChanged(); });
    connect(process, &QProcess::finished, this,
            [this, name](int exitCode, QProcess::ExitStatus exitStatus) {
                qInfo().noquote() << "DriveBeacon FUSE helper finished for" << name
                                  << "exit=" << exitCode
                                  << "status=" << exitStatus;
                if (auto *process = m_fuseProcesses.take(name)) {
                    process->deleteLater();
                }
                Q_EMIT statusChanged();
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, name](QProcess::ProcessError error) {
                qWarning().noquote() << "DriveBeacon FUSE helper error for" << name
                                     << error;
                Q_EMIT statusChanged();
            });
    m_fuseProcesses.insert(name, process);
    process->start();
    Q_EMIT statusChanged();
}

void DriveBeaconService::unmountProfile(const QString &profileName)
{
    const QString name = profileName.trimmed();
    auto *process = m_fuseProcesses.value(name, nullptr);
    if (!process) {
        return;
    }
    const QString mountPoint = profileStatus(name).value(QStringLiteral("mountPoint")).toString();
    const QString fusermount = QStandardPaths::findExecutable(QStringLiteral("fusermount3"));
    if (!fusermount.isEmpty()) {
        QStringList mountPoints{mountPoint};
        const QString oldMountPoint = legacyMountPoint(name);
        if (!mountPoints.contains(oldMountPoint)) {
            mountPoints.append(oldMountPoint);
        }
        for (const QString &point : mountPoints) {
            if (!point.isEmpty()) {
                QProcess::execute(fusermount, {QStringLiteral("-u"), point});
            }
        }
    }
    process->terminate();
    if (!process->waitForFinished(3000)) {
        process->kill();
    }
    m_fuseProcesses.remove(name);
    process->deleteLater();
    Q_EMIT statusChanged();
}

QString DriveBeaconService::syncStatus() const
{
    return activeController() ? activeController()->graphSyncStatus() : QStringLiteral("Idle");
}

int DriveBeaconService::syncProgress() const
{
    return activeController() ? activeController()->graphSyncProgress() : 0;
}

bool DriveBeaconService::graphAuthenticated() const
{
    return activeController() && activeController()->graphAuthenticated();
}

QString DriveBeaconService::errorMessage() const
{
    return activeController() ? activeController()->errorMessage() : QString();
}

bool DriveBeaconService::graphSyncEnabled() const
{
    return activeController() && activeController()->graphSyncEnabled();
}

bool DriveBeaconService::globalSyncEnabled() const
{
    return m_profileStore.globalSyncEnabled();
}

void DriveBeaconService::synchronizeGraph()
{
    if (auto *controller = activeController()) {
        controller->synchronizeGraph();
    }
}

void DriveBeaconService::forceRemoteResync(const QString &profileName)
{
    if (auto *controller = m_controllers.value(profileName.trimmed(), nullptr)) {
        controller->forceRemoteResync();
    }
}

void DriveBeaconService::refreshGraphFolders()
{
    if (auto *controller = activeController()) {
        controller->refreshGraphFolders();
    }
}

void DriveBeaconService::setProfileSyncEnabled(const QString &profileName, bool enabled)
{
    if (auto *controller = m_controllers.value(profileName.trimmed(), nullptr)) {
        controller->setGraphSyncEnabled(enabled);
        publishStatus();
    }
}

void DriveBeaconService::setProfileAvailability(const QString &profileName,
                                                const QString &availability)
{
    if (auto *controller = m_controllers.value(profileName.trimmed(), nullptr)) {
        controller->setAvailability(availability);
        publishStatus();
    }
}

void DriveBeaconService::setGlobalSyncEnabled(bool enabled)
{
    m_profileStore.setGlobalSyncEnabled(enabled);
    for (auto *controller : m_controllers) {
        controller->setGlobalGraphSyncEnabled(enabled);
    }
    publishStatus();
}

void DriveBeaconService::setPrimaryProfile(const QString &profileName)
{
    const QString name = profileName.trimmed();
    if (!m_controllers.contains(name)) {
        return;
    }
    // The primary account is a presentation preference, not a synchronization
    // switch; changing it must not restart controllers or reset their state.
    m_activeProfileName = name;
    m_requestedProfileName.clear();
    m_profileStore.setActiveProfileName(name);
    Q_EMIT statusChanged();
}

void DriveBeaconService::publishStatus()
{
    Q_EMIT statusChanged();
}
