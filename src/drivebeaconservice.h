// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "onedrivecontroller.h"

#include <QObject>
#include <QHash>

/**
 * Headless D-Bus facade for the synchronization controller.
 *
 * The service owns one controller per configured Graph profile. Each controller
 * retains its own token, delta cursor, local baseline, and transfer queues;
 * the active profile is only the compatibility view exposed to the tray/CLI.
 */
class DriveBeaconService final : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.clmates.DriveBeacon1")
    Q_PROPERTY(QString profileName READ profileName CONSTANT)
    Q_PROPERTY(QString backendName READ backendName CONSTANT)
    Q_PROPERTY(QString syncStatus READ syncStatus NOTIFY statusChanged)
    Q_PROPERTY(int syncProgress READ syncProgress NOTIFY statusChanged)
    Q_PROPERTY(bool graphAuthenticated READ graphAuthenticated NOTIFY statusChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY statusChanged)
    /** Whether the active profile is currently allowed to synchronize. */
    Q_PROPERTY(bool graphSyncEnabled READ graphSyncEnabled NOTIFY statusChanged)
    /** Whether the global Graph synchronization switch is enabled. */
    Q_PROPERTY(bool globalSyncEnabled READ globalSyncEnabled NOTIFY statusChanged)

public:
    /** Creates the service facade and starts enabled Graph profiles. */
    explicit DriveBeaconService(const QString &profileName, QObject *parent = nullptr);

    [[nodiscard]] QString profileName() const;
    [[nodiscard]] QString backendName() const;
    [[nodiscard]] QString syncStatus() const;
    [[nodiscard]] int syncProgress() const;
    [[nodiscard]] bool graphAuthenticated() const;
    [[nodiscard]] QString errorMessage() const;
    [[nodiscard]] bool graphSyncEnabled() const;
    [[nodiscard]] bool globalSyncEnabled() const;

public Q_SLOTS:
    /** Requests an immediate Graph synchronization pass. */
    void synchronizeGraph();
    /** Requests a fresh first-level folder listing. */
    void refreshGraphFolders();
    /** Enables or pauses one named Graph profile without deleting its state. */
    void setProfileSyncEnabled(const QString &profileName, bool enabled);
    /** Pauses or resumes all Graph profiles while preserving their own flags. */
    void setGlobalSyncEnabled(bool enabled);
    /** Controls the legacy onedrive.service through the existing manager. */
    void startLegacyService();
    void stopLegacyService();
    void restartLegacyService();

Q_SIGNALS:
    /** Emitted when status, progress, or authentication changes. */
    void statusChanged();
    /** Emitted for activity messages consumed by future tray clients. */
    void activityMessage(const QString &message);

private:
    /** Forwards controller state changes without exposing Qt UI objects. */
    void publishStatus();
    /** Returns the profile currently used by compatibility D-Bus properties. */
    [[nodiscard]] OneDriveController *activeController() const;

    ProfileStore m_profileStore;
    QHash<QString, OneDriveController *> m_controllers;
    QString m_activeProfileName;
};
