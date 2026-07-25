// SPDX-License-Identifier: GPL-3.0-only

#include "drivebeaconservice.h"

#include <algorithm>

DriveBeaconService::DriveBeaconService(const QString &profileName, QObject *parent)
    : QObject(parent)
    , m_profileStore(this)
    , m_activeProfileName(profileName.isEmpty() ? m_profileStore.activeProfileName()
                                                 : profileName)
{
    const bool globallyEnabled = m_profileStore.globalSyncEnabled();
    for (const QString &name : m_profileStore.profileNames()) {
        const SyncProfile profile = m_profileStore.load(name);
        if (profile.backend != SyncBackend::MicrosoftGraph) {
            continue;
        }
        // A paused profile remains configured and keeps its persisted state;
        // its controller still owns authentication, but its Graph polling and
        // transfer schedulers remain stopped until the profile is resumed.
        auto *controller = new OneDriveController(name, {}, {}, true, false, this);
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
    status.insert(QStringLiteral("authenticated"), controller->graphAuthenticated());
    status.insert(QStringLiteral("syncEnabled"), controller->graphSyncEnabled());
    status.insert(QStringLiteral("syncStatus"), controller->graphSyncStatus());
    status.insert(QStringLiteral("syncProgress"), controller->graphSyncProgress());
    status.insert(QStringLiteral("error"), controller->errorMessage());
    return status;
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

void DriveBeaconService::setGlobalSyncEnabled(bool enabled)
{
    m_profileStore.setGlobalSyncEnabled(enabled);
    for (auto *controller : m_controllers) {
        controller->setGlobalGraphSyncEnabled(enabled);
    }
    publishStatus();
}

void DriveBeaconService::startLegacyService()
{
    if (auto *controller = activeController()) {
        controller->startService();
    }
}

void DriveBeaconService::stopLegacyService()
{
    if (auto *controller = activeController()) {
        controller->stopService();
    }
}

void DriveBeaconService::restartLegacyService()
{
    if (auto *controller = activeController()) {
        controller->restartService();
    }
}

void DriveBeaconService::publishStatus()
{
    Q_EMIT statusChanged();
}
