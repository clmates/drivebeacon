// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "onedrivecontroller.h"

#include <QObject>

/**
 * Headless D-Bus facade for the synchronization controller.
 *
 * The first service iteration deliberately reuses OneDriveController so
 * authentication, profiles, Graph state, and journald logging remain single-
 * sourced while the tray is migrated to a separate client process.
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

public:
    /** Creates the service facade for one isolated synchronization profile. */
    explicit DriveBeaconService(const QString &profileName, QObject *parent = nullptr);

    [[nodiscard]] QString profileName() const;
    [[nodiscard]] QString backendName() const;
    [[nodiscard]] QString syncStatus() const;
    [[nodiscard]] int syncProgress() const;
    [[nodiscard]] bool graphAuthenticated() const;
    [[nodiscard]] QString errorMessage() const;

public Q_SLOTS:
    /** Requests an immediate Graph synchronization pass. */
    void synchronizeGraph();
    /** Requests a fresh first-level folder listing. */
    void refreshGraphFolders();
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

    OneDriveController m_controller;
};
