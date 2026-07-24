// SPDX-License-Identifier: GPL-3.0-only

#include "drivebeaconservice.h"

DriveBeaconService::DriveBeaconService(const QString &profileName, QObject *parent)
    : QObject(parent)
    , m_controller(profileName, {}, {}, this)
{
    connect(&m_controller, &OneDriveController::graphSyncChanged,
            this, &DriveBeaconService::publishStatus);
    connect(&m_controller, &OneDriveController::graphAuthChanged,
            this, &DriveBeaconService::publishStatus);
    connect(&m_controller, &OneDriveController::logMessage,
            this, &DriveBeaconService::activityMessage);
    connect(&m_controller, &OneDriveController::errorMessageChanged,
            this, &DriveBeaconService::publishStatus);
}

QString DriveBeaconService::profileName() const
{
    return m_controller.profileName();
}

QString DriveBeaconService::syncStatus() const
{
    return m_controller.graphSyncStatus();
}

int DriveBeaconService::syncProgress() const
{
    return m_controller.graphSyncProgress();
}

bool DriveBeaconService::graphAuthenticated() const
{
    return m_controller.graphAuthenticated();
}

QString DriveBeaconService::errorMessage() const
{
    return m_controller.errorMessage();
}

void DriveBeaconService::synchronizeGraph()
{
    m_controller.synchronizeGraph();
}

void DriveBeaconService::refreshGraphFolders()
{
    m_controller.refreshGraphFolders();
}

void DriveBeaconService::startLegacyService()
{
    m_controller.startService();
}

void DriveBeaconService::stopLegacyService()
{
    m_controller.stopService();
}

void DriveBeaconService::restartLegacyService()
{
    m_controller.restartService();
}

void DriveBeaconService::publishStatus()
{
    Q_EMIT statusChanged();
}
