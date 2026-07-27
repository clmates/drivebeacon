// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "storagequota.h"
#include "syncprofile.h"

#include <QNetworkAccessManager>
#include <QUrl>
#include <QSaveFile>
#include <QObject>
#include <QQueue>
#include <QHash>
#include <QDateTime>
#include <QTimer>
#include <QSet>
#include <QPointer>
#include <QVariantList>

#include <memory>

class QNetworkReply;
class QJsonObject;

/** Remote folder queued while walking the initial tree enumeration. */
struct GraphSyncFolder {
    /** Stable driveItem identifier used for the children request. */
    QString id;
    /** Slash-separated path relative to the local synchronization root. */
    QString relativePath;
};

/** Remote file queued for download during an initial or delta synchronization. */
struct GraphSyncFile {
    /** Stable driveItem identifier used to fetch file content. */
    QString id;
    /** Slash-separated path relative to the local synchronization root. */
    QString relativePath;
    /** Remote size used to reserve the large-transfer concurrency slot. */
    qint64 size = -1;
};

/** Local file waiting to be uploaded after the polling scan detects a change. */
struct GraphLocalFile {
    /** Path used as the remote item name. */
    QString relativePath;
    /** Absolute path on the local filesystem. */
    QString localPath;
};

/** Remote item deletion requested because its local counterpart disappeared. */
struct GraphRemoteDelete {
    /** Path used to identify the item in the local baseline. */
    QString relativePath;
    /** Stable Graph item ID; avoids deleting a newly recreated same-named file. */
    QString itemId;
};

/** Remote item rename requested after a local rename was detected by content hash. */
struct GraphRemoteRename {
    /** Previous path in the persisted baseline. */
    QString oldPath;
    /** New path in the same parent directory. */
    QString newPath;
    /** Stable Graph item ID whose name should be patched. */
    QString itemId;
};

/** Describes a folder directly below the remote drive root for profile setup. */
struct GraphRemoteFolder {
    /** Stable folder ID used when traversing the remote tree. */
    QString id;
    /** Display name and first-level profile selection value. */
    QString name;
};

/**
 * Graph transport and synchronization state machine.
 *
 * The client keeps a content-hash baseline for local files and an ID/path/eTag
 * baseline for remote files. Queues serialize mutations so a local operation
 * cannot race a delta operation that is still updating the same item.
 */
class GraphClient final : public QObject
{
    Q_OBJECT

public:
    /** Creates a client with an injectable network manager parent. */
    explicit GraphClient(QObject *parent = nullptr);

    /** Requests quota for a drive using an already acquired bearer token. */
    void fetchQuota(const QString &driveId, const QString &accessToken);
    /** Discovers the signed-in user's default drive and its quota. */
    void fetchCurrentDrive(const QString &accessToken);
    /** Lists folders directly below the drive root for the profile editor. */
    void fetchRootFolders(const QString &driveId, const QString &accessToken);
    /** Enumerates the remote tree and reconciles it with the service cache. */
    void synchronize(const QString &driveId, const QString &accessToken,
                     const QString &localDirectory, const QStringList &includedFolders,
                     const QStringList &excludedFolders);
    /** Re-enumerates selected folders against the service cache without resetting the baseline. */
    void refreshSelectedFolders(const QString &driveId, const QString &accessToken,
                               const QString &localDirectory,
                               const QStringList &includedFolders,
                               const QStringList &excludedFolders);
    /** Starts delta-based remote change checks using the configured interval. */
    void startRemoteMonitoring(const QString &driveId, const QString &accessToken,
                               int intervalSeconds, const QString &deltaLink = {});
    /** Stops polling without discarding queues, cursors, or local baselines. */
    void stopMonitoring();
    /** Applies per-profile transfer limits before synchronization starts. */
    void configureTransferConcurrency(int downloads, int uploads, int largeTransfers);
    /** Sets the profile-wide fallback for materializing content in the service cache. */
    void setLocalAvailability(LocalAvailability availability);
    /** Applies persisted path overrides; descendants inherit the nearest rule. */
    void setPathPolicies(const QStringList &policies);
    /** Returns path policies in the persisted path<TAB>availability format. */
    [[nodiscard]] QStringList pathPolicies() const;
    /** Sets one file/folder policy; descendants inherit folder policies. */
    void setPathPolicy(const QString &relativePath, LocalAvailability availability);
    /** Restores local signatures and starts local change monitoring without a full scan. */
    void initializeLocalMonitoring(const QStringList &signatures,
                                   const QStringList &remotePaths,
                                   const QString &localDirectory);
    /** Restores placeholder paths persisted for a RemoteOnly profile. */
    void setPlaceholderPaths(const QStringList &paths);
    /** Returns placeholder paths suitable for profile persistence. */
    [[nodiscard]] QStringList placeholderPaths() const;
    /** Returns path/hash pairs suitable for profile persistence. */
    [[nodiscard]] QStringList localSignatures() const;
    /** Returns remote ID/path pairs suitable for profile persistence. */
    [[nodiscard]] QStringList remotePaths() const;
    /** Returns whether a transfer or serialized remote mutation is active. */
    [[nodiscard]] bool hasActiveTransfers() const;
    /** Returns the current remote tree for a filesystem provider. */
    [[nodiscard]] QVariantList remoteEntries() const;
    /** Queues one remote file for a foreground filesystem read. */
    void materializeFile(const QString &relativePath);
    /** Materializes one file or all files below a selected folder. */
    void materializePath(const QString &relativePath);
    /** Evicts cached content without issuing any remote deletion. */
    void evictPath(const QString &relativePath);

Q_SIGNALS:
    /** Emitted when Graph returns a valid quota snapshot. */
    void quotaReceived(const StorageQuota &quota);
    /** Emitted when /me/drive discovers the default drive identifier. */
    void driveDiscovered(const QString &driveId, const StorageQuota &quota);
    /** Emitted with the current first-level remote folders. */
    void rootFoldersReceived(const QList<GraphRemoteFolder> &folders);
    /** Emitted for transport, authentication, or malformed-response failures. */
    void errorOccurred(const QString &message);
    /** Emitted for throttled requests with the provider's requested delay. */
    void retryableError(int retryAfterSeconds, const QString &message);
    /** Reports remote-to-local synchronization progress and the current path. */
    void syncProgress(int percent, const QString &relativePath);
    /** Emitted after all queued remote files have been downloaded. */
    void syncFinished();
    /** Emitted when Graph returns the opaque cursor for the next delta round. */
    void deltaLinkChanged(const QString &deltaLink);
    /** Emitted for diagnostic messages shown in the tray activity history. */
    void logMessage(const QString &message);
    /** Emitted when the local baseline changes after a successful operation. */
    void localStateChanged(const QStringList &signatures, const QStringList &remotePaths);
    /** Emitted when RemoteOnly placeholder paths change. */
    void placeholderStateChanged(const QStringList &paths);
    /** Emitted when a Dolphin/CLI path policy changes. */
    void pathPoliciesChanged(const QStringList &policies);

private:
    /** Per-download state retained across Graph redirect and content replies. */
    struct DownloadTransfer;
    /** Per-upload state retained across session creation and chunk replies. */
    struct UploadTransfer;

    /** Sends one diagnostic to stdout, journald, and the tray activity model. */
    void log(const QString &message);
    /** Sends a diagnostic to stdout and journald without creating a tray row. */
    void logProgress(const QString &message);
    /** Reports an HTTP failure and extracts Graph throttling metadata. */
    [[nodiscard]] QString networkError(QNetworkReply *reply, const QString &fallback);
    /** Continues the breadth-first remote folder enumeration. */
    void processNextFolder();
    /** Continues the queued remote file downloads. */
    void processNextFile();
    /** Queues a confirmed file after availability and path validation. */
    void queueMaterialization(const QString &relativePath, const QString &itemId);
    /** Starts as many queued downloads as the concurrency budget allows. */
    void startPendingDownloads();
    /** Starts one independent streaming download transfer. */
    void startDownload(const GraphSyncFile &file);
    /** Validates a content response and atomically writes the local file. */
    void processDownloadedReply(QNetworkReply *reply,
                                const std::shared_ptr<DownloadTransfer> &transfer);
    /** Drains a network reply into the active atomic download file. */
    void writeDownloadChunk(QNetworkReply *reply,
                            const std::shared_ptr<DownloadTransfer> &transfer);
    /** Reads the final content length as soon as download headers arrive. */
    void updateDownloadMetadata(QNetworkReply *reply,
                                const std::shared_ptr<DownloadTransfer> &transfer);
    /** Compares the filesystem with the persisted hash baseline. */
    void scanLocalChanges();
    /** Uploads the next local create or content update. */
    void uploadNextLocalFile();
    /** Starts queued uploads while the upload concurrency budget has capacity. */
    void startPendingUploads();
    /** Starts one small direct upload or large upload-session transfer. */
    void startUpload(const GraphLocalFile &file);
    /** Creates a resumable Graph upload session for a large local file. */
    void createUploadSession(const std::shared_ptr<UploadTransfer> &transfer);
    /** Sends the next aligned byte range of the active upload session. */
    void uploadNextChunk(const std::shared_ptr<UploadTransfer> &transfer);
    /** Completes bookkeeping for a transfer and starts the next queued work. */
    void finishUpload(const std::shared_ptr<UploadTransfer> &transfer,
                      const QJsonObject &uploaded);
    /** Deletes the next remote item whose local counterpart was removed. */
    void deleteNextRemoteFile();
    /** Patches the name of the next remote item while preserving its ID. */
    void renameNextRemoteFile();
    /** Runs rename, delete, and upload queues in their conflict-safe order. */
    void processPendingLocalOperations();
    /** Returns whether a path is covered by the include/exclude profile policy. */
    [[nodiscard]] bool isIncluded(const QString &relativePath) const;
    /** Evicts cached files when a profile-wide RemoteOnly policy is applied. */
    void evictMaterializedFiles();
    /** Creates a zero-byte visible marker for a remote-only file. */
    void createPlaceholder(const QString &relativePath);
    /** Returns whether a remote folder should be traversed during enumeration. */
    [[nodiscard]] bool shouldTraverse(const QString &relativePath) const;
    /** Identifies folder paths before they can enter the file transfer queue. */
    [[nodiscard]] bool isRemoteFolderPath(const QString &relativePath) const;
    /** Converts a relative path into a confined local path or an empty path. */
    [[nodiscard]] QString safeLocalPath(const QString &relativePath) const;
    /** Resolves the nearest inherited availability policy for a path. */
    [[nodiscard]] LocalAvailability availabilityForPath(const QString &relativePath) const;
    /** Returns whether a remote file should be downloaded without an explicit read. */
    [[nodiscard]] bool shouldMaterializePath(const QString &relativePath) const;
    /** Returns whether a path should be represented by a placeholder. */
    [[nodiscard]] bool shouldKeepRemotePath(const QString &relativePath) const;

    QNetworkAccessManager m_network;
    /** Credentials and filters used by the initial synchronization operation. */
    QString m_syncDriveId;
    QString m_syncToken;
    QString m_syncDirectory;
    QStringList m_includedFolders;
    QStringList m_excludedFolders;
    /** Breadth-first queues used by the initial remote tree walk. */
    QQueue<GraphSyncFolder> m_pendingFolders;
    QQueue<GraphSyncFile> m_pendingFiles;
    /** Serialized local mutations and their path-level duplicate guards. */
    QQueue<GraphLocalFile> m_pendingUploads;
    QQueue<GraphRemoteDelete> m_pendingRemoteDeletes;
    QQueue<GraphRemoteRename> m_pendingRemoteRenames;
    QSet<QString> m_pendingRemoteDeletePaths;
    /** Current remote identity index: path -> item/folder ID. */
    QHash<QString, QString> m_remoteItemIds;
    QHash<QString, QString> m_remoteFolderIds;
    /** Reverse identity index used to recognize remote renames in delta pages. */
    QHash<QString, QString> m_remotePathsById;
    /** Last accepted remote eTag per item; unchanged delta entries are ignored. */
    QHash<QString, QString> m_remoteEtags;
    /** Remote sizes and folder markers used by FUSE stat/readdir responses. */
    QHash<QString, qint64> m_remoteSizes;
    QSet<QString> m_remoteFolders;
    /** SHA-256 signatures of local files at the last persisted baseline. */
    QHash<QString, QString> m_localSignatures;
    /** Paths occupied by zero-byte files that must never be uploaded. */
    QSet<QString> m_placeholderPaths;
    /** Exact path policy overrides; folder rules apply to all descendants. */
    QHash<QString, LocalAvailability> m_pathPolicies;
    /**
     * Metadata used to skip hashing files whose size and modification time
     * are unchanged since the previous local scan.
     */
    QHash<QString, QPair<qint64, QDateTime>> m_localMetadata;
    /** Local polling and remote delta polling timers. */
    QTimer m_uploadTimer;
    QTimer m_remoteTimer;
    /** False while a profile is paused; persisted state remains untouched. */
    bool m_monitoringEnabled = true;
    /** Opaque Graph delta cursor and credentials for subsequent polling. */
    QString m_deltaDriveId;
    QString m_deltaToken;
    QString m_deltaLink;
    /** Cursor received for the current page, committed after its work succeeds. */
    QString m_pendingDeltaLink;
    /** Prevents advancing a delta cursor when any item in that page failed. */
    bool m_deltaPageFailed = false;
    /** Indicates that one or more uploads are active. */
    bool m_uploadInProgress = false;
    /** True when the profile-wide fallback automatically materializes content. */
    bool m_materializeFiles = true;
    /** Whether the profile-wide fallback owns placeholders and cache eviction. */
    bool m_remoteOnlyMode = false;
    /** Profile-wide fallback used when no path override matches. */
    LocalAvailability m_defaultAvailability = LocalAvailability::KeepLocal;
    /** True while the current local-change batch still has queued work. */
    bool m_uploadBatchActive = false;
    bool m_deleteInProgress = false;
    /** Active local-to-remote delete, aborted when entering RemoteOnly. */
    QPointer<QNetworkReply> m_activeDeleteReply;
    bool m_renameInProgress = false;
    QSet<QString> m_pendingRemoteRenamePaths;
    QSet<QString> m_pendingUploadPaths;
    /**
     * Local mutations echoed by Graph are consumed without downloading again.
     * IDs are preferred; paths cover a newly uploaded item before its ID is known.
     */
    QSet<QString> m_suppressedRemoteItemIds;
    QSet<QString> m_suppressedRemotePaths;
    /** Remote-only/on-demand paths explicitly requested by the FUSE reader. */
    QSet<QString> m_requestedMaterializations;
    /** Progress counters for the currently active download batch. */
    int m_downloadedFiles = 0;
    int m_totalFiles = 0;
    /** Independent transfers currently occupying download/upload slots. */
    QHash<QString, std::shared_ptr<DownloadTransfer>> m_activeDownloads;
    QHash<QString, std::shared_ptr<UploadTransfer>> m_activeUploads;
    /** Initial conservative budgets; profile settings can expose these later. */
    int m_maxConcurrentDownloads = 2;
    int m_maxConcurrentUploads = 2;
    int m_maxConcurrentLargeTransfers = 1;
};
