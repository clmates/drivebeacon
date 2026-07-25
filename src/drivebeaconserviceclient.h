// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QObject>
#include <QHash>
#include <QVariantList>
#include <QVariantMap>

/**
 * Client-side proxy for the headless DriveBeacon D-Bus service.
 *
 * The proxy is intentionally independent of OneDriveController so the tray
 * can migrate its status and actions without importing synchronization logic.
 */
class DriveBeaconServiceClient final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(QString syncStatus READ syncStatus NOTIFY statusChanged)
    Q_PROPERTY(int syncProgress READ syncProgress NOTIFY statusChanged)
    Q_PROPERTY(bool graphAuthenticated READ graphAuthenticated NOTIFY statusChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY statusChanged)
    Q_PROPERTY(QStringList graphProfiles READ graphProfiles NOTIFY profilesChanged)
    /** Account whose status is presented in the tray header. */
    Q_PROPERTY(QString primaryProfileName READ primaryProfileName NOTIFY statusChanged)
    /** Cached pause state of the active service profile. */
    Q_PROPERTY(bool graphSyncEnabled READ graphSyncEnabled NOTIFY statusChanged)
    /** Cached global pause state reported by the service. */
    Q_PROPERTY(bool globalSyncEnabled READ globalSyncEnabled NOTIFY statusChanged)

public:
    /** Creates a proxy and begins watching the well-known service name. */
    explicit DriveBeaconServiceClient(QObject *parent = nullptr);

    [[nodiscard]] bool available() const;
    [[nodiscard]] QString syncStatus() const;
    [[nodiscard]] int syncProgress() const;
    [[nodiscard]] bool graphAuthenticated() const;
    [[nodiscard]] QString errorMessage() const;
    /** Returns the persisted account selected as the tray's primary account. */
    [[nodiscard]] QString primaryProfileName() const;
    /** Returns the loaded Graph profile names cached from the service. */
    [[nodiscard]] QStringList graphProfiles() const;
    /** Returns the last status snapshot for one loaded profile. */
    [[nodiscard]] QVariantMap profileStatus(const QString &profileName) const;
    [[nodiscard]] bool graphSyncEnabled() const;
    [[nodiscard]] bool globalSyncEnabled() const;

public Q_SLOTS:
    /** Refreshes service availability and all exposed status properties. */
    void refresh();
    /** Requests a synchronization pass from the service. */
    void synchronizeGraph();
    /** Rebuilds one named profile locally from its selected remote tree. */
    void forceRemoteResync(const QString &profileName);
    /** Requests a non-destructive remote folder refresh. */
    void refreshGraphFolders();
    /** Pauses or resumes one named profile through the service. */
    void setProfileSyncEnabled(const QString &profileName, bool enabled);
    /** Changes one named profile's local availability through the service. */
    void setProfileAvailability(const QString &profileName, const QString &availability);
    /** Pauses or resumes all profiles through the service. */
    void setGlobalSyncEnabled(bool enabled);
    /** Requests discovery of profiles saved after the service started. */
    void reloadProfiles();
    /** Changes the persisted account used by the tray summary. */
    void setPrimaryProfile(const QString &profileName);

Q_SIGNALS:
    /** Emitted when the service appears or disappears from the session bus. */
    void availabilityChanged();
    /** Emitted when a remote status property changes. */
    void statusChanged();
    /** Emitted when the service adds or removes a loaded profile. */
    void profilesChanged();
    /** Emitted when an action cannot be sent to the service. */
    void errorOccurred(const QString &message);
    /** Emitted for synchronization log messages forwarded by the service. */
    void activityMessage(const QString &message);

private Q_SLOTS:
    /** Re-reads properties after the service emits its status signal. */
    void onRemoteStatusChanged();
    /** Receives one activity signal from the service's D-Bus interface. */
    void onRemoteActivityMessage(const QString &message);

private:
    /** Calls a service method and reports D-Bus errors to the tray. */
    void call(const QString &method, const QVariantList &arguments = {});
    /** Updates cached properties from a GetAll reply. */
    void applyProperties(const QVariantMap &properties);
    /** Refreshes per-profile snapshots after the service profile list changes. */
    void refreshProfileStatuses();

    bool m_available = false;
    QString m_syncStatus = QStringLiteral("Service unavailable");
    int m_syncProgress = 0;
    bool m_graphAuthenticated = false;
    QString m_errorMessage;
    QString m_primaryProfileName;
    bool m_graphSyncEnabled = false;
    bool m_globalSyncEnabled = true;
    QStringList m_graphProfiles;
    QHash<QString, QVariantMap> m_profileStatuses;
};
