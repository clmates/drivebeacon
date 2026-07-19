// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "activitymodel.h"
#include "journalreader.h"
#include "systemdmanager.h"

#include <QObject>
#include <QProcess>

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
    explicit OneDriveController(QObject *parent = nullptr);

    [[nodiscard]] ActivityModel *activities();
    [[nodiscard]] QString activeState() const;
    [[nodiscard]] QString subState() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorMessage() const;
    [[nodiscard]] QString syncDirectory() const;

    Q_INVOKABLE void startService();
    Q_INVOKABLE void stopService();
    Q_INVOKABLE void restartService();
    Q_INVOKABLE void openActivityPath(const QString &relativePath) const;
    Q_INVOKABLE void clearError();

Q_SIGNALS:
    void stateChanged();
    void errorMessageChanged();
    void syncDirectoryChanged();

private:
    void loadConfiguration();
    void setJournalError(const QString &message);

    ActivityModel m_activities;
    JournalReader m_journalReader;
    SystemdManager m_systemdManager;
    QProcess m_configProcess;
    QString m_syncDirectory;
    QString m_journalError;
};
