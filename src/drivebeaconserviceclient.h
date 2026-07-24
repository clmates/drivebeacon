// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QObject>

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

public:
    /** Creates a proxy and begins watching the well-known service name. */
    explicit DriveBeaconServiceClient(QObject *parent = nullptr);

    [[nodiscard]] bool available() const;
    [[nodiscard]] QString syncStatus() const;
    [[nodiscard]] int syncProgress() const;
    [[nodiscard]] bool graphAuthenticated() const;
    [[nodiscard]] QString errorMessage() const;

public Q_SLOTS:
    /** Refreshes service availability and all exposed status properties. */
    void refresh();
    /** Requests a synchronization pass from the service. */
    void synchronizeGraph();
    /** Requests a non-destructive remote folder refresh. */
    void refreshGraphFolders();

Q_SIGNALS:
    /** Emitted when the service appears or disappears from the session bus. */
    void availabilityChanged();
    /** Emitted when a remote status property changes. */
    void statusChanged();
    /** Emitted when an action cannot be sent to the service. */
    void errorOccurred(const QString &message);

private Q_SLOTS:
    /** Re-reads properties after the service emits its status signal. */
    void onRemoteStatusChanged();

private:
    /** Calls one no-argument service method and reports D-Bus errors. */
    void call(const QString &method);
    /** Updates cached properties from a GetAll reply. */
    void applyProperties(const QVariantMap &properties);

    bool m_available = false;
    QString m_syncStatus = QStringLiteral("Service unavailable");
    int m_syncProgress = 0;
    bool m_graphAuthenticated = false;
    QString m_errorMessage;
};
