// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QObject>
#include <QTimer>

/** Reads and controls the user's onedrive.service through the session D-Bus. */
class SystemdManager final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString activeState READ activeState NOTIFY stateChanged)
    Q_PROPERTY(QString subState READ subState NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)

public:
    /** Creates the manager and starts periodic service-state polling. */
    explicit SystemdManager(QObject *parent = nullptr);

    /** Returns systemd's high-level service state. */
    [[nodiscard]] QString activeState() const;
    /** Returns systemd's more specific service sub-state. */
    [[nodiscard]] QString subState() const;
    /** Returns the last D-Bus or systemd error, if any. */
    [[nodiscard]] QString errorMessage() const;

    /** Reads the current service state from systemd. */
    Q_INVOKABLE void refresh();
    /** Requests that systemd start the OneDrive service. */
    Q_INVOKABLE void startService();
    /** Requests that systemd stop the OneDrive service. */
    Q_INVOKABLE void stopService();
    /** Requests that systemd restart the OneDrive service. */
    Q_INVOKABLE void restartService();

Q_SIGNALS:
    /** Emitted when either systemd state property changes. */
    void stateChanged();
    /** Emitted when the D-Bus error message changes. */
    void errorMessageChanged();

private:
    /** Sends a start/stop/restart request and refreshes state when it completes. */
    void callManager(const QString &method);
    /** Updates the error property and notifies QML only when it changes. */
    void setErrorMessage(const QString &message);

    QString m_activeState = QStringLiteral("unknown");
    QString m_subState;
    QString m_errorMessage;
    QTimer m_refreshTimer;
};
