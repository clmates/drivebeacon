// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "activitymodel.h"
#include "journalreader.h"
#include "systemdmanager.h"

#include <QObject>
#include <QProcess>

/** QML-facing coordinator for service control, configuration, and activity history. */
class OneDriveController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(ActivityModel *activities READ activities CONSTANT)
    Q_PROPERTY(QString activeState READ activeState NOTIFY stateChanged)
    Q_PROPERTY(QString subState READ subState NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString syncDirectory READ syncDirectory NOTIFY syncDirectoryChanged)

public:
    /** Creates the backend objects, loads configuration, and starts journal monitoring. */
    explicit OneDriveController(QObject *parent = nullptr);

    /** Returns the model containing recent OneDrive activity. */
    [[nodiscard]] ActivityModel *activities();
    /** Returns systemd's high-level service state. */
    [[nodiscard]] QString activeState() const;
    /** Returns systemd's detailed service sub-state. */
    [[nodiscard]] QString subState() const;
    /** Returns a localized human-readable status derived from the active state. */
    [[nodiscard]] QString statusText() const;
    /** Returns the journal error in preference to a service-control error. */
    [[nodiscard]] QString errorMessage() const;
    /** Returns the configured local OneDrive directory. */
    [[nodiscard]] QString syncDirectory() const;

    /** Starts the user's OneDrive systemd service. */
    Q_INVOKABLE void startService();
    /** Stops the user's OneDrive systemd service. */
    Q_INVOKABLE void stopService();
    /** Restarts the user's OneDrive systemd service. */
    Q_INVOKABLE void restartService();
    /** Opens a safe path relative to the configured synchronization directory. */
    Q_INVOKABLE void openActivityPath(const QString &relativePath) const;
    /** Clears the journal error currently shown in the interface. */
    Q_INVOKABLE void clearError();

Q_SIGNALS:
    /** Emitted whenever the service state or sub-state changes. */
    void stateChanged();
    /** Emitted when the effective error message changes. */
    void errorMessageChanged();
    /** Emitted when configuration discovery finds a different sync directory. */
    void syncDirectoryChanged();

private:
    /** Runs onedrive's configuration display command and extracts sync_dir. */
    void loadConfiguration();
    /** Stores a journal error and notifies QML when it changes. */
    void setJournalError(const QString &message);

    ActivityModel m_activities;
    JournalReader m_journalReader;
    SystemdManager m_systemdManager;
    QProcess m_configProcess;
    QString m_syncDirectory;
    QString m_journalError;
};
