// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QObject>
#include <QVariantList>

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
    [[nodiscard]] bool graphSyncEnabled() const;
    [[nodiscard]] bool globalSyncEnabled() const;

public Q_SLOTS:
    /** Refreshes service availability and all exposed status properties. */
    void refresh();
    /** Requests a synchronization pass from the service. */
    void synchronizeGraph();
    /** Requests a non-destructive remote folder refresh. */
    void refreshGraphFolders();
    /** Pauses or resumes one named profile through the service. */
    void setProfileSyncEnabled(const QString &profileName, bool enabled);
    /** Pauses or resumes all profiles through the service. */
    void setGlobalSyncEnabled(bool enabled);

Q_SIGNALS:
    /** Emitted when the service appears or disappears from the session bus. */
    void availabilityChanged();
    /** Emitted when a remote status property changes. */
    void statusChanged();
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

    bool m_available = false;
    QString m_syncStatus = QStringLiteral("Service unavailable");
    int m_syncProgress = 0;
    bool m_graphAuthenticated = false;
    QString m_errorMessage;
    bool m_graphSyncEnabled = false;
    bool m_globalSyncEnabled = true;
};
