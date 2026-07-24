// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "activitymodel.h"
#include "deviceloginauth.h"
#include "graphclient.h"
#include "journalreader.h"
#include "profilestore.h"
#include "storagequota.h"
#include "syncprofile.h"
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
    Q_PROPERTY(QString profileName READ profileName CONSTANT)
    Q_PROPERTY(QString backendName READ backendName NOTIFY profileChanged)
    Q_PROPERTY(QString availabilityName READ availabilityName NOTIFY profileChanged)
    Q_PROPERTY(qint64 remoteQuotaTotal READ remoteQuotaTotal NOTIFY remoteQuotaChanged)
    Q_PROPERTY(qint64 remoteQuotaUsed READ remoteQuotaUsed NOTIFY remoteQuotaChanged)
    Q_PROPERTY(qint64 remoteQuotaRemaining READ remoteQuotaRemaining NOTIFY remoteQuotaChanged)
    Q_PROPERTY(QDateTime remoteQuotaUpdatedAt READ remoteQuotaUpdatedAt NOTIFY remoteQuotaChanged)
    Q_PROPERTY(bool remoteQuotaStale READ remoteQuotaStale NOTIFY remoteQuotaChanged)
    Q_PROPERTY(bool graphAuthenticated READ graphAuthenticated NOTIFY graphAuthChanged)
    Q_PROPERTY(QString graphVerificationUri READ graphVerificationUri NOTIFY graphAuthChanged)
    Q_PROPERTY(QString graphUserCode READ graphUserCode NOTIFY graphAuthChanged)
    Q_PROPERTY(QString graphErrorMessage READ graphErrorMessage NOTIFY graphAuthChanged)
    Q_PROPERTY(QString graphAuthorizationUrl READ graphAuthorizationUrl NOTIFY graphAuthChanged)
    Q_PROPERTY(QString graphSyncStatus READ graphSyncStatus NOTIFY graphSyncChanged)
    Q_PROPERTY(int graphSyncProgress READ graphSyncProgress NOTIFY graphSyncChanged)
    Q_PROPERTY(QStringList graphRemoteFolders READ graphRemoteFolders NOTIFY graphRemoteFoldersChanged)

public:
    /** Creates the backend objects, loads configuration, and starts journal monitoring. */
    explicit OneDriveController(const QString &profileName = {},
                                const QString &backendOverride = {},
                                const QString &directoryOverride = {},
                                QObject *parent = nullptr);

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
    /** Returns the configured backend identifier. */
    [[nodiscard]] QString backendName() const;
    /** Returns the name of the isolated profile loaded by this process. */
    [[nodiscard]] QString profileName() const;
    /** Returns the persistent profile store used by the configuration dialog. */
    [[nodiscard]] ProfileStore *profileStore();
    /** Returns the configured local availability policy. */
    [[nodiscard]] QString availabilityName() const;
    /** Returns the last reported remote capacity in bytes. */
    [[nodiscard]] qint64 remoteQuotaTotal() const;
    /** Returns bytes reported as used by the remote drive. */
    [[nodiscard]] qint64 remoteQuotaUsed() const;
    /** Returns bytes still available in the remote drive. */
    [[nodiscard]] qint64 remoteQuotaRemaining() const;
    /** Returns when the remote quota was last refreshed. */
    [[nodiscard]] QDateTime remoteQuotaUpdatedAt() const;
    /** Returns whether the displayed quota could not be refreshed. */
    [[nodiscard]] bool remoteQuotaStale() const;
    /** Returns whether this profile is allowed to control onedrive.service. */
    [[nodiscard]] bool serviceControlAvailable() const;
    /** Returns whether a Graph access token is currently held in memory. */
    [[nodiscard]] bool graphAuthenticated() const;
    /** Returns the legacy Device Code URL, if a provider supplies one. */
    [[nodiscard]] QString graphVerificationUri() const;
    /** Returns the legacy Device Code user code, if a provider supplies one. */
    [[nodiscard]] QString graphUserCode() const;
    /** Returns the last Graph authentication error shown in the profile dialog. */
    [[nodiscard]] QString graphErrorMessage() const;
    /** Returns the browser URL for the current interactive OAuth request. */
    [[nodiscard]] QString graphAuthorizationUrl() const;
    /** Returns the current Graph synchronization status. */
    [[nodiscard]] QString graphSyncStatus() const;
    /** Returns the current remote-to-local synchronization percentage. */
    [[nodiscard]] int graphSyncProgress() const;
    /** Returns the first-level folders discovered in the signed-in drive. */
    [[nodiscard]] QStringList graphRemoteFolders() const;

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
    /** Starts browser OAuth authentication for the configured public client. */
    Q_INVOKABLE void beginGraphLogin(const QString &clientId);
    /** Cancels an active Graph authentication flow. */
    Q_INVOKABLE void cancelGraphLogin();
    /** Exchanges the browser redirect URL for Graph tokens. */
    Q_INVOKABLE void completeGraphLogin(const QString &responseUrl);
    /** Starts a remote-to-local Graph synchronization for the active profile. */
    Q_INVOKABLE void synchronizeGraph();
    /** Refreshes the first-level remote folder list for profile configuration. */
    Q_INVOKABLE void refreshGraphFolders();
    /** Confirms migration of the discovered abraunegg configuration. */
    Q_INVOKABLE void confirmLegacyMigration(const QString &profileName);
    /** Leaves the discovered legacy configuration untouched and continues. */
    Q_INVOKABLE void dismissLegacyMigration();

Q_SIGNALS:
    /** Emitted whenever the service state or sub-state changes. */
    void stateChanged();
    /** Emitted when the effective error message changes. */
    void errorMessageChanged();
    /** Emitted when configuration discovery finds a different sync directory. */
    void syncDirectoryChanged();
    /** Emitted when profile or remote capacity data changes. */
    void profileChanged();
    /** Emitted when any remote quota field changes. */
    void remoteQuotaChanged();
    /** Emitted when Graph authentication state or user instructions change. */
    void graphAuthChanged();
    /** Emitted when Graph synchronization status or progress changes. */
    void graphSyncChanged();
    /** Emitted when the remote folder choices available to the profile editor change. */
    void graphRemoteFoldersChanged();
    /** Emitted when a 0.1.x abraunegg configuration needs a profile name. */
    void legacyProfileDetected(const QString &localDirectory);

private:
    /** Runs onedrive's configuration display command and extracts sync_dir. */
    void loadConfiguration();
    /** Starts journal monitoring after profile initialization or migration. */
    void startJournalBackend();
    /** Stores a journal error and notifies QML when it changes. */
    void setJournalError(const QString &message);

    ActivityModel m_activities;
    ProfileStore m_profileStore;
    SyncProfile m_profile;
    DeviceLoginAuth m_graphAuth;
    GraphClient m_graphClient;
    JournalReader m_journalReader;
    SystemdManager m_systemdManager;
    QProcess m_configProcess;
    QString m_syncDirectory;
    QString m_journalError;
    StorageQuota m_remoteQuota;
    OAuthTokens m_graphTokens;
    QString m_graphVerificationUri;
    QString m_graphUserCode;
    QString m_graphErrorMessage;
    QString m_graphAuthorizationUrl;
    QString m_graphSyncStatus = QStringLiteral("Idle");
    int m_graphSyncProgress = 0;
    QStringList m_graphRemoteFolders;
    QString m_legacyDirectory;
    bool m_legacyMigrationPending = false;
};
