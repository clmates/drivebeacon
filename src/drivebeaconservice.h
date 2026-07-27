// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "onedrivecontroller.h"

#include <QObject>
#include <QHash>
#include <QVariantMap>
#include <QProcess>

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
    Q_PROPERTY(QString profileName READ profileName NOTIFY statusChanged)
    Q_PROPERTY(QString backendName READ backendName CONSTANT)
    /** Names of all Graph profiles loaded by this service instance. */
    Q_PROPERTY(QStringList graphProfiles READ graphProfiles NOTIFY statusChanged)
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
    /** Unmounts profile FUSE views owned by the service before shutdown. */
    ~DriveBeaconService() override;

    [[nodiscard]] QString profileName() const;
    [[nodiscard]] QString backendName() const;
    /** Returns the sorted Graph profile names loaded by the service. */
    [[nodiscard]] QStringList graphProfiles() const;
    [[nodiscard]] QString syncStatus() const;
    [[nodiscard]] int syncProgress() const;
    [[nodiscard]] bool graphAuthenticated() const;
    [[nodiscard]] QString errorMessage() const;
    [[nodiscard]] bool graphSyncEnabled() const;
    [[nodiscard]] bool globalSyncEnabled() const;

public Q_SLOTS:
    /** Requests an immediate Graph synchronization pass. */
    void synchronizeGraph();
    /** Rebuilds one profile's selected local tree from the remote drive. */
    void forceRemoteResync(const QString &profileName);
    /** Requests a fresh first-level folder listing. */
    void refreshGraphFolders();
    /** Enables or pauses one named Graph profile without deleting its state. */
    void setProfileSyncEnabled(const QString &profileName, bool enabled);
    /** Pauses or resumes all Graph profiles while preserving their own flags. */
    void setGlobalSyncEnabled(bool enabled);
    /** Discovers newly saved Graph profiles without restarting the service. */
    void reloadProfiles();
    /** Persists the account whose summary is shown in the tray header. */
    void setPrimaryProfile(const QString &profileName);
    /** Returns status fields for one loaded profile for CLI and tray clients. */
    Q_INVOKABLE QVariantMap profileStatus(const QString &profileName) const;
    /** Returns remote path metadata cached by one Graph controller. */
    Q_INVOKABLE QVariantList remoteEntries(const QString &profileName) const;
    /** Requests content for one remote-only/on-demand path. */
    void materializeFile(const QString &profileName, const QString &relativePath);
    /** Marks one file/folder KeepLocal and materializes folder descendants. */
    void keepLocalPath(const QString &profileName, const QString &relativePath);
    /** Releases one cached file or folder without changing remote content. */
    void evictPath(const QString &profileName, const QString &relativePath);
    /** Mounts one profile's read-only FUSE view at its configured user path. */
    void mountProfile(const QString &profileName);
    /** Unmounts one profile without changing synchronization or remote state. */
    void unmountProfile(const QString &profileName);
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
    /** Optional command-line profile override retained across reloads. */
    QString m_requestedProfileName;
    /** One foreground FUSE helper is owned by the service for each mounted profile. */
    QHash<QString, QProcess *> m_fuseProcesses;
};
