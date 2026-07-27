// SPDX-License-Identifier: GPL-3.0-only

#include "onedrivecontroller.h"

#include "activityparser.h"
#include "tokenstore.h"

#include <KLocalizedString>

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QTimer>
#include <QDateTime>
#include <QDebug>
#include <systemd/sd-journal.h>

namespace {
/** Mirrors Graph errors into journald with the same UTC format as GraphClient. */
void journalGraphError(const QString &message)
{
    const QString timestamped = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyyMMdd_HHmmss'Z'")) + QLatin1Char(' ') + message;
    qWarning().noquote() << timestamped;
    sd_journal_send("MESSAGE=%s", qPrintable(timestamped),
                    "PRIORITY=%i", 3,
                    "SYSLOG_IDENTIFIER=%s", "drivebeacon", nullptr);
}
}

OneDriveController::OneDriveController(const QString &profileName,
                                       const QString &backendOverride,
                                       const QString &directoryOverride,
                                       bool autoStartGraphSync,
                                       bool setActiveProfile,
                                       QObject *parent)
    : QObject(parent)
    , m_profileStore(this)
    , m_profile(m_profileStore.load(profileName))
    , m_graphAuth(this)
    , m_graphClient(this)
    , m_syncDirectory(m_profile.localDirectory)
    , m_autoStartGraphSync(autoStartGraphSync)
{
    // Configure materialization before any restored delta or initial scan is
    // started; RemoteOnly must never enqueue local content transfers.
    m_graphClient.setLocalAvailability(m_profile.availability);
    m_graphClient.setPathPolicies(m_profile.graphPathPolicies);
    m_graphClient.setPlaceholderPaths(m_profile.graphPlaceholderPaths);
    m_globalGraphSyncEnabled = m_profileStore.globalSyncEnabled();
    m_graphSyncEnabled = m_autoStartGraphSync && m_profile.syncEnabled
        && m_globalGraphSyncEnabled;
    if (!backendOverride.isEmpty()) {
        m_profile.backend = syncBackendFromName(backendOverride);
    }
    if (!directoryOverride.isEmpty()) {
        m_profile.localDirectory = QDir::cleanPath(QFileInfo(directoryOverride).absoluteFilePath());
        m_syncDirectory = m_profile.localDirectory;
    }
    if ((!profileName.isEmpty() || !backendOverride.isEmpty() || !directoryOverride.isEmpty())
        && setActiveProfile) {
        m_profileStore.save(m_profile);
        m_profileStore.setActiveProfileName(m_profile.name);
    }
    m_legacyMigrationPending = profileName.isEmpty() && backendOverride.isEmpty()
        && directoryOverride.isEmpty() && m_profile.backend == SyncBackend::AbrauneggJournal
        && m_profileStore.profileNames().isEmpty();
    // The controller deliberately owns the backend helpers so QML sees one stable API.
    connect(&m_systemdManager, &SystemdManager::stateChanged,
            this, &OneDriveController::stateChanged);
    connect(&m_systemdManager, &SystemdManager::errorMessageChanged,
            this, &OneDriveController::errorMessageChanged);
    connect(&m_journalReader, &JournalReader::errorOccurred,
            this, &OneDriveController::setJournalError);
    connect(&m_journalReader, &JournalReader::messageReceived, this,
            [this](const QString &message, const QDateTime &timestamp) {
                if (auto event = ActivityParser::parse(message, timestamp)) {
                    m_activities.prepend(std::move(*event));
                }
            });
    connect(&m_graphAuth, &DeviceLoginAuth::browserAuthorizationRequired, this,
            [this](const QUrl &authorizationUrl) {
                m_graphAuthorizationUrl = authorizationUrl.toString(QUrl::FullyEncoded);
                Q_EMIT graphAuthChanged();
            });
    connect(&m_graphAuth, &DeviceLoginAuth::authenticated, this,
            [this](const OAuthTokens &tokens) {
                m_graphTokens = tokens;
                QString walletError;
                const bool stored = TokenStore::save(m_profile.name, tokens, &walletError);
                m_graphErrorMessage.clear();
                if (!stored) {
                    m_graphErrorMessage = walletError;
                }
                m_graphAuthorizationUrl.clear();
                Q_EMIT graphAuthChanged();
                if (m_profile.remoteDriveId.isEmpty()) {
                    m_graphClient.fetchCurrentDrive(m_graphTokens.accessToken);
                } else {
                    m_graphClient.fetchQuota(m_profile.remoteDriveId, m_graphTokens.accessToken);
                    refreshGraphFolders();
                }
            });
    connect(&m_graphAuth, &DeviceLoginAuth::errorOccurred, this,
            [this](const QString &message) {
                m_graphErrorMessage = message;
                // Do not keep polling Graph with the rejected bearer token.
                // The persisted sync flag remains enabled so a successful
                // manual reconnect can resume the profile automatically.
                m_graphTokens = {};
                m_graphClient.stopMonitoring();
                m_graphAuthorizationUrl.clear();
                setJournalError(message);
                Q_EMIT graphAuthChanged();
            });
    connect(&m_graphClient, &GraphClient::quotaReceived, this,
            [this](const StorageQuota &quota) {
                m_remoteQuota = quota;
                Q_EMIT remoteQuotaChanged();
            });
    connect(&m_graphClient, &GraphClient::driveDiscovered, this,
            [this](const QString &driveId, const StorageQuota &quota) {
                m_profile.remoteDriveId = driveId;
                m_profileStore.save(m_profile);
                m_remoteQuota = quota;
                Q_EMIT profileChanged();
                Q_EMIT remoteQuotaChanged();
                refreshGraphFolders();
            });
    connect(&m_graphClient, &GraphClient::rootFoldersReceived, this,
            [this](const QList<GraphRemoteFolder> &folders) {
                m_graphRemoteFolders.clear();
                for (const GraphRemoteFolder &folder : folders) {
                    m_graphRemoteFolders.append(folder.name);
                }
                Q_EMIT graphRemoteFoldersChanged();
                // Tray clients may inspect authentication and folders while
                // the headless service owns all Graph synchronization. They
                // must not start a second monitor for the same profile.
                if (!m_graphSyncEnabled) {
                    return;
                }
                if (m_autoStartGraphSync && (m_profile.graphLocalSignatures.isEmpty()
                    || m_profile.graphRemotePaths.isEmpty())) {
                    synchronizeGraph();
                } else if (m_profile.graphSyncedIncludedFolders != m_profile.includedFolders
                           || m_profile.graphSyncedExcludedFolders != m_profile.excludedFolders) {
                    // A folder-policy edit is incremental: preserve local
                    // files from deselected folders and enumerate only to
                    // discover existing files in newly selected folders.
                    m_graphClient.initializeLocalMonitoring(m_profile.graphLocalSignatures,
                                                            m_profile.graphRemotePaths,
                                                            m_profile.localDirectory);
                    m_graphClient.configureTransferConcurrency(
                        m_profile.concurrentDownloads, m_profile.concurrentUploads,
                        m_profile.concurrentLargeTransfers);
                    m_graphClient.refreshSelectedFolders(
                        m_profile.remoteDriveId, m_graphTokens.accessToken,
                        m_profile.localDirectory, m_profile.includedFolders,
                        m_profile.excludedFolders);
                } else {
                    // A persisted delta cursor means the local tree already
                    // has a known baseline; do not replay the initial download.
                    m_graphClient.initializeLocalMonitoring(m_profile.graphLocalSignatures,
                                                            m_profile.graphRemotePaths,
                                                            m_profile.localDirectory);
                    m_graphClient.configureTransferConcurrency(
                        m_profile.concurrentDownloads, m_profile.concurrentUploads,
                        m_profile.concurrentLargeTransfers);
                    m_graphClient.startRemoteMonitoring(
                        m_profile.remoteDriveId, m_graphTokens.accessToken,
                        m_profile.remoteCheckIntervalSeconds, m_profile.graphDeltaLink);
                    m_graphSyncStatus = QStringLiteral("Completed");
                    m_graphSyncProgress = 100;
                    Q_EMIT graphSyncChanged();
                }
            });
    connect(&m_graphClient, &GraphClient::errorOccurred, this,
            [this](const QString &message) {
                journalGraphError(QStringLiteral("Graph error: %1").arg(message));
                m_activities.prepend({QDateTime::currentDateTimeUtc(), QStringLiteral("graph-log"),
                                      {}, {}, QStringLiteral("Graph error: %1").arg(message), true});
                setJournalError(message);
                m_graphSyncStatus = QStringLiteral("Error: %1").arg(message);
                Q_EMIT graphSyncChanged();
                if (!m_remoteQuota.stale) {
                    m_remoteQuota.stale = true;
                    Q_EMIT remoteQuotaChanged();
                }
            });
    connect(&m_graphClient, &GraphClient::syncProgress, this,
            [this](int progress, const QString &path) {
                m_graphSyncProgress = progress;
                m_graphSyncStatus = path.isEmpty()
                    ? (progress == 100 ? QStringLiteral("Completed") : QStringLiteral("Preparing"))
                    : path.startsWith(QStringLiteral("Listing "))
                        ? path
                        : path.startsWith(QStringLiteral("Uploading "))
                            ? path
                        : QStringLiteral("Downloading %1").arg(path);
                if (!path.isEmpty() && !path.startsWith(QStringLiteral("Listing "))) {
                    const bool uploading = path.startsWith(QStringLiteral("Uploading "));
                    const QString transferPath = uploading ? path.sliced(10) : path;
                    m_activities.updateGraphProgress(
                        transferPath,
                        QStringLiteral("%1 · %2%").arg(transferPath).arg(progress),
                        progress >= 100);
                }
                Q_EMIT graphSyncChanged();
            });
    connect(&m_graphClient, &GraphClient::syncFinished, this, [this] {
        if (m_forceRemoteResyncPending) {
            // Upload completion also emits syncFinished. Defer the full pull
            // until that callback has released the active transfer state.
            m_forceRemoteResyncPending = false;
            QTimer::singleShot(0, this, &OneDriveController::forceRemoteResync);
            return;
        }
        m_profile.graphSyncedIncludedFolders = m_profile.includedFolders;
        m_profile.graphSyncedExcludedFolders = m_profile.excludedFolders;
        m_profileStore.save(m_profile);
        m_graphSyncProgress = 100;
        m_graphSyncStatus = QStringLiteral("Completed");
        Q_EMIT graphSyncChanged();
        if (m_graphSyncEnabled) {
            m_graphClient.startRemoteMonitoring(m_profile.remoteDriveId,
                                                m_graphTokens.accessToken,
                                                m_profile.remoteCheckIntervalSeconds,
                                                m_profile.graphDeltaLink);
        }
    });
    connect(&m_graphClient, &GraphClient::deltaLinkChanged, this,
            [this](const QString &deltaLink) {
        m_profile.graphDeltaLink = deltaLink;
        m_profileStore.save(m_profile);
    });
    connect(&m_graphClient, &GraphClient::localStateChanged, this,
            [this](const QStringList &signatures, const QStringList &remotePaths) {
        m_profile.graphLocalSignatures = signatures;
        m_profile.graphRemotePaths = remotePaths;
        m_profileStore.save(m_profile);
    });
    connect(&m_graphClient, &GraphClient::placeholderStateChanged, this,
            [this](const QStringList &paths) {
        m_profile.graphPlaceholderPaths = paths;
        m_profileStore.save(m_profile);
    });
    connect(&m_graphClient, &GraphClient::pathPoliciesChanged, this,
            [this](const QStringList &policies) {
        m_profile.graphPathPolicies = policies;
        m_profileStore.save(m_profile);
    });
    connect(&m_graphClient, &GraphClient::logMessage, this,
            [this](const QString &message) {
        m_activities.prepend({QDateTime::currentDateTimeUtc(), QStringLiteral("graph-log"),
                              {}, {}, message, true});
        Q_EMIT logMessage(message);
    });
    // `onedrive --display-config` is asynchronous because configuration lookup may start a process.
    connect(&m_configProcess, &QProcess::finished, this,
            [this](int, QProcess::ExitStatus) {
                const QString output = QString::fromLocal8Bit(m_configProcess.readAllStandardOutput());
                static const QRegularExpression syncDirectoryExpression(
                    QStringLiteral("^Config option 'sync_dir'\\s*=\\s*(.+)$"),
                    QRegularExpression::MultilineOption);
                const auto match = syncDirectoryExpression.match(output);
                if (!match.hasMatch()) {
                    if (m_legacyMigrationPending) {
                        m_legacyMigrationPending = false;
                        startJournalBackend();
                    }
                    return;
                }

                QString path = match.captured(1).trimmed();
                if (path.startsWith(QLatin1String("~/"))) {
                    path = QDir::home().filePath(path.sliced(2));
                }
                path = QDir::cleanPath(path);
                if (path != m_syncDirectory) {
                    m_syncDirectory = QDir::cleanPath(path);
                    Q_EMIT syncDirectoryChanged();
                }
                if (m_legacyMigrationPending) {
                    m_legacyDirectory = m_syncDirectory;
                    Q_EMIT legacyProfileDetected(m_legacyDirectory);
                }
            });

    // The selected backend owns its own lifecycle; Graph profiles must not also follow journalctl.
    if (m_profile.backend == SyncBackend::AbrauneggJournal) {
        loadConfiguration();
        if (!m_legacyMigrationPending) {
            startJournalBackend();
        }
    }

    // Opening a locked wallet can show a desktop prompt, so defer it until the
    // event loop exists. Access tokens remain memory-only; only the refresh token
    // is persisted by TokenStore.
    if (m_profile.backend == SyncBackend::MicrosoftGraph
        && !m_profile.graphClientId.isEmpty()) {
        QTimer::singleShot(0, this, [this] {
            OAuthTokens storedTokens;
            QString walletError;
            if (!TokenStore::load(m_profile.name, &storedTokens, &walletError)) {
                m_graphErrorMessage = walletError.isEmpty()
                    ? QStringLiteral("No stored Microsoft Graph credentials for profile '%1'.")
                          .arg(m_profile.name)
                    : walletError;
                Q_EMIT graphAuthChanged();
                return;
            }
            m_graphAuth.refresh(m_profile.graphClientId, storedTokens.refreshToken);
        });
    }
}

ActivityModel *OneDriveController::activities()
{
    return &m_activities;
}

QString OneDriveController::activeState() const
{
    return m_systemdManager.activeState();
}

QString OneDriveController::subState() const
{
    return m_systemdManager.subState();
}

QString OneDriveController::statusText() const
{
    if (!serviceControlAvailable()) {
        return graphAuthenticated() ? i18n("Microsoft Graph is connected")
                                    : i18n("Microsoft Graph profile is not connected");
    }
    // Keep state-to-text mapping centralized so the tray and QML use the same wording.
    if (activeState() == QLatin1String("active")) {
        return i18n("OneDrive is running");
    }
    if (activeState() == QLatin1String("activating")) {
        return i18n("OneDrive is starting");
    }
    if (activeState() == QLatin1String("deactivating")) {
        return i18n("OneDrive is stopping");
    }
    if (activeState() == QLatin1String("failed")) {
        return i18n("OneDrive has failed");
    }
    if (activeState() == QLatin1String("inactive")) {
        return i18n("OneDrive is stopped");
    }
    if (activeState() == QLatin1String("not-found")) {
        return i18n("OneDrive service was not found");
    }
    return i18n("Checking OneDrive status");
}

QString OneDriveController::errorMessage() const
{
    if (!m_journalError.isEmpty()) {
        return m_journalError;
    }
    if (!m_graphErrorMessage.isEmpty()) {
        return m_graphErrorMessage;
    }
    return m_systemdManager.errorMessage();
}

QString OneDriveController::syncDirectory() const
{
    return m_syncDirectory;
}

QString OneDriveController::backendName() const
{
    return syncBackendName(m_profile.backend);
}

QString OneDriveController::profileName() const
{
    return m_profile.name;
}

bool OneDriveController::usesGraphService() const
{
    return m_profile.backend == SyncBackend::MicrosoftGraph;
}

ProfileStore *OneDriveController::profileStore()
{
    return &m_profileStore;
}

QString OneDriveController::availabilityName() const
{
    return localAvailabilityName(m_profile.availability);
}

qint64 OneDriveController::remoteQuotaTotal() const
{
    return m_remoteQuota.total;
}

qint64 OneDriveController::remoteQuotaUsed() const
{
    return m_remoteQuota.used;
}

qint64 OneDriveController::remoteQuotaRemaining() const
{
    return m_remoteQuota.remaining;
}

QDateTime OneDriveController::remoteQuotaUpdatedAt() const
{
    return m_remoteQuota.lastUpdated;
}

bool OneDriveController::remoteQuotaStale() const
{
    return m_remoteQuota.stale;
}

bool OneDriveController::serviceControlAvailable() const
{
    return m_profile.backend == SyncBackend::AbrauneggJournal;
}

bool OneDriveController::graphAuthenticated() const
{
    return !m_graphTokens.accessToken.isEmpty();
}

QString OneDriveController::graphErrorMessage() const
{
    return m_graphErrorMessage;
}

QString OneDriveController::graphAuthorizationUrl() const
{
    return m_graphAuthorizationUrl;
}

QString OneDriveController::graphSyncStatus() const
{
    return m_graphSyncStatus;
}

int OneDriveController::graphSyncProgress() const
{
    return m_graphSyncProgress;
}

bool OneDriveController::graphSyncEnabled() const
{
    return m_graphSyncEnabled;
}

QStringList OneDriveController::graphRemoteFolders() const
{
    return m_graphRemoteFolders;
}

void OneDriveController::setGraphPathPolicies(const QStringList &policies)
{
    m_graphClient.setPathPolicies(policies);
    m_profile.graphPathPolicies = m_graphClient.pathPolicies();
    m_profileStore.save(m_profile);
}

void OneDriveController::setGraphPathPolicy(const QString &relativePath,
                                            const QString &availability)
{
    const LocalAvailability policy = localAvailabilityFromName(availability.trimmed().toLower());
    if (policy == LocalAvailability::RemoteOnly) {
        setJournalError(QStringLiteral("Remote-only is represented by Release Local cache."));
        return;
    }
    m_graphClient.setPathPolicy(relativePath, policy);
    if (policy == LocalAvailability::KeepLocal) {
        m_graphClient.materializePath(relativePath);
    }
}

QVariantList OneDriveController::graphRemoteEntries() const
{
    return m_graphClient.remoteEntries();
}

void OneDriveController::startService()
{
    if (!serviceControlAvailable()) {
        return;
    }
    m_systemdManager.startService();
}

void OneDriveController::stopService()
{
    if (!serviceControlAvailable()) {
        return;
    }
    m_systemdManager.stopService();
}

void OneDriveController::restartService()
{
    if (!serviceControlAvailable()) {
        return;
    }
    m_systemdManager.restartService();
}

void OneDriveController::openActivityPath(const QString &relativePath) const
{
    // Resolve only inside sync_dir; journal paths are untrusted input for a desktop opener.
    const QString syncRoot = QDir::cleanPath(QFileInfo(m_syncDirectory).absoluteFilePath());
    const QString targetPath = QDir::cleanPath(QDir(syncRoot).filePath(relativePath));
    if (targetPath != syncRoot && !targetPath.startsWith(syncRoot + QDir::separator())) {
        return;
    }

    QFileInfo target(targetPath);
    QFileInfo location = target.exists() && target.isDir() ? target : QFileInfo(target.absolutePath());
    while (!location.exists() && location.absoluteFilePath() != QDir::rootPath()) {
        location = QFileInfo(location.absolutePath());
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(location.absoluteFilePath()));
}

void OneDriveController::clearError()
{
    setJournalError({});
}

void OneDriveController::beginGraphLogin(const QString &clientId)
{
    m_graphErrorMessage.clear();
    m_graphAuthorizationUrl.clear();
    Q_EMIT graphAuthChanged();
    m_graphAuth.start(clientId);
}

void OneDriveController::cancelGraphLogin()
{
    m_graphAuth.cancel();
    m_graphErrorMessage.clear();
    m_graphAuthorizationUrl.clear();
    Q_EMIT graphAuthChanged();
}

void OneDriveController::completeGraphLogin(const QString &responseUrl)
{
    m_graphErrorMessage.clear();
    Q_EMIT graphAuthChanged();
    m_graphAuth.submitAuthorizationResponse(responseUrl);
}

void OneDriveController::synchronizeGraph()
{
    if (m_profile.backend != SyncBackend::MicrosoftGraph || !graphAuthenticated()
        || m_profile.remoteDriveId.isEmpty() || !m_graphSyncEnabled) {
        return;
    }
    m_graphSyncStatus = QStringLiteral("Preparing");
    m_graphSyncProgress = 0;
    Q_EMIT graphSyncChanged();
    // A full tree enumeration establishes a new baseline; the old cursor
    // could replay changes that this enumeration has already applied.
    m_profile.graphDeltaLink.clear();
    m_profileStore.save(m_profile);
    m_graphClient.configureTransferConcurrency(
        m_profile.concurrentDownloads, m_profile.concurrentUploads,
        m_profile.concurrentLargeTransfers);
    m_graphClient.synchronize(m_profile.remoteDriveId, m_graphTokens.accessToken,
                              m_profile.localDirectory, m_profile.includedFolders,
                              m_profile.excludedFolders);
}

void OneDriveController::forceRemoteResync()
{
    if (m_profile.backend != SyncBackend::MicrosoftGraph || !graphAuthenticated()
        || m_profile.remoteDriveId.isEmpty() || !m_graphSyncEnabled) {
        return;
    }
    if (m_graphClient.hasActiveTransfers()) {
        m_forceRemoteResyncPending = true;
        m_graphSyncStatus = QStringLiteral("Waiting for active transfer before remote resync");
        Q_EMIT graphSyncChanged();
        return;
    }
    // Keep the identity index, eTags, and local signatures. The folder walk
    // then downloads only missing or changed files, including nested folders.
    m_graphClient.stopMonitoring();
    m_graphClient.initializeLocalMonitoring(m_profile.graphLocalSignatures,
                                            m_profile.graphRemotePaths,
                                            m_profile.localDirectory);
    m_graphClient.configureTransferConcurrency(
        m_profile.concurrentDownloads, m_profile.concurrentUploads,
        m_profile.concurrentLargeTransfers);
    m_graphClient.refreshSelectedFolders(
        m_profile.remoteDriveId, m_graphTokens.accessToken,
        m_profile.localDirectory, m_profile.includedFolders,
        m_profile.excludedFolders);
}

void OneDriveController::setGraphSyncEnabled(bool enabled)
{
    m_profile.syncEnabled = enabled;
    m_profileStore.save(m_profile);
    m_graphSyncEnabled = enabled && m_autoStartGraphSync && m_globalGraphSyncEnabled;
    if (!m_graphSyncEnabled) {
        m_graphClient.stopMonitoring();
        m_graphSyncStatus = QStringLiteral("Paused");
        m_graphSyncProgress = 0;
        Q_EMIT graphSyncChanged();
        return;
    }
    if (!graphAuthenticated() || m_profile.remoteDriveId.isEmpty()) {
        m_graphSyncStatus = QStringLiteral("Ready");
        Q_EMIT graphSyncChanged();
        return;
    }
    m_graphClient.initializeLocalMonitoring(m_profile.graphLocalSignatures,
                                            m_profile.graphRemotePaths,
                                            m_profile.localDirectory);
    m_graphClient.configureTransferConcurrency(
        m_profile.concurrentDownloads, m_profile.concurrentUploads,
        m_profile.concurrentLargeTransfers);
    if (m_profile.graphLocalSignatures.isEmpty() || m_profile.graphRemotePaths.isEmpty()) {
        synchronizeGraph();
        return;
    }
    m_graphClient.startRemoteMonitoring(
        m_profile.remoteDriveId, m_graphTokens.accessToken,
        m_profile.remoteCheckIntervalSeconds, m_profile.graphDeltaLink);
    m_graphSyncStatus = QStringLiteral("Completed");
    m_graphSyncProgress = 100;
    Q_EMIT graphSyncChanged();
}

void OneDriveController::setAvailability(const QString &availability)
{
    const QString normalized = availability.trimmed().toLower();
    if (normalized != QLatin1String("keep-local")
        && normalized != QLatin1String("remote-only")
        && normalized != QLatin1String("on-demand")) {
        setJournalError(QStringLiteral("Unknown availability policy: %1").arg(availability));
        return;
    }
    const LocalAvailability next = localAvailabilityFromName(normalized);
    if (next == m_profile.availability) {
        return;
    }
    m_profile.availability = next;
    m_profileStore.save(m_profile);
    m_graphClient.setLocalAvailability(next);
    Q_EMIT profileChanged();
    if (m_profile.backend != SyncBackend::MicrosoftGraph || !m_graphSyncEnabled) {
        return;
    }
    m_graphClient.stopMonitoring();
    // Keep the existing cursor and baseline when possible. Switching from
    // RemoteOnly to KeepLocal naturally materializes missing content when the
    // local signature baseline no longer proves the files are present.
    setGraphSyncEnabled(true);
}

void OneDriveController::materializeGraphFile(const QString &relativePath)
{
    if (m_profile.backend != SyncBackend::MicrosoftGraph || !graphAuthenticated()
        || !m_graphSyncEnabled) {
        return;
    }
    m_graphClient.materializePath(relativePath);
}

void OneDriveController::evictGraphPath(const QString &relativePath)
{
    if (m_profile.backend != SyncBackend::MicrosoftGraph || !graphAuthenticated()
        || !m_graphSyncEnabled) {
        return;
    }
    m_graphClient.evictPath(relativePath);
}

void OneDriveController::setGlobalGraphSyncEnabled(bool enabled)
{
    m_globalGraphSyncEnabled = enabled;
    const bool shouldRun = m_profile.syncEnabled && enabled && m_autoStartGraphSync;
    if (shouldRun == m_graphSyncEnabled) {
        return;
    }
    if (!shouldRun) {
        m_graphSyncEnabled = false;
        m_graphClient.stopMonitoring();
        m_graphSyncStatus = QStringLiteral("Paused");
        m_graphSyncProgress = 0;
        Q_EMIT graphSyncChanged();
        return;
    }
    setGraphSyncEnabled(true);
}

void OneDriveController::refreshGraphFolders()
{
    if (m_profile.backend != SyncBackend::MicrosoftGraph || !graphAuthenticated()
        || m_profile.remoteDriveId.isEmpty()) {
        return;
    }
    m_graphClient.fetchRootFolders(m_profile.remoteDriveId, m_graphTokens.accessToken);
}

void OneDriveController::confirmLegacyMigration(const QString &requestedProfileName)
{
    if (!m_legacyMigrationPending || m_legacyDirectory.isEmpty()) {
        return;
    }

    const QString name = requestedProfileName.trimmed().isEmpty()
        ? QStringLiteral("abraunegg")
        : requestedProfileName.trimmed();
    m_profile.name = name;
    m_profile.backend = SyncBackend::AbrauneggJournal;
    m_profile.localDirectory = m_legacyDirectory;
    m_profileStore.save(m_profile);
    m_profileStore.setActiveProfileName(m_profile.name);
    m_legacyMigrationPending = false;
    Q_EMIT profileChanged();
    startJournalBackend();
}

void OneDriveController::dismissLegacyMigration()
{
    if (!m_legacyMigrationPending) {
        return;
    }
    m_legacyMigrationPending = false;
    startJournalBackend();
}

void OneDriveController::loadConfiguration()
{
    // The CLI is the authoritative source for the effective OneDrive sync directory.
    m_configProcess.start(QStringLiteral("onedrive"), {QStringLiteral("--display-config")});
}

void OneDriveController::startJournalBackend()
{
    if (m_profile.backend == SyncBackend::AbrauneggJournal) {
        m_journalReader.start();
    }
}

void OneDriveController::setJournalError(const QString &message)
{
    if (message == m_journalError) {
        return;
    }
    m_journalError = message;
    Q_EMIT errorMessageChanged();
}
