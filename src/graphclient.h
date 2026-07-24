// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "storagequota.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QHash>
#include <QTimer>
#include <QSet>

class QNetworkReply;

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
    /** Downloads the remote tree into localDirectory for the first sync slice. */
    void synchronize(const QString &driveId, const QString &accessToken,
                     const QString &localDirectory, const QStringList &includedFolders,
                     const QStringList &excludedFolders);
    /** Starts delta-based remote change checks using the configured interval. */
    void startRemoteMonitoring(const QString &driveId, const QString &accessToken,
                               int intervalSeconds, const QString &deltaLink = {});
    /** Restores local signatures and starts local change monitoring without a full scan. */
    void initializeLocalMonitoring(const QStringList &signatures,
                                   const QStringList &remotePaths,
                                   const QString &localDirectory);
    /** Returns path/hash pairs suitable for profile persistence. */
    [[nodiscard]] QStringList localSignatures() const;
    /** Returns remote ID/path pairs suitable for profile persistence. */
    [[nodiscard]] QStringList remotePaths() const;

Q_SIGNALS:
    /** Emitted when Graph returns a valid quota snapshot. */
    void quotaReceived(const StorageQuota &quota);
    /** Emitted when /me/drive discovers the default drive identifier. */
    void driveDiscovered(const QString &driveId, const StorageQuota &quota);
    /** Emitted with the current first-level remote folders. */
    void rootFoldersReceived(const QList<GraphRemoteFolder> &folders);
    /** Emitted for transport, authentication, or malformed-response failures. */
    void errorOccurred(const QString &message);
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

private:
    /** Sends one diagnostic to stdout, journald, and the tray activity model. */
    void log(const QString &message);
    /** Continues the breadth-first remote folder enumeration. */
    void processNextFolder();
    /** Continues the queued remote file downloads. */
    void processNextFile();
    /** Validates a content response and atomically writes the local file. */
    void processDownloadedReply(QNetworkReply *reply, const GraphSyncFile &file,
                                const QString &localPath);
    /** Compares the filesystem with the persisted hash baseline. */
    void scanLocalChanges();
    /** Uploads the next local create or content update. */
    void uploadNextLocalFile();
    /** Deletes the next remote item whose local counterpart was removed. */
    void deleteNextRemoteFile();
    /** Patches the name of the next remote item while preserving its ID. */
    void renameNextRemoteFile();
    /** Runs rename, delete, and upload queues in their conflict-safe order. */
    void processPendingLocalOperations();
    /** Returns whether a path is covered by the include/exclude profile policy. */
    [[nodiscard]] bool isIncluded(const QString &relativePath) const;
    /** Returns whether a remote folder should be traversed during enumeration. */
    [[nodiscard]] bool shouldTraverse(const QString &relativePath) const;
    /** Converts a relative path into a confined local path or an empty path. */
    [[nodiscard]] QString safeLocalPath(const QString &relativePath) const;

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
    /** SHA-256 signatures of local files at the last persisted baseline. */
    QHash<QString, QString> m_localSignatures;
    /** Local polling and remote delta polling timers. */
    QTimer m_uploadTimer;
    QTimer m_remoteTimer;
    /** Opaque Graph delta cursor and credentials for subsequent polling. */
    QString m_deltaDriveId;
    QString m_deltaToken;
    QString m_deltaLink;
    /** Prevents overlapping mutations and recursive scans while a request runs. */
    bool m_uploadInProgress = false;
    bool m_deleteInProgress = false;
    bool m_renameInProgress = false;
    QSet<QString> m_pendingRemoteRenamePaths;
    QSet<QString> m_pendingUploadPaths;
    /**
     * Local mutations echoed by Graph are consumed without downloading again.
     * IDs are preferred; paths cover a newly uploaded item before its ID is known.
     */
    QSet<QString> m_suppressedRemoteItemIds;
    QSet<QString> m_suppressedRemotePaths;
    /** Progress counters for the currently active download batch. */
    int m_downloadedFiles = 0;
    int m_totalFiles = 0;
};
