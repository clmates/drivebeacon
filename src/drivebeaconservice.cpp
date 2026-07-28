// SPDX-License-Identifier: GPL-3.0-only

#include "drivebeaconservice.h"

#include "tokenstore.h"

#include <algorithm>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSet>
#include <QTimer>

namespace {

/** Encodes immediate D-Bus command acceptance for CLI and tray clients. */
QVariantMap operationResult(bool ok, const QString &message = {})
{
    return {{QStringLiteral("ok"), ok}, {QStringLiteral("message"), message}};
}

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

QVariantMap DriveBeaconService::reloadProfiles()
{
    const bool globallyEnabled = m_profileStore.globalSyncEnabled();
    const QStringList configuredProfileNames = m_profileStore.profileNames();
    const QSet<QString> configuredProfiles = QSet<QString>(
        configuredProfileNames.cbegin(), configuredProfileNames.cend());
    // Remove controllers for deleted profiles or profiles switched to the
    // optional legacy backend. Their cache and remote data remain untouched;
    // only this service's in-memory owner is retired.
    const QStringList loadedProfiles = m_controllers.keys();
    for (const QString &name : loadedProfiles) {
        const SyncProfile profile = m_profileStore.load(name);
        if (!configuredProfiles.contains(name)
            || profile.backend != SyncBackend::MicrosoftGraph) {
            unmountProfile(name);
            auto *controller = m_controllers.take(name);
            controller->deleteLater();
        }
    }
    for (const QString &name : configuredProfiles) {
        const SyncProfile profile = m_profileStore.load(name);
        if (profile.backend != SyncBackend::MicrosoftGraph) {
            continue;
        }
        if (m_controllers.contains(name)) {
            auto *controller = m_controllers.value(name);
            controller->setGlobalGraphSyncEnabled(globallyEnabled);
            const QString oldMountPoint = controller->mountDirectory();
            controller->reloadProfileSettings(profile);
            if (oldMountPoint != profile.mountDirectory) {
                unmountProfile(name);
                if (controller->graphAuthenticated()) {
                    QTimer::singleShot(0, this, [this, name] { mountProfile(name); });
                }
            }
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
        connect(controller, &OneDriveController::graphAuthChanged, this,
                [this, name, controller] {
                    Q_EMIT graphAuthStateChanged(name, controller->graphAuthenticated(),
                                                 controller->graphErrorMessage(),
                                                 controller->graphAuthorizationUrl());
                });
        connect(controller, &OneDriveController::graphRemoteFoldersChanged, this,
                [this, name, controller] {
                    Q_EMIT graphRemoteFoldersChanged(name, controller->graphRemoteFolders());
                });
        // Mounts are service-owned runtime state, so a service restart drops
        // the helper process. Recreate the user's configured FUSE view as soon
        // as that profile has authenticated instead of requiring the tray to
        // issue a second mount command.
        connect(controller, &OneDriveController::graphAuthChanged, this,
                [this, name, controller] {
                    if (controller->graphAuthenticated()) {
                        QTimer::singleShot(0, this, [this, name] { mountProfile(name); });
                    }
                });
        connect(controller, &OneDriveController::logMessage, this,
                [this, name](const QString &message) {
                    // Include the account key so the shared tray history can
                    // distinguish identical paths handled by different users.
                    Q_EMIT activityMessage(QStringLiteral("[%1] %2").arg(name, message));
                });
        connect(controller, &OneDriveController::errorMessageChanged,
                this, &DriveBeaconService::publishStatus);
        if (controller->graphAuthenticated()) {
            QTimer::singleShot(0, this, [this, name] { mountProfile(name); });
        }
    }
    if (m_requestedProfileName.isEmpty()) {
        m_activeProfileName = m_profileStore.activeProfileName();
    }
    Q_EMIT statusChanged();
    return operationResult(true, QStringLiteral("Profiles reloaded."));
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

QVariantMap DriveBeaconService::materializeFile(const QString &profileName,
                                                const QString &relativePath)
{
    auto *controller = m_controllers.value(profileName.trimmed(), nullptr);
    if (!controller) {
        return operationResult(false, QStringLiteral("Profile is not loaded."));
    }
    if (relativePath.trimmed().isEmpty()) {
        return operationResult(false, QStringLiteral("A relative path is required."));
    }
    controller->materializeGraphFile(relativePath);
    return operationResult(true, QStringLiteral("Materialization queued."));
}

QVariantMap DriveBeaconService::keepLocalPath(const QString &profileName,
                                              const QString &relativePath)
{
    auto *controller = m_controllers.value(profileName.trimmed(), nullptr);
    if (!controller) {
        return operationResult(false, QStringLiteral("Profile is not loaded."));
    }
    if (relativePath.trimmed().isEmpty()) {
        return operationResult(false, QStringLiteral("A relative path is required."));
    }
    controller->setGraphPathPolicy(relativePath, QStringLiteral("keep-local"));
    return operationResult(true, QStringLiteral("Keep Local applied."));
}

QVariantMap DriveBeaconService::evictPath(const QString &profileName,
                                          const QString &relativePath)
{
    auto *controller = m_controllers.value(profileName.trimmed(), nullptr);
    if (!controller) {
        return operationResult(false, QStringLiteral("Profile is not loaded."));
    }
    if (relativePath.trimmed().isEmpty()) {
        return operationResult(false, QStringLiteral("A relative path is required."));
    }
    controller->evictGraphPath(relativePath);
    return operationResult(true, QStringLiteral("Local cache release queued."));
}

QVariantMap DriveBeaconService::deleteProfile(const QString &profileName, bool deleteCache)
{
    const QString name = profileName.trimmed();
    if (name.isEmpty() || !m_profileStore.profileNames().contains(name)) {
        return operationResult(false, QStringLiteral("Profile does not exist."));
    }

    const SyncProfile profile = m_profileStore.load(name);
    // Unmount before touching the cache so no FUSE helper retains an endpoint
    // while the user deletes local profile state.
    if (m_fuseProcesses.contains(name)) {
        unmountProfile(name);
    } else {
        // A crashed or externally terminated helper may no longer be tracked
        // by this service even though its configured mount point remains busy.
        const QString fusermount = QStandardPaths::findExecutable(
            QStringLiteral("fusermount3"));
        if (!fusermount.isEmpty() && !profile.mountDirectory.isEmpty()) {
            QProcess::execute(fusermount, {QStringLiteral("-u"), profile.mountDirectory});
        }
    }
    if (auto *controller = m_controllers.take(name)) {
        controller->setGraphSyncEnabled(false);
        controller->deleteLater();
    }

    QString walletError;
    const bool credentialsRemoved = TokenStore::remove(name, &walletError);
    const bool settingsRemoved = m_profileStore.removeProfile(name);
    if (deleteCache) {
        QDir(ProfileStore::cacheDirectory(name)).removeRecursively();
    }
    if (m_activeProfileName == name) {
        m_activeProfileName = m_profileStore.activeProfileName();
    }
    if (!credentialsRemoved && !walletError.isEmpty()) {
        qWarning().noquote() << "DriveBeacon: profile credentials were not removed:"
                             << walletError;
    }
    Q_EMIT statusChanged();
    if (!settingsRemoved) {
        return operationResult(false, QStringLiteral("Could not remove local profile settings."));
    }
    return operationResult(true, deleteCache
                                      ? QStringLiteral("Profile and local cache removed.")
                                      : QStringLiteral("Profile removed; local cache preserved."));
}

QVariantMap DriveBeaconService::notifyLocalChange(const QString &profileName)
{
    auto *controller = m_controllers.value(profileName.trimmed(), nullptr);
    if (!controller) {
        return operationResult(false, QStringLiteral("Profile is not loaded."));
    }
    controller->notifyGraphLocalChange();
    return operationResult(true, QStringLiteral("Local change scan requested."));
}

QVariantMap DriveBeaconService::renameLocalPath(const QString &profileName,
                                                const QString &oldPath,
                                                const QString &newPath)
{
    auto *controller = m_controllers.value(profileName.trimmed(), nullptr);
    if (!controller) {
        return operationResult(false, QStringLiteral("Profile is not loaded."));
    }
    if (oldPath.trimmed().isEmpty() || newPath.trimmed().isEmpty()) {
        return operationResult(false, QStringLiteral("Both relative paths are required."));
    }
    controller->renameGraphPath(oldPath, newPath);
    return operationResult(true, QStringLiteral("Local rename queued."));
}

QVariantMap DriveBeaconService::mountProfile(const QString &profileName)
{
    const QString name = profileName.trimmed();
    if (name.isEmpty() || m_fuseProcesses.contains(name)) {
        qWarning().noquote() << "DriveBeacon FUSE mount skipped for" << name
                             << "because the profile is empty or already mounted";
        return operationResult(false, QStringLiteral("Profile is empty or already mounted."));
    }
    auto *controller = m_controllers.value(name, nullptr);
    if (!controller || !controller->graphAuthenticated()) {
        qWarning().noquote() << "DriveBeacon FUSE mount rejected for" << name
                             << "because the Graph profile is not loaded or authenticated";
        return operationResult(false, QStringLiteral("Profile is not loaded or authenticated."));
    }
    const QVariantMap status = profileStatus(name);
    const QString mountPoint = status.value(QStringLiteral("mountPoint")).toString();
    const QString backingDirectory = status.value(QStringLiteral("cacheDirectory")).toString();
    if (mountPoint.isEmpty() || backingDirectory.isEmpty()) {
        qWarning().noquote() << "DriveBeacon FUSE mount rejected for" << name
                             << "because mount/cache paths are invalid"
                             << mountPoint << backingDirectory;
        return operationResult(false, QStringLiteral("Mount/cache paths are invalid."));
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
        return operationResult(false, QStringLiteral("Mount/cache paths are invalid."));
    }
    QString helper = QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("drivebeacon-fs"));
    if (!QFileInfo::exists(helper)) {
        helper = QStandardPaths::findExecutable(QStringLiteral("drivebeacon-fs"));
    }
    if (helper.isEmpty()) {
        qWarning().noquote() << "DriveBeacon FUSE mount rejected for" << name
                             << "because drivebeacon-fs was not found";
        return operationResult(false, QStringLiteral("drivebeacon-fs was not found."));
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
    return operationResult(true, QStringLiteral("FUSE mount requested."));
}

QVariantMap DriveBeaconService::unmountProfile(const QString &profileName)
{
    const QString name = profileName.trimmed();
    auto *process = m_fuseProcesses.value(name, nullptr);
    if (!process) {
        return operationResult(false, QStringLiteral("Profile is not mounted."));
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
    return operationResult(true, QStringLiteral("FUSE unmount requested."));
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

QVariantMap DriveBeaconService::synchronizeGraph()
{
    auto *controller = activeController();
    if (!controller) {
        return operationResult(false, QStringLiteral("No active Graph profile is loaded."));
    }
    controller->synchronizeGraph();
    return operationResult(true, QStringLiteral("Synchronization requested."));
}

QVariantMap DriveBeaconService::beginGraphLogin(const QString &profileName,
                                                const QString &clientId)
{
    const QString name = profileName.trimmed();
    auto *controller = m_controllers.value(name, nullptr);
    if (!controller) {
        // A profile can have been saved by the tray moments before this call.
        // Reload here as a small race-free bridge instead of requiring a tray
        // restart or a second “Use this profile” operation.
        reloadProfiles();
        controller = m_controllers.value(name, nullptr);
    }
    if (!controller || controller->backendName() != QStringLiteral("graph")) {
        return operationResult(false, QStringLiteral("Graph profile is not loaded."));
    }
    if (clientId.trimmed().isEmpty()) {
        return operationResult(false, QStringLiteral("A Graph application client ID is required."));
    }
    controller->beginGraphLogin(clientId.trimmed());
    QVariantMap result = operationResult(true, QStringLiteral("Graph sign-in started."));
    result.insert(QStringLiteral("authorizationUrl"), controller->graphAuthorizationUrl());
    return result;
}

QVariantMap DriveBeaconService::completeGraphLogin(const QString &profileName,
                                                   const QString &responseUrl)
{
    const QString name = profileName.trimmed();
    auto *controller = m_controllers.value(name, nullptr);
    if (!controller) {
        return operationResult(false, QStringLiteral("Graph profile is not loaded."));
    }
    if (responseUrl.trimmed().isEmpty()) {
        return operationResult(false, QStringLiteral("A browser response URL is required."));
    }
    controller->completeGraphLogin(responseUrl.trimmed());
    return operationResult(true, QStringLiteral("Graph sign-in completion requested."));
}

QVariantMap DriveBeaconService::forceRemoteResync(const QString &profileName)
{
    auto *controller = m_controllers.value(profileName.trimmed(), nullptr);
    if (!controller) {
        return operationResult(false, QStringLiteral("Profile is not loaded."));
    }
    controller->forceRemoteResync();
    return operationResult(true, QStringLiteral("Remote resynchronization requested."));
}

QVariantMap DriveBeaconService::refreshGraphFolders()
{
    auto *controller = activeController();
    if (!controller) {
        return operationResult(false, QStringLiteral("No active Graph profile is loaded."));
    }
    controller->refreshGraphFolders();
    return operationResult(true, QStringLiteral("Folder refresh requested."));
}

QVariantMap DriveBeaconService::refreshGraphFoldersForProfile(const QString &profileName)
{
    const QString name = profileName.trimmed();
    auto *controller = m_controllers.value(name, nullptr);
    if (!controller) {
        return operationResult(false, QStringLiteral("Profile is not loaded."));
    }
    if (!controller->graphAuthenticated()) {
        return operationResult(false, QStringLiteral("Graph profile is not authenticated."));
    }
    controller->refreshGraphFolders();
    return operationResult(true, QStringLiteral("Folder refresh requested."));
}

QStringList DriveBeaconService::graphRemoteFolders(const QString &profileName) const
{
    if (auto *controller = m_controllers.value(profileName.trimmed(), nullptr)) {
        return controller->graphRemoteFolders();
    }
    return {};
}

QVariantMap DriveBeaconService::setProfileSyncEnabled(const QString &profileName, bool enabled)
{
    auto *controller = m_controllers.value(profileName.trimmed(), nullptr);
    if (!controller) {
        return operationResult(false, QStringLiteral("Profile is not loaded."));
    }
    controller->setGraphSyncEnabled(enabled);
    publishStatus();
    return operationResult(true, enabled ? QStringLiteral("Profile resumed.")
                                         : QStringLiteral("Profile paused."));
}

QVariantMap DriveBeaconService::setGlobalSyncEnabled(bool enabled)
{
    m_profileStore.setGlobalSyncEnabled(enabled);
    for (auto *controller : m_controllers) {
        controller->setGlobalGraphSyncEnabled(enabled);
    }
    publishStatus();
    return operationResult(true, enabled ? QStringLiteral("Global synchronization resumed.")
                                         : QStringLiteral("Global synchronization paused."));
}

QVariantMap DriveBeaconService::setPrimaryProfile(const QString &profileName)
{
    const QString name = profileName.trimmed();
    if (!m_controllers.contains(name)) {
        return operationResult(false, QStringLiteral("Profile is not loaded."));
    }
    // The primary account is a presentation preference, not a synchronization
    // switch; changing it must not restart controllers or reset their state.
    m_activeProfileName = name;
    m_requestedProfileName.clear();
    m_profileStore.setActiveProfileName(name);
    Q_EMIT statusChanged();
    return operationResult(true, QStringLiteral("Primary profile changed."));
}

void DriveBeaconService::publishStatus()
{
    Q_EMIT statusChanged();
}
