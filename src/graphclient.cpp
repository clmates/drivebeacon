// SPDX-License-Identifier: GPL-3.0-only

#include "graphclient.h"
#include "graphretrypolicy.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QDebug>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QDateTime>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QScopeGuard>
#include <QUrl>
#include <QUrlQuery>
#include <QSet>
#include <systemd/sd-journal.h>

#include <algorithm>

namespace {
/** Files above this size use resumable sessions; smaller files use one PUT. */
constexpr qint64 largeUploadThreshold = 10 * 1024 * 1024;
/** 10 MiB is aligned to Graph's required 320 KiB upload range multiple. */
constexpr qint64 uploadChunkSize = 10 * 1024 * 1024;

/** Serializes path policies locally so GraphClient tests remain self-contained. */
QString graphAvailabilityName(LocalAvailability availability)
{
    switch (availability) {
    case LocalAvailability::KeepLocal:
        return QStringLiteral("keep-local");
    case LocalAvailability::RemoteOnly:
        return QStringLiteral("remote-only");
    case LocalAvailability::OnDemand:
        return QStringLiteral("on-demand");
    }
    return QStringLiteral("on-demand");
}

/** Adds a UTC timestamp and publishes the same message to stderr/journald. */
void graphLog(const QString &message)
{
    const QString timestamped = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyyMMdd_HHmmss'Z'")) + QLatin1Char(' ') + message;
    qInfo().noquote() << timestamped;
    sd_journal_send("MESSAGE=%s", qPrintable(timestamped),
                    "PRIORITY=%i", 6,
                    "SYSLOG_IDENTIFIER=%s", "drivebeacon", nullptr);
}

QNetworkRequest graphRequest(const QUrl &url, const QByteArray &accessToken)
{
    QNetworkRequest request(url);
    // Some Microsoft Graph tenants advertise HTTP/2 but reject authenticated
    // streams intermittently. HTTP/1.1 keeps the bearer request reliable and
    // still permits the normal Graph redirects for file content.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + accessToken);
    request.setRawHeader("Accept", QByteArrayLiteral("application/json"));
    return request;
}

QString graphError(QNetworkReply *reply, const QString &fallback)
{
    const QByteArray body = reply->readAll();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (document.isObject()) {
        const QJsonObject error = document.object().value(QStringLiteral("error")).toObject();
        const QString message = error.value(QStringLiteral("message")).toString();
        if (!message.isEmpty()) {
            return QStringLiteral("Graph (%1): %2").arg(reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt()).arg(message);
        }
    }
    return reply->errorString().isEmpty() ? fallback : reply->errorString();
}

QString normalizeFolderFilter(const QString &value)
{
    QString normalized = value.trimmed();
    while (normalized.startsWith(QLatin1Char('/'))) {
        normalized.remove(0, 1);
    }
    while (normalized.endsWith(QLatin1Char('/'))) {
        normalized.chop(1);
    }
    return normalized;
}

QString localFileSignature(const QString &path)
{
    // Hashes make change detection independent of filesystem timestamp
    // resolution and also let a same-directory rename retain the remote ID.
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&input)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}
}

struct GraphClient::DownloadTransfer {
    /** Per-file state kept alive by the network reply callbacks. */
    GraphSyncFile file;
    /** Final local destination; the temporary QSaveFile lives beside it. */
    QString localPath;
    /** Atomic output prevents an interrupted download becoming a baseline file. */
    std::unique_ptr<QSaveFile> output;
    /** Set when a disk write fails; the delta cursor must then remain unchanged. */
    bool writeFailed = false;
    /** Bytes committed to the temporary file for progress and diagnostics. */
    qint64 bytes = 0;
    /** Content-Length from Graph or the signed download URL, when available. */
    qint64 totalBytes = -1;
    /** Last percentage sent to the UI; updates are throttled to five points. */
    int lastProgress = -1;
    /** Fallback byte threshold used when the server omits Content-Length. */
    qint64 lastLogBytes = 0;
};

struct GraphClient::UploadTransfer {
    /** Per-file state shared by a simple PUT or every upload-session chunk. */
    GraphLocalFile file;
    /** Pre-authenticated Graph upload-session endpoint for large files. */
    QUrl uploadUrl;
    /** Snapshot size used to build Content-Range and progress information. */
    qint64 size = 0;
    /** Offset of the next byte requested by Graph. */
    qint64 offset = 0;
    /** Last percentage reported for this upload. */
    int lastProgress = -1;
};

GraphClient::GraphClient(QObject *parent)
    : QObject(parent)
    , m_network(this)
    , m_uploadTimer(this)
    , m_remoteTimer(this)
{
    m_uploadTimer.setInterval(10000);
    connect(&m_uploadTimer, &QTimer::timeout, this, &GraphClient::scanLocalChanges);
    connect(&m_remoteTimer, &QTimer::timeout, this, [this] {
        // Do not advance the cursor while a transfer is still being applied.
        // Otherwise a restart could resume after an unfinished large download.
        if (!m_activeDownloads.isEmpty() || !m_activeUploads.isEmpty()
            || m_deleteInProgress
            || m_renameInProgress) {
            return;
        }
        // Delta is a persisted pull cursor, not a complete listing. A page can
        // contain both a tombstone and the live item for a rename, and the same
        // item may be repeated; the sets below make each identity idempotent.
        log(QStringLiteral("Graph sync: checking remote delta"));
        QUrl url = m_deltaLink.isEmpty()
            ? QUrl(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1/root/delta?token=latest")
                       .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_deltaDriveId))))
            : QUrl(m_deltaLink);
        if (m_deltaLink.isEmpty()) {
            // The first delta page must carry parentReference and size so
            // nested files can be routed and scheduled correctly after a
            // restart; subsequent opaque links must remain untouched.
            QUrlQuery query(url);
            query.addQueryItem(QStringLiteral("$select"),
                               QStringLiteral("id,name,folder,file,deleted,parentReference,eTag,size"));
            url.setQuery(query);
        } else {
            // Older persisted cursors selected only `file`. Preserve the
            // opaque delta token but request the folder facet as well, so an
            // empty directory cannot be mistaken for a zero-byte file.
            QUrlQuery query(url);
            query.removeAllQueryItems(QStringLiteral("$select"));
            query.addQueryItem(QStringLiteral("$select"),
                               QStringLiteral("id,name,folder,file,deleted,parentReference,eTag,size"));
            url.setQuery(query);
        }
        auto *reply = m_network.get(graphRequest(url, m_deltaToken.toUtf8()));
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
            if (reply->error() != QNetworkReply::NoError) {
                Q_EMIT errorOccurred(networkError(reply, QStringLiteral("Could not query remote changes.")));
                return;
            }
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                Q_EMIT errorOccurred(QStringLiteral("Graph returned invalid delta JSON."));
                return;
            }
            const QJsonObject object = document.object();
            const QString nextLink = object.value(QStringLiteral("@odata.nextLink")).toString();
            const QString deltaLink = object.value(QStringLiteral("@odata.deltaLink")).toString();
            if (!deltaLink.isEmpty()) {
                m_pendingDeltaLink = deltaLink;
            } else if (!nextLink.isEmpty()) {
                m_pendingDeltaLink = nextLink;
            }
            m_deltaPageFailed = false;
            const QJsonArray deltaValues = object.value(QStringLiteral("value")).toArray();
            QSet<QString> activeDeltaIds;
            for (const QJsonValue &value : deltaValues) {
                const QJsonObject item = value.toObject();
                if (!item.contains(QStringLiteral("deleted"))
                    && item.contains(QStringLiteral("file"))) {
                    activeDeltaIds.insert(item.value(QStringLiteral("id")).toString());
                }
            }
            bool structureChanged = false;
            // Keep only the first actionable representation of an item in this
            // page. This avoids downloading a file twice when Graph coalesces
            // several updates into one delta response.
            QSet<QString> handledDeltaIds;
            for (const QJsonValue &value : deltaValues) {
                const QJsonObject item = value.toObject();
                const QString itemId = item.value(QStringLiteral("id")).toString();
                if (handledDeltaIds.contains(itemId)) {
                    continue;
                }
                const QString oldPath = m_remotePathsById.value(itemId);
                if (item.contains(QStringLiteral("deleted"))) {
                    if (activeDeltaIds.contains(itemId)) {
                        // A rename can be represented by a tombstone plus the
                        // current item in the same delta page. Apply the live
                        // item only; the identity is still the same driveItem.
                        continue;
                    }
                    log(QStringLiteral("Graph sync: remote deletion reported for %1")
                            .arg(oldPath.isEmpty() ? itemId : oldPath));
                    if (!oldPath.isEmpty() && isIncluded(oldPath)) {
                        log(QStringLiteral("Graph sync: removing remotely deleted %1").arg(oldPath));
                        const QString localPath = safeLocalPath(oldPath);
                        if (QFileInfo(localPath).isDir()) {
                            QDir().rmdir(localPath);
                        } else {
                            QFile::remove(localPath);
                        }
                        m_placeholderPaths.remove(oldPath);
                        m_localSignatures.remove(oldPath);
                        m_localMetadata.remove(oldPath);
                        m_pendingRemoteDeletePaths.remove(oldPath);
                        QQueue<GraphLocalFile> remainingUploads;
                        while (!m_pendingUploads.isEmpty()) {
                            const GraphLocalFile pending = m_pendingUploads.dequeue();
                            if (pending.relativePath != oldPath) {
                                remainingUploads.enqueue(pending);
                            }
                        }
                        m_pendingUploads = std::move(remainingUploads);
                        Q_EMIT placeholderStateChanged(placeholderPaths());
                    }
                    m_remotePathsById.remove(itemId);
                    m_remoteItemIds.remove(oldPath);
                    handledDeltaIds.insert(itemId);
                    continue;
                }
                if (!item.contains(QStringLiteral("file"))) {
                    // Delta commonly includes a parent folder when a child
                    // changes; that is metadata context, not a folder change.
                    if (item.contains(QStringLiteral("folder")) && !oldPath.isEmpty()) {
                        // The persisted baseline does not store item facets.
                        // Recover the known folder identity before continuing
                        // so this path cannot enter the file queue.
                        m_remoteFolders.insert(oldPath);
                        m_remoteFolderIds.insert(oldPath, itemId);
                        m_remoteEtags.insert(itemId, item.value(QStringLiteral("eTag")).toString());
                    }
                    continue;
                }
                const QString parentId = item.value(QStringLiteral("parentReference")).toObject()
                                             .value(QStringLiteral("id")).toString();
                QString parentPath = m_remotePathsById.value(parentId);
                if (parentPath.isEmpty() && !parentId.isEmpty()) {
                    // The folder index is path keyed, so use it as a second
                    // in-memory identity source before parsing Graph's path.
                    for (auto folderIt = m_remoteFolderIds.cbegin();
                         folderIt != m_remoteFolderIds.cend(); ++folderIt) {
                        if (folderIt.value() == parentId) {
                            parentPath = folderIt.key();
                            break;
                        }
                    }
                }
                if (parentPath.isEmpty()) {
                    // Delta responses can omit a folder identity that was not
                    // present in the persisted baseline. Graph still provides
                    // parentReference.path, which preserves the real folder.
                    const QString graphParentPath = item.value(QStringLiteral("parentReference"))
                                                       .toObject()
                                                       .value(QStringLiteral("path"))
                                                       .toString();
                    const int rootMarker = graphParentPath.indexOf(QStringLiteral("root:"));
                    if (rootMarker >= 0) {
                        parentPath = graphParentPath.sliced(rootMarker + 5);
                        while (parentPath.startsWith(QLatin1Char('/'))) {
                            parentPath.remove(0, 1);
                        }
                        while (parentPath.endsWith(QLatin1Char('/'))) {
                            parentPath.chop(1);
                        }
                    } else if (!graphParentPath.isEmpty()) {
                        // Some Graph responses omit the `root:` marker but
                        // still return a usable slash-separated parent path.
                        parentPath = graphParentPath;
                        while (parentPath.startsWith(QLatin1Char('/'))) {
                            parentPath.remove(0, 1);
                        }
                        while (parentPath.endsWith(QLatin1Char('/'))) {
                            parentPath.chop(1);
                        }
                    }
                }
                const QString name = item.value(QStringLiteral("name")).toString();
                if (name.isEmpty() || (parentId.isEmpty() && oldPath.isEmpty())) {
                    structureChanged = true;
                    continue;
                }
                const QString relativePath = oldPath.isEmpty()
                    ? (parentPath.isEmpty() ? name : parentPath + QLatin1Char('/') + name)
                    : oldPath;
                if (oldPath.isEmpty() && parentPath.isEmpty() && !parentId.isEmpty()) {
                    log(QStringLiteral("Graph sync: delta item %1 has no resolvable parent path")
                            .arg(itemId));
                }
                const QString effectiveParentPath = parentPath.isEmpty() && !oldPath.isEmpty()
                    ? QFileInfo(oldPath).path() : parentPath;
                const QString renamedPath = effectiveParentPath.isEmpty()
                    ? name : effectiveParentPath + QLatin1Char('/') + name;
                const QString eTag = item.value(QStringLiteral("eTag")).toString();
                if (!oldPath.isEmpty() && oldPath == renamedPath && !eTag.isEmpty()
                    && m_remoteEtags.value(itemId) == eTag) {
                    handledDeltaIds.insert(itemId);
                    continue;
                }
                // Consume a locally-originated echo only when both identity
                // and expected path match. Matching by ID alone could hide a
                // later genuine remote rename of that same driveItem.
                const bool suppressedItem = m_suppressedRemoteItemIds.contains(itemId)
                    && m_suppressedRemotePaths.contains(renamedPath);
                if (suppressedItem) {
                    m_suppressedRemoteItemIds.remove(itemId);
                    m_suppressedRemotePaths.remove(renamedPath);
                    m_remotePathsById.insert(itemId, renamedPath);
                    m_remoteEtags.insert(itemId, eTag);
                    m_remoteItemIds.remove(oldPath);
                    m_remoteItemIds.insert(renamedPath, itemId);
                    handledDeltaIds.insert(itemId);
                    continue;
                }
                if (!oldPath.isEmpty() && oldPath != renamedPath) {
                    const QString oldLocalPath = safeLocalPath(oldPath);
                    const QString newLocalPath = safeLocalPath(renamedPath);
                    if (QFileInfo(oldLocalPath).exists() && !m_placeholderPaths.contains(oldPath)) {
                        QDir().mkpath(QFileInfo(newLocalPath).absolutePath());
                        if (!QFile::rename(oldLocalPath, newLocalPath)) {
                            Q_EMIT errorOccurred(QStringLiteral("Could not apply remote rename: %1 → %2")
                                                     .arg(oldPath, renamedPath));
                            continue;
                        }
                        log(QStringLiteral("Graph sync: renamed local %1 → %2")
                                .arg(oldPath, renamedPath));
                        const QString signature = m_localSignatures.take(oldPath);
                        if (!signature.isEmpty()) {
                            m_localSignatures.insert(renamedPath, signature);
                        }
                        if (m_localMetadata.contains(oldPath)) {
                            m_localMetadata.insert(renamedPath, m_localMetadata.take(oldPath));
                        }
                    } else if (m_placeholderPaths.contains(oldPath)) {
                        QDir().mkpath(QFileInfo(newLocalPath).absolutePath());
                        if (QFile::rename(oldLocalPath, newLocalPath)) {
                            m_placeholderPaths.remove(oldPath);
                            m_placeholderPaths.insert(renamedPath);
                            log(QStringLiteral("Graph sync: renamed local placeholder %1 → %2")
                                    .arg(oldPath, renamedPath));
                            Q_EMIT placeholderStateChanged(placeholderPaths());
                        }
                    }
                    m_remoteItemIds.remove(oldPath);
                    m_remoteItemIds.insert(renamedPath, itemId);
                    m_remotePathsById.insert(itemId, renamedPath);
                    handledDeltaIds.insert(itemId);
                    continue;
                }
                m_remotePathsById.insert(itemId, relativePath);
                m_remoteItemIds.insert(relativePath, itemId);
                m_remoteEtags.insert(itemId, eTag);
                if ((shouldMaterializePath(relativePath)
                     || m_requestedMaterializations.contains(relativePath))
                    && isIncluded(relativePath)
                    && !isRemoteFolderPath(relativePath)) {
                    m_pendingFiles.enqueue({itemId, relativePath,
                                            item.value(QStringLiteral("size")).toVariant().toLongLong()});
                }
                handledDeltaIds.insert(itemId);
            }
            if (structureChanged && !m_syncDirectory.isEmpty()) {
                log(QStringLiteral("Graph sync: remote folder structure changed; refreshing selected tree"));
                synchronize(m_deltaDriveId, m_deltaToken, m_syncDirectory,
                            m_includedFolders, m_excludedFolders);
            } else if (!m_pendingFiles.isEmpty()) {
                log(QStringLiteral("Graph sync: downloading %1 changed remote file(s)")
                             .arg(m_pendingFiles.size()));
                m_totalFiles = m_pendingFiles.size();
                m_downloadedFiles = 0;
                processNextFile();
            } else if (!m_pendingDeltaLink.isEmpty()) {
                // An empty page is still a successful page: advance its
                // cursor so the same delta is not replayed on every poll.
                m_deltaLink = m_pendingDeltaLink;
                Q_EMIT deltaLinkChanged(m_pendingDeltaLink);
                m_pendingDeltaLink.clear();
            }
        });
    });
}

void GraphClient::log(const QString &message)
{
    graphLog(message);
    Q_EMIT logMessage(message);
}

void GraphClient::logProgress(const QString &message)
{
    // Transfer progress is represented by one mutable ActivityModel row; it
    // remains fully visible in journald without creating a row per percentage.
    // Forward it as well so a headless service can reproduce that row in the
    // tray process after synchronization was decoupled from the UI.
    graphLog(message);
    Q_EMIT logMessage(message);
}

QString GraphClient::networkError(QNetworkReply *reply, const QString &fallback)
{
    const QString message = graphError(reply, fallback);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 429) {
        Q_EMIT retryableError(graphRetryAfterSeconds(reply->rawHeader("Retry-After")), message);
    }
    return message;
}

void GraphClient::startRemoteMonitoring(const QString &driveId, const QString &accessToken,
                                        int intervalSeconds, const QString &deltaLink)
{
    // This path is also used after restoring persisted state, without calling
    // synchronize(); keep all Graph operations supplied with the active drive
    // and bearer token in that case too. This also resumes any work that was
    // queued when a profile was paused.
    m_syncDriveId = driveId;
    m_syncToken = accessToken;
    m_deltaDriveId = driveId;
    m_deltaToken = accessToken;
    m_deltaLink = deltaLink;
    m_monitoringEnabled = true;
    m_remoteTimer.setInterval(qBound(10, intervalSeconds, 3600) * 1000);
    m_remoteTimer.start();
    m_uploadTimer.start();
    // A pause can occur while a page still has queued work. Re-enable the
    // schedulers so that resuming continues that page before polling again.
    startPendingDownloads();
    startPendingUploads();
}

void GraphClient::stopMonitoring()
{
    // Stopping only the polling sources is intentional: persisted cursors and
    // in-memory identity maps must survive a temporary account pause.
    m_monitoringEnabled = false;
    m_uploadTimer.stop();
    m_remoteTimer.stop();
}

void GraphClient::configureTransferConcurrency(int downloads, int uploads, int largeTransfers)
{
    // Keep limits conservative even if an old or manually edited profile has
    // invalid values; each direction has independent general and large slots.
    m_maxConcurrentDownloads = qBound(1, downloads, 8);
    m_maxConcurrentUploads = qBound(1, uploads, 8);
    m_maxConcurrentLargeTransfers = qBound(1, largeTransfers, 4);
    logProgress(QStringLiteral("Graph sync: transfer limits downloads=%1 uploads=%2 large=%3")
                    .arg(m_maxConcurrentDownloads)
                    .arg(m_maxConcurrentUploads)
                    .arg(m_maxConcurrentLargeTransfers));
}

void GraphClient::setPathPolicies(const QStringList &policies)
{
    m_pathPolicies.clear();
    for (const QString &record : policies) {
        const int separator = record.indexOf(QLatin1Char('\t'));
        if (separator <= 0) {
            continue;
        }
        const QString path = QDir::cleanPath(record.left(separator)).trimmed();
        if (path.isEmpty() || path == QLatin1String(".")) {
            continue;
        }
        const QString policy = record.sliced(separator + 1).trimmed().toLower();
        const LocalAvailability availability = policy == QLatin1String("keep-local")
            ? LocalAvailability::KeepLocal
            : policy == QLatin1String("remote-only")
                ? LocalAvailability::RemoteOnly
                : LocalAvailability::OnDemand;
        m_pathPolicies.insert(path, availability);
    }
}

QStringList GraphClient::pathPolicies() const
{
    QStringList result;
    for (auto it = m_pathPolicies.cbegin(); it != m_pathPolicies.cend(); ++it) {
        result.append(it.key() + QLatin1Char('\t') + graphAvailabilityName(it.value()));
    }
    std::sort(result.begin(), result.end());
    return result;
}

void GraphClient::setPathPolicy(const QString &relativePath, LocalAvailability availability)
{
    const QString normalized = QDir::cleanPath(relativePath).trimmed();
    if (normalized.isEmpty() || normalized == QLatin1String(".")) {
        Q_EMIT errorOccurred(QStringLiteral("A relative file or folder path is required."));
        return;
    }
    const QString prefix = normalized + QLatin1Char('/');
    // A folder policy becomes the new inheritance boundary. Remove all
    // descendant overrides so a later KeepLocal, OnDemand, or RemoteOnly
    // decision cannot be shadowed by an older child policy.
    for (auto it = m_pathPolicies.begin(); it != m_pathPolicies.end();) {
        if (it.key() == normalized || it.key().startsWith(prefix)) {
            it = m_pathPolicies.erase(it);
        } else {
            ++it;
        }
    }
    m_pathPolicies.insert(normalized, availability);
    Q_EMIT pathPoliciesChanged(pathPolicies());
}

LocalAvailability GraphClient::availabilityForPath(const QString &relativePath) const
{
    QString candidate = QDir::cleanPath(relativePath).trimmed();
    while (!candidate.isEmpty() && candidate != QLatin1String(".")) {
        if (m_pathPolicies.contains(candidate)) {
            return m_pathPolicies.value(candidate);
        }
        const int separator = candidate.lastIndexOf(QLatin1Char('/'));
        if (separator < 0) {
            break;
        }
        candidate.truncate(separator);
    }
    return m_defaultAvailability;
}

bool GraphClient::shouldMaterializePath(const QString &relativePath) const
{
    return availabilityForPath(relativePath) == LocalAvailability::KeepLocal;
}

bool GraphClient::shouldKeepRemotePath(const QString &relativePath) const
{
    return availabilityForPath(relativePath) == LocalAvailability::RemoteOnly;
}

void GraphClient::setPlaceholderPaths(const QStringList &paths)
{
    m_placeholderPaths = QSet<QString>(paths.cbegin(), paths.cend());
}

QStringList GraphClient::placeholderPaths() const
{
    QStringList paths = m_placeholderPaths.values();
    std::sort(paths.begin(), paths.end());
    return paths;
}

void GraphClient::initializeLocalMonitoring(const QStringList &signatures,
                                            const QStringList &remotePaths,
                                            const QString &localDirectory)
{
    m_syncDirectory = QDir::cleanPath(QFileInfo(localDirectory).absoluteFilePath());
    m_localSignatures.clear();
    m_localMetadata.clear();
    for (const QString &entry : signatures) {
        const int separator = entry.indexOf(QLatin1Char('\t'));
        if (separator > 0) {
            // Versions of the baseline writer briefly appended an empty eTag
            // field here. Accept that historical form, but keep local hashes
            // strictly two-field records from now on.
            const QString storedHash = entry.sliced(separator + 1);
            const int legacyEtags = storedHash.indexOf(QLatin1Char('\t'));
            const QString relativePath = entry.left(separator);
            m_localSignatures.insert(relativePath, legacyEtags < 0
                                         ? storedHash
                                         : storedHash.left(legacyEtags));
            const QFileInfo fileInfo(safeLocalPath(relativePath));
            if (fileInfo.isFile()) {
                m_localMetadata.insert(relativePath,
                                       {fileInfo.size(), fileInfo.lastModified()});
            }
        }
    }
    m_remoteItemIds.clear();
    m_remotePathsById.clear();
    m_remoteEtags.clear();
    m_remoteSizes.clear();
    m_remoteFolders.clear();
    for (const QString &entry : remotePaths) {
        const int separator = entry.indexOf(QLatin1Char('\t'));
        const int etagSeparator = entry.indexOf(QLatin1Char('\t'), separator + 1);
        const int sizeSeparator = etagSeparator < 0
            ? -1 : entry.indexOf(QLatin1Char('\t'), etagSeparator + 1);
        if (separator > 0) {
            const QString itemId = entry.left(separator);
            const QString path = etagSeparator < 0
                ? entry.sliced(separator + 1)
                : entry.sliced(separator + 1, etagSeparator - separator - 1);
            const QString etag = etagSeparator < 0 ? QString()
                : entry.sliced(etagSeparator + 1,
                               (sizeSeparator < 0 ? entry.size() : sizeSeparator)
                                   - etagSeparator - 1);
            m_remotePathsById.insert(itemId, path);
            m_remoteEtags.insert(itemId, etag);
            if (sizeSeparator >= 0) {
                bool sizeOk = false;
                const qint64 size = entry.sliced(sizeSeparator + 1).toLongLong(&sizeOk);
                if (sizeOk) {
                    m_remoteSizes.insert(path, size);
                }
            }
            if (!path.isEmpty() && path != QStringLiteral("/")) {
                m_remoteItemIds.insert(path, itemId);
            }
        }
    }
    // A previous interrupted materialization can leave a stale placeholder
    // marker even though the local baseline already contains real content.
    // Reconcile that marker before exposing the tree to FUSE.
    bool placeholdersChanged = false;
    for (auto it = m_placeholderPaths.cbegin(); it != m_placeholderPaths.cend();) {
        if (m_localSignatures.contains(*it) && !localFileSignature(*it).isEmpty()) {
            it = m_placeholderPaths.erase(it);
            placeholdersChanged = true;
        } else {
            ++it;
        }
    }
    if (placeholdersChanged) {
        Q_EMIT placeholderStateChanged(placeholderPaths());
    }
    // The persisted baseline predates the explicit folder index. Recover
    // directory entries from descendants so a restart does not expose a
    // parent such as Documentos as a regular file to FUSE or the scheduler.
    const QStringList persistedPaths = m_remotePathsById.values();
    for (const QString &path : persistedPaths) {
        if (path.isEmpty() || path == QLatin1String("/")) {
            continue;
        }
        const QString prefix = path + QLatin1Char('/');
        for (const QString &candidate : persistedPaths) {
            if (candidate.startsWith(prefix)) {
                m_remoteFolders.insert(path);
                break;
            }
        }
    }
    if (m_monitoringEnabled) {
        m_uploadTimer.start();
    }
}

void GraphClient::evictMaterializedFiles()
{
    const QStringList localPaths = m_localSignatures.keys();
    for (const QString &relativePath : localPaths) {
        if (!isIncluded(relativePath)) {
            continue;
        }
        const QString localPath = safeLocalPath(relativePath);
        if (localPath.isEmpty()) {
            continue;
        }
        if (QFileInfo::exists(localPath) && !QFile::remove(localPath)) {
            Q_EMIT errorOccurred(QStringLiteral("Could not evict local file: %1")
                                     .arg(relativePath));
            continue;
        }
        createPlaceholder(relativePath);
        log(QStringLiteral("Graph sync: evicted local %1 (Remote only)")
                .arg(relativePath));
    }
    for (const QString &relativePath : localPaths) {
        if (isIncluded(relativePath)) {
            m_localSignatures.remove(relativePath);
            m_localMetadata.remove(relativePath);
        }
    }
    Q_EMIT placeholderStateChanged(placeholderPaths());
}

void GraphClient::createPlaceholder(const QString &relativePath)
{
    const QString localPath = safeLocalPath(relativePath);
    if (localPath.isEmpty()) {
        return;
    }
    QDir().mkpath(QFileInfo(localPath).absolutePath());
    QFile placeholder(localPath);
    if (QFileInfo(localPath).isDir()) {
        return;
    }
    if (placeholder.exists() && QFileInfo(localPath).size() > 0
        && !QFile::remove(localPath)) {
        Q_EMIT errorOccurred(QStringLiteral("Could not evict local file: %1")
                                 .arg(relativePath));
        return;
    }
    if (!placeholder.exists()
        && placeholder.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        placeholder.close();
        m_placeholderPaths.insert(relativePath);
        log(QStringLiteral("Graph sync: created local placeholder %1 (Remote only)")
                .arg(relativePath));
    } else if (placeholder.exists() && QFileInfo(localPath).size() == 0) {
        m_placeholderPaths.insert(relativePath);
    }
}

QStringList GraphClient::localSignatures() const
{
    QStringList result;
    result.reserve(m_localSignatures.size());
    for (auto it = m_localSignatures.cbegin(); it != m_localSignatures.cend(); ++it) {
        result.append(it.key() + QLatin1Char('\t') + it.value());
    }
    return result;
}

QStringList GraphClient::remotePaths() const
{
    QStringList result;
    result.reserve(m_remotePathsById.size());
    for (auto it = m_remotePathsById.cbegin(); it != m_remotePathsById.cend(); ++it) {
        QString serialized = it.key() + QLatin1Char('\t') + it.value()
            + QLatin1Char('\t') + m_remoteEtags.value(it.key());
        if (m_remoteSizes.contains(it.value())) {
            serialized += QLatin1Char('\t')
                + QString::number(m_remoteSizes.value(it.value()));
        }
        result.append(serialized);
    }
    return result;
}

bool GraphClient::hasActiveTransfers() const
{
    return !m_activeDownloads.isEmpty() || !m_activeUploads.isEmpty()
        || m_deleteInProgress || m_renameInProgress;
}

QVariantList GraphClient::remoteEntries() const
{
    QVariantList entries;
    for (auto it = m_remotePathsById.cbegin(); it != m_remotePathsById.cend(); ++it) {
        const QString path = it.value();
        if (path.isEmpty() || path == QLatin1String("/")) {
            continue;
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("path"), path);
        entry.insert(QStringLiteral("id"), it.key());
        entry.insert(QStringLiteral("folder"), m_remoteFolders.contains(path));
        entry.insert(QStringLiteral("placeholder"), m_placeholderPaths.contains(path));
        entry.insert(QStringLiteral("sizeKnown"), m_remoteSizes.contains(path));
        entry.insert(QStringLiteral("size"), m_remoteSizes.value(path, 0));
        entries.append(entry);
    }
    return entries;
}

void GraphClient::materializeFile(const QString &relativePath)
{
    const QString normalized = QDir::cleanPath(relativePath).trimmed();
    if (normalized.isEmpty() || normalized == QLatin1String(".")) {
        Q_EMIT errorOccurred(QStringLiteral("A relative file path is required."));
        return;
    }
    const QString itemId = m_remoteItemIds.value(normalized);
    if (itemId.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Remote file is not indexed: %1").arg(normalized));
        return;
    }
    // Graph exposes file content through /content only.  Reject directories
    // here as a second line of defence for CLI, D-Bus, and FUSE callers.
    if (m_remoteFolders.contains(normalized)) {
        Q_EMIT errorOccurred(QStringLiteral("Remote path is a folder, not a file: %1")
                                 .arg(normalized));
        return;
    }
    // The persisted baseline predates folder facets and may describe an empty
    // directory as a zero-byte item. Confirm that ambiguous case with Graph
    // before allowing FUSE or the CLI to enqueue a /content request.
    if (m_remoteSizes.value(normalized, -1) <= 0) {
        const QUrl url(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1/items/%2?$select=id,folder,file,size")
                           .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_syncDriveId)),
                                QString::fromUtf8(QUrl::toPercentEncoding(itemId))));
        auto *reply = m_network.get(graphRequest(url, m_syncToken.toUtf8()));
        connect(reply, &QNetworkReply::finished, this, [this, reply, normalized, itemId] {
            const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
            if (reply->error() != QNetworkReply::NoError) {
                Q_EMIT errorOccurred(networkError(reply,
                                                 QStringLiteral("Could not inspect remote item.")));
                return;
            }
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                Q_EMIT errorOccurred(QStringLiteral("Graph returned invalid item metadata."));
                return;
            }
            const QJsonObject item = document.object();
            if (item.contains(QStringLiteral("folder"))) {
                m_remoteFolders.insert(normalized);
                m_remoteFolderIds.insert(normalized, itemId);
                Q_EMIT errorOccurred(QStringLiteral("Remote path is a folder, not a file: %1")
                                         .arg(normalized));
                return;
            }
            queueMaterialization(normalized, itemId);
        });
        return;
    }
    queueMaterialization(normalized, itemId);
}

void GraphClient::materializePath(const QString &relativePath)
{
    const QString normalized = QDir::cleanPath(relativePath).trimmed();
    const QString prefix = normalized + QLatin1Char('/');
    if (m_remoteFolders.contains(normalized)) {
        QStringList paths;
        for (auto it = m_remoteItemIds.cbegin(); it != m_remoteItemIds.cend(); ++it) {
            if (it.key().startsWith(prefix) && !m_remoteFolders.contains(it.key())) {
                paths.append(it.key());
            }
        }
        std::sort(paths.begin(), paths.end());
        for (const QString &path : paths) {
            queueMaterialization(path, m_remoteItemIds.value(path));
        }
        log(QStringLiteral("Graph sync: Keep Local queued %1 file(s) below %2")
                .arg(paths.size()).arg(normalized));
        return;
    }
    materializeFile(normalized);
}

void GraphClient::queueMaterialization(const QString &normalized, const QString &itemId)
{
    const QString localPath = safeLocalPath(normalized);
    if (localPath.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Unsafe remote path rejected: %1").arg(normalized));
        return;
    }
    if (QFileInfo(localPath).isFile() && !m_placeholderPaths.contains(normalized)) {
        return;
    }
    // The request is kept separate from the inherited policy: Release Local
    // Cache avoids normal downloads, but a FUSE read is an explicit exception.
    m_requestedMaterializations.insert(normalized);
    bool queued = false;
    for (const GraphSyncFile &file : std::as_const(m_pendingFiles)) {
        if (file.relativePath == normalized) {
            queued = true;
            break;
        }
    }
    if (!queued) {
        m_pendingFiles.enqueue({itemId, normalized, m_remoteSizes.value(normalized, -1)});
    }
    processNextFile();
}

void GraphClient::evictPath(const QString &relativePath)
{
    const QString normalized = QDir::cleanPath(relativePath).trimmed();
    if (normalized.isEmpty() || normalized == QLatin1String(".")) {
        Q_EMIT errorOccurred(QStringLiteral("A relative file or folder path is required."));
        return;
    }
    // A selected path receives an OnDemand override, so Release Local cache
    // also works for profiles created with the older KeepLocal default.
    const QString localRoot = safeLocalPath(normalized);
    if (localRoot.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Unsafe local path rejected: %1").arg(normalized));
        return;
    }

    const QString prefix = normalized + QLatin1Char('/');
    QStringList files;
    for (auto it = m_remoteItemIds.cbegin(); it != m_remoteItemIds.cend(); ++it) {
        if (it.key() == normalized || it.key().startsWith(prefix)) {
            if (!m_remoteFolders.contains(it.key())) {
                files.append(it.key());
            }
        }
    }
    for (const QString &path : m_localSignatures.keys()) {
        if (m_remoteItemIds.contains(path)
            && (path == normalized || path.startsWith(prefix))) {
            files.append(path);
        }
    }
    files.removeDuplicates();
    std::sort(files.begin(), files.end());
    if (files.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Remote file or folder is not indexed: %1")
                                 .arg(normalized));
        return;
    }

    // Validate every file first, so a dirty file cannot leave a folder half
    // evicted while a later file is protected from data loss.
    for (const QString &path : files) {
        const QString current = localFileSignature(path);
        const QString baseline = m_localSignatures.value(path);
        if (!current.isEmpty() && baseline.isEmpty()) {
            Q_EMIT errorOccurred(QStringLiteral("Local file is not tracked: %1").arg(path));
            return;
        }
        if (!current.isEmpty() && !baseline.isEmpty() && current != baseline) {
            Q_EMIT errorOccurred(QStringLiteral("Local changes must be uploaded first: %1")
                                     .arg(path));
            return;
        }
    }

    for (const QString &path : files) {
        createPlaceholder(path);
        m_localSignatures.remove(path);
        m_localMetadata.remove(path);
        log(QStringLiteral("Graph sync: evicted local %1 (On demand)").arg(path));
    }
    setPathPolicy(normalized, LocalAvailability::OnDemand);
    Q_EMIT placeholderStateChanged(placeholderPaths());
    Q_EMIT localStateChanged(localSignatures(), remotePaths());
}

void GraphClient::fetchQuota(const QString &driveId, const QString &accessToken)
{
    if (driveId.isEmpty() || accessToken.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("A drive ID and access token are required."));
        return;
    }

    const QUrl url(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1?$select=quota")
                      .arg(QString::fromUtf8(QUrl::toPercentEncoding(driveId))));
    QNetworkRequest request = graphRequest(url, accessToken.toUtf8());

    auto *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT errorOccurred(networkError(reply, QStringLiteral("Could not read drive quota.")));
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            Q_EMIT errorOccurred(QStringLiteral("Graph returned invalid JSON."));
            return;
        }

        const QJsonObject quota = document.object().value(QStringLiteral("quota")).toObject();
        if (quota.isEmpty()) {
            Q_EMIT errorOccurred(QStringLiteral("Graph response did not contain drive quota."));
            return;
        }
        Q_EMIT quotaReceived(StorageQuota::fromGraphObject(quota, QDateTime::currentDateTimeUtc()));
    });
}

void GraphClient::fetchCurrentDrive(const QString &accessToken)
{
    if (accessToken.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("An access token is required."));
        return;
    }

    QNetworkRequest request = graphRequest(
        QUrl(QStringLiteral("https://graph.microsoft.com/v1.0/me/drive?$select=id,quota")),
        accessToken.toUtf8());
    auto *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT errorOccurred(networkError(reply, QStringLiteral("Could not discover the current drive.")));
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            Q_EMIT errorOccurred(QStringLiteral("Graph returned invalid drive JSON."));
            return;
        }

        const QJsonObject object = document.object();
        const QString driveId = object.value(QStringLiteral("id")).toString();
        const QJsonObject quotaObject = object.value(QStringLiteral("quota")).toObject();
        if (driveId.isEmpty() || quotaObject.isEmpty()) {
            Q_EMIT errorOccurred(QStringLiteral("Graph response did not contain drive information."));
            return;
        }
        Q_EMIT driveDiscovered(driveId,
                               StorageQuota::fromGraphObject(quotaObject,
                                                             QDateTime::currentDateTimeUtc()));
    });
}

void GraphClient::fetchRootFolders(const QString &driveId, const QString &accessToken)
{
    if (driveId.isEmpty() || accessToken.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("A drive ID and access token are required."));
        return;
    }

    const QUrl url(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1/root/children")
                      .arg(QString::fromUtf8(QUrl::toPercentEncoding(driveId))));
    QNetworkRequest request = graphRequest(url, accessToken.toUtf8());
    request.setUrl(QUrl(url.toString() + QStringLiteral(
        "?$select=id,name,folder,deleted,eTag,parentReference,size")));
    auto *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT errorOccurred(networkError(reply, QStringLiteral("Could not list remote folders.")));
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            Q_EMIT errorOccurred(QStringLiteral("Graph returned invalid root-folder JSON."));
            return;
        }
        QList<GraphRemoteFolder> folders;
        for (const QJsonValue &value : document.object().value(QStringLiteral("value")).toArray()) {
            const QJsonObject item = value.toObject();
            if (item.contains(QStringLiteral("deleted")) || !item.contains(QStringLiteral("folder"))) {
                continue;
            }
            const QString id = item.value(QStringLiteral("id")).toString();
            const QString name = item.value(QStringLiteral("name")).toString();
            if (!id.isEmpty() && !name.isEmpty()) {
                folders.append({id, name});
            }
        }
        Q_EMIT rootFoldersReceived(folders);
    });
}

void GraphClient::synchronize(const QString &driveId, const QString &accessToken,
                              const QString &localDirectory,
                              const QStringList &includedFolders,
                              const QStringList &excludedFolders)
{
    if (driveId.isEmpty() || accessToken.isEmpty() || localDirectory.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("A drive, access token, and local directory are required."));
        return;
    }
    m_syncDriveId = driveId;
    m_syncToken = accessToken;
    // A forced resync stops polling first to prevent local uploads from
    // racing the remote pull; the remote transfer scheduler must be enabled
    // again for the queued files to actually download.
    m_monitoringEnabled = true;
    m_syncDirectory = QDir::cleanPath(QFileInfo(localDirectory).absoluteFilePath());
    m_includedFolders = includedFolders;
    m_excludedFolders = excludedFolders;
    m_pendingFolders.clear();
    m_pendingFiles.clear();
    m_pendingUploads.clear();
    m_remoteItemIds.clear();
    m_remoteFolderIds.clear();
    m_remotePathsById.clear();
    m_uploadInProgress = false;
    m_downloadedFiles = 0;
    m_totalFiles = 0;
    if (!QDir().mkpath(m_syncDirectory)) {
        Q_EMIT errorOccurred(QStringLiteral("The local synchronization directory could not be created."));
        return;
    }
    m_pendingFolders.enqueue({QStringLiteral("root"), {}});
    m_remoteFolderIds.insert({}, QStringLiteral("root"));
    m_remotePathsById.insert(QStringLiteral("root"), {});
    log(QStringLiteral("Graph sync: starting remote enumeration in %1")
                 .arg(m_syncDirectory));
    Q_EMIT syncProgress(0, {});
    processNextFolder();
}

void GraphClient::refreshSelectedFolders(const QString &driveId, const QString &accessToken,
                                         const QString &localDirectory,
                                         const QStringList &includedFolders,
                                         const QStringList &excludedFolders)
{
    if (driveId.isEmpty() || accessToken.isEmpty() || localDirectory.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("A drive, access token, and local directory are required."));
        return;
    }
    // Folder-policy changes reuse the persisted hashes, IDs, eTags, and delta
    // cursor. Enumeration is only a discovery pass for newly selected data.
    m_syncDriveId = driveId;
    m_syncToken = accessToken;
    m_syncDirectory = QDir::cleanPath(QFileInfo(localDirectory).absoluteFilePath());
    m_monitoringEnabled = true;
    m_includedFolders = includedFolders;
    m_excludedFolders = excludedFolders;
    m_pendingFolders.clear();
    m_pendingFiles.clear();
    m_downloadedFiles = 0;
    m_totalFiles = 0;
    if (!QDir().mkpath(m_syncDirectory)) {
        Q_EMIT errorOccurred(QStringLiteral("The local synchronization directory could not be created."));
        return;
    }
    m_pendingFolders.enqueue({QStringLiteral("root"), {}});
    m_remoteFolderIds.insert({}, QStringLiteral("root"));
    m_remotePathsById.insert(QStringLiteral("root"), {});
    log(QStringLiteral("Graph sync: refreshing selected folders without resetting the baseline"));
    Q_EMIT syncProgress(0, {});
    processNextFolder();
}

void GraphClient::processNextFolder()
{
    if (m_pendingFolders.isEmpty()) {
        m_totalFiles = m_pendingFiles.size();
        // Keep this diagnostic beside the scheduler boundary: enumeration can
        // succeed while a stale service or a disabled materializer leaves the
        // transfer queue untouched. It also makes forced remote refreshes
        // auditable without dumping file contents or access tokens.
        log(QStringLiteral("Graph sync: comparison queued %1 file(s), mode=on-demand, monitoring=%2")
                .arg(m_pendingFiles.size())
                .arg(m_monitoringEnabled ? QStringLiteral("yes") : QStringLiteral("no")));
        processNextFile();
        return;
    }

    const GraphSyncFolder folder = m_pendingFolders.dequeue();
    const QUrl url = folder.id == QLatin1String("root")
        ? QUrl(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1/root/children")
                   .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_syncDriveId))))
        : QUrl(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1/items/%2/children")
                   .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_syncDriveId)),
                        QString::fromUtf8(QUrl::toPercentEncoding(folder.id))));
    QNetworkRequest request = graphRequest(url, m_syncToken.toUtf8());
    request.setUrl(QUrl(url.toString() + QStringLiteral(
        "?$select=id,name,folder,file,deleted,eTag,parentReference,size")));
    const QString displayPath = folder.relativePath.isEmpty()
        ? QStringLiteral("/") : folder.relativePath;
    log(QStringLiteral("Graph sync: listing %1").arg(displayPath));
    Q_EMIT syncProgress(0, QStringLiteral("Listing %1").arg(displayPath));
    auto *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, folder, displayPath] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT errorOccurred(networkError(reply, QStringLiteral("Could not read a remote folder.")));
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            Q_EMIT errorOccurred(QStringLiteral("Graph returned invalid folder JSON."));
            return;
        }
        const QJsonArray values = document.object().value(QStringLiteral("value")).toArray();
        int foldersFound = 0;
        int filesFound = 0;
        for (const QJsonValue &value : values) {
            const QJsonObject item = value.toObject();
            if (item.contains(QStringLiteral("deleted"))) {
                continue;
            }
            const QString name = item.value(QStringLiteral("name")).toString();
            const QString relativePath = folder.relativePath.isEmpty()
                ? name
                : folder.relativePath + QLatin1Char('/') + name;
            if (name.isEmpty() || !isIncluded(relativePath)) {
                if (item.contains(QStringLiteral("folder")) && shouldTraverse(relativePath)) {
                    m_pendingFolders.enqueue({item.value(QStringLiteral("id")).toString(), relativePath});
                }
                continue;
            }
            if (item.contains(QStringLiteral("folder"))) {
                ++foldersFound;
                m_remoteFolders.insert(relativePath);
                m_remoteSizes.insert(relativePath, 0);
                m_remotePathsById.insert(item.value(QStringLiteral("id")).toString(), relativePath);
                m_remoteEtags.insert(item.value(QStringLiteral("id")).toString(),
                                     item.value(QStringLiteral("eTag")).toString());
                const QString parentId = item.value(QStringLiteral("parentReference")).toObject()
                                             .value(QStringLiteral("id")).toString();
                if (!parentId.isEmpty()) {
                    m_remotePathsById.insert(parentId, folder.relativePath);
                }
                m_remoteFolderIds.insert(relativePath, item.value(QStringLiteral("id")).toString());
                const QString localPath = safeLocalPath(relativePath);
                if (shouldMaterializePath(relativePath) && !localPath.isEmpty()
                    && !QDir().mkpath(localPath)) {
                    Q_EMIT errorOccurred(QStringLiteral("Could not create local folder: %1").arg(relativePath));
                    return;
                }
                if (shouldTraverse(relativePath)) {
                    m_pendingFolders.enqueue({item.value(QStringLiteral("id")).toString(), relativePath});
                }
            } else if (item.contains(QStringLiteral("file"))) {
                ++filesFound;
                const QString itemId = item.value(QStringLiteral("id")).toString();
                const QString eTag = item.value(QStringLiteral("eTag")).toString();
                const QFileInfo localInfo(safeLocalPath(relativePath));
                const qint64 remoteSize = item.value(QStringLiteral("size")).toVariant().toLongLong();
                const bool localBaselineMatches = localInfo.isFile()
                    && m_localSignatures.value(relativePath) == localFileSignature(localInfo.absoluteFilePath());
                const bool sizeMatches = remoteSize < 0 || localInfo.size() == remoteSize;
                const bool alreadyCurrent = !m_placeholderPaths.contains(relativePath)
                    && m_remoteItemIds.value(relativePath) == itemId
                    && m_remoteEtags.value(itemId) == eTag
                    && localBaselineMatches && sizeMatches;
                m_remotePathsById.insert(itemId, relativePath);
                m_remoteEtags.insert(itemId, eTag);
                m_remoteItemIds.insert(relativePath, itemId);
                m_remoteSizes.insert(relativePath, remoteSize);
                if (!alreadyCurrent && !isRemoteFolderPath(relativePath)) {
                    m_pendingFiles.enqueue({itemId, relativePath,
                                            item.value(QStringLiteral("size")).toVariant().toLongLong()});
                }
            }
        }
        log(QStringLiteral("Graph sync: %1 contained %2 folders and %3 files")
                     .arg(folder.relativePath.isEmpty() ? QStringLiteral("/")
                                                         : folder.relativePath)
                     .arg(foldersFound).arg(filesFound));
        const QString nextLink = document.object().value(QStringLiteral("@odata.nextLink"))
                                     .toString();
        if (!nextLink.isEmpty()) {
            log(QStringLiteral("Graph sync: requesting next page for %1").arg(displayPath));
            QNetworkRequest nextRequest = graphRequest(QUrl(nextLink), m_syncToken.toUtf8());
            auto *nextReply = m_network.get(nextRequest);
            connect(nextReply, &QNetworkReply::finished, this,
                    [this, nextReply, folder] {
                const auto cleanup = qScopeGuard([nextReply] { nextReply->deleteLater(); });
                if (nextReply->error() != QNetworkReply::NoError) {
                    Q_EMIT errorOccurred(networkError(
                        nextReply, QStringLiteral("Could not read the next remote folder page.")));
                    return;
                }
                // Reuse the normal queue step after this page; Graph's nextLink
                // is uncommon for small folders, so keep this path explicit and
                // report it rather than silently dropping later items.
                QJsonParseError nextParseError;
                const QJsonDocument nextDocument = QJsonDocument::fromJson(
                    nextReply->readAll(), &nextParseError);
                if (nextParseError.error != QJsonParseError::NoError
                    || !nextDocument.isObject()) {
                    Q_EMIT errorOccurred(QStringLiteral("Graph returned invalid paginated folder JSON."));
                    return;
                }
                for (const QJsonValue &value : nextDocument.object().value(QStringLiteral("value")).toArray()) {
                    const QJsonObject item = value.toObject();
                    if (item.contains(QStringLiteral("deleted"))) {
                        continue;
                    }
                    const QString name = item.value(QStringLiteral("name")).toString();
                    const QString relativePath = folder.relativePath.isEmpty()
                        ? name : folder.relativePath + QLatin1Char('/') + name;
                    if (item.contains(QStringLiteral("folder"))) {
                        m_remoteFolders.insert(relativePath);
                        m_remoteSizes.insert(relativePath, 0);
                        m_remotePathsById.insert(item.value(QStringLiteral("id")).toString(),
                                                 relativePath);
                        m_remoteFolderIds.insert(relativePath, item.value(QStringLiteral("id")).toString());
                        if (isIncluded(relativePath)) {
                            const QString localPath = safeLocalPath(relativePath);
                            if (shouldMaterializePath(relativePath) && !localPath.isEmpty()) {
                                QDir().mkpath(localPath);
                            }
                            if (shouldTraverse(relativePath)) {
                                m_pendingFolders.enqueue({item.value(QStringLiteral("id")).toString(), relativePath});
                            }
                        }
                    } else if (item.contains(QStringLiteral("file")) && isIncluded(relativePath)) {
                        const QString itemId = item.value(QStringLiteral("id")).toString();
                        const QString eTag = item.value(QStringLiteral("eTag")).toString();
                        const QFileInfo localInfo(safeLocalPath(relativePath));
                        const qint64 remoteSize = item.value(QStringLiteral("size"))
                                                      .toVariant().toLongLong();
                        const bool localBaselineMatches = localInfo.isFile()
                            && m_localSignatures.value(relativePath)
                                   == localFileSignature(localInfo.absoluteFilePath());
                        const bool sizeMatches = remoteSize < 0 || localInfo.size() == remoteSize;
                        const bool alreadyCurrent = !m_placeholderPaths.contains(relativePath)
                            && m_remoteItemIds.value(relativePath) == itemId
                            && m_remoteEtags.value(itemId) == eTag
                            && localBaselineMatches && sizeMatches;
                        m_remotePathsById.insert(itemId, relativePath);
                        m_remoteEtags.insert(itemId, eTag);
                        m_remoteItemIds.insert(relativePath, itemId);
                        m_remoteSizes.insert(relativePath, remoteSize);
                        if (!alreadyCurrent && !isRemoteFolderPath(relativePath)) {
                            m_pendingFiles.enqueue({itemId, relativePath,
                                                    item.value(QStringLiteral("size"))
                                                        .toVariant().toLongLong()});
                        }
                    }
                }
                processNextFolder();
            });
            return;
        }
        processNextFolder();
    });
}

void GraphClient::processNextFile()
{
    // Transfer callbacks re-enter this scheduler. The delta cursor is
    // persisted only after every file in the page is durable on disk.
    // Filter by the inherited path policy. Keep-local paths download even
    // when the profile default is on-demand; keep-remote paths retain only a
    // placeholder; ordinary on-demand paths wait for a FUSE read.
    QQueue<GraphSyncFile> deferredFiles;
    while (!m_pendingFiles.isEmpty()) {
        const GraphSyncFile file = m_pendingFiles.dequeue();
        if (isRemoteFolderPath(file.relativePath)) {
            // A stale baseline or an unusual delta representation can leave a
            // folder in the legacy queue. Never turn that metadata entry into
            // a Graph /content request; discard it and continue the batch.
            log(QStringLiteral("Graph sync: skipping folder in download queue %1")
                    .arg(file.relativePath));
            continue;
        }
        if (m_requestedMaterializations.contains(file.relativePath)
            || shouldMaterializePath(file.relativePath)) {
            deferredFiles.enqueue(file);
        } else if (shouldKeepRemotePath(file.relativePath)) {
            createPlaceholder(file.relativePath);
        }
    }
    m_pendingFiles = std::move(deferredFiles);
    Q_EMIT placeholderStateChanged(placeholderPaths());
    startPendingDownloads();
    if (m_pendingFiles.isEmpty() && m_activeDownloads.isEmpty()) {
        if (!m_deltaPageFailed && !m_pendingDeltaLink.isEmpty()) {
            // Persist the cursor only after every file in its page has been
            // written and hashed successfully.
            m_deltaLink = m_pendingDeltaLink;
            Q_EMIT deltaLinkChanged(m_pendingDeltaLink);
        }
        m_pendingDeltaLink.clear();
        m_deltaPageFailed = false;
        Q_EMIT syncProgress(100, {});
        Q_EMIT syncFinished();
        Q_EMIT localStateChanged(localSignatures(), remotePaths());
        if (m_monitoringEnabled && !m_uploadTimer.isActive()) {
            scanLocalChanges();
            m_uploadTimer.start();
        }
        return;
    }
}

void GraphClient::startPendingDownloads()
{
    if (!m_monitoringEnabled) {
        return;
    }
    if (!m_pendingFiles.isEmpty()) {
        log(QStringLiteral("Graph sync: starting download scheduler with %1 queued file(s), %2 active")
                .arg(m_pendingFiles.size()).arg(m_activeDownloads.size()));
    }
    // Small files may pass a queued video, while large transfers have a
    // separate cap so one video cannot occupy every download slot.
    while (m_activeDownloads.size() < m_maxConcurrentDownloads && !m_pendingFiles.isEmpty()) {
        int activeLarge = 0;
        for (const auto &active : m_activeDownloads) {
            if (active->file.size > largeUploadThreshold) {
                ++activeLarge;
            }
        }
        int selectedIndex = -1;
        for (int index = 0; index < m_pendingFiles.size(); ++index) {
            const GraphSyncFile &candidate = m_pendingFiles.at(index);
            const bool large = candidate.size > largeUploadThreshold;
            if (!large || activeLarge < m_maxConcurrentLargeTransfers) {
                selectedIndex = index;
                break;
            }
        }
        if (selectedIndex < 0) {
            // Every queued item is large and the independent large-transfer
            // budget is full. Keep the queue intact; a completion callback
            // will re-enter this scheduler and release the next item.
            log(QStringLiteral("Graph sync: large-transfer limit reached; %1 file(s) remain queued")
                    .arg(m_pendingFiles.size()));
            return;
        }
        // Remove exactly the selected item. Rebuilding the queue by repeated
        // dequeue/move made this boundary unnecessarily hard to audit and
        // could lose the tail item when transfers were started concurrently.
        const GraphSyncFile file = m_pendingFiles.takeAt(selectedIndex);
        if (isRemoteFolderPath(file.relativePath)) {
            log(QStringLiteral("Graph sync: skipping folder download %1")
                    .arg(file.relativePath));
            continue;
        }
        if (m_activeDownloads.contains(file.relativePath)) {
            log(QStringLiteral("Graph sync: skipping already active download %1")
                    .arg(file.relativePath));
            continue;
        }
        log(QStringLiteral("Graph sync: scheduling download %1 (%2 MiB, %3 queued after selection)")
                .arg(file.relativePath)
                .arg(file.size / (1024 * 1024))
                .arg(m_pendingFiles.size()));
        startDownload(file);
    }
}

void GraphClient::startDownload(const GraphSyncFile &file)
{
    logProgress(QStringLiteral("Graph sync: downloading %1 (%2/%3)")
                 .arg(file.relativePath).arg(m_downloadedFiles + 1).arg(m_totalFiles));
    Q_EMIT syncProgress(0, file.relativePath);
    const QString localPath = safeLocalPath(file.relativePath);
    if (localPath.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Unsafe remote path rejected: %1").arg(file.relativePath));
        processNextFile();
        return;
    }
    QDir().mkpath(QFileInfo(localPath).absolutePath());
    auto transfer = std::make_shared<DownloadTransfer>();
    transfer->file = file;
    transfer->localPath = localPath;
    transfer->output = std::make_unique<QSaveFile>(localPath);
    if (!transfer->output->open(QIODevice::WriteOnly)) {
        Q_EMIT errorOccurred(QStringLiteral("Could not open local file for download: %1")
                                 .arg(file.relativePath));
        processNextFile();
        return;
    }
    m_activeDownloads.insert(file.relativePath, transfer);
    const QString itemId = QString::fromUtf8(QUrl::toPercentEncoding(file.id));
    const QUrl url(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1/items/%2/content")
                       .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_syncDriveId)), itemId));
    QNetworkRequest request = graphRequest(url, m_syncToken.toUtf8());
    // /content normally responds with a short-lived download URL. Do not let
    // Qt forward the Graph bearer to that different host; the temporary URL
    // authenticates itself and must be requested without Authorization.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    auto *reply = m_network.get(request);
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply, transfer] {
        updateDownloadMetadata(reply, transfer);
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, transfer] {
        writeDownloadChunk(reply, transfer);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, transfer] {
        processDownloadedReply(reply, transfer);
    });
}

void GraphClient::updateDownloadMetadata(QNetworkReply *reply,
                                         const std::shared_ptr<DownloadTransfer> &transfer)
{
    const qint64 contentLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    if (contentLength > 0) {
        transfer->totalBytes = contentLength;
    }
}

void GraphClient::writeDownloadChunk(QNetworkReply *reply,
                                     const std::shared_ptr<DownloadTransfer> &transfer)
{
    if (transfer->writeFailed || !transfer->output) {
        return;
    }
    const QByteArray chunk = reply->readAll();
    if (transfer->output->write(chunk) < 0) {
        transfer->writeFailed = true;
        return;
    }
    transfer->bytes += chunk.size();
    if (transfer->totalBytes > 0) {
        const int progress = static_cast<int>((transfer->bytes * 100) / transfer->totalBytes);
        if (progress >= 100 || progress >= transfer->lastProgress + 5) {
            transfer->lastProgress = progress;
            logProgress(QStringLiteral("Graph sync: downloading %1 (%2%, %3/%4 MiB)")
                    .arg(transfer->file.relativePath)
                    .arg(progress)
                    .arg(transfer->bytes / (1024 * 1024))
                    .arg(transfer->totalBytes / (1024 * 1024)));
            Q_EMIT syncProgress(progress, transfer->file.relativePath);
        }
    } else if (transfer->bytes - transfer->lastLogBytes >= 10 * 1024 * 1024) {
        transfer->lastLogBytes = transfer->bytes;
        logProgress(QStringLiteral("Graph sync: downloading %1 (%2 MiB)")
                .arg(transfer->file.relativePath)
                .arg(transfer->bytes / (1024 * 1024)));
    }
}

void GraphClient::processDownloadedReply(QNetworkReply *reply,
                                         const std::shared_ptr<DownloadTransfer> &transfer)
{
    const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) {
        const QUrl location = reply->url().resolved(
            reply->header(QNetworkRequest::LocationHeader).toUrl());
        if (!location.isValid() || location.scheme() != QLatin1String("https")) {
            m_deltaPageFailed = true;
            m_pendingDeltaLink.clear();
            m_activeDownloads.remove(transfer->file.relativePath);
            Q_EMIT errorOccurred(QStringLiteral("Graph returned an invalid file download URL."));
            processNextFile();
            return;
        }
        logProgress(QStringLiteral("Graph sync: following temporary download URL for %1")
                     .arg(transfer->file.relativePath));
        QNetworkRequest downloadRequest(location);
        downloadRequest.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        auto *downloadReply = m_network.get(downloadRequest);
        connect(downloadReply, &QNetworkReply::metaDataChanged, this,
                [this, downloadReply, transfer] {
            updateDownloadMetadata(downloadReply, transfer);
        });
        connect(downloadReply, &QNetworkReply::readyRead, this,
                [this, downloadReply, transfer] {
            writeDownloadChunk(downloadReply, transfer);
        });
        connect(downloadReply, &QNetworkReply::finished, this,
                [this, downloadReply, transfer] {
            processDownloadedReply(downloadReply, transfer);
        });
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        m_deltaPageFailed = true;
        m_pendingDeltaLink.clear();
        m_activeDownloads.remove(transfer->file.relativePath);
            Q_EMIT errorOccurred(networkError(reply, QStringLiteral("Could not download a remote file.")));
        processNextFile();
        return;
    }
    updateDownloadMetadata(reply, transfer);
    writeDownloadChunk(reply, transfer);
    if (transfer->writeFailed || !transfer->output || !transfer->output->commit()) {
        m_deltaPageFailed = true;
        m_pendingDeltaLink.clear();
        m_activeDownloads.remove(transfer->file.relativePath);
        Q_EMIT errorOccurred(QStringLiteral("Could not write local file: %1")
                                 .arg(transfer->file.relativePath));
        processNextFile();
        return;
    }
    m_activeDownloads.remove(transfer->file.relativePath);
    m_requestedMaterializations.remove(transfer->file.relativePath);
    logProgress(QStringLiteral("Graph sync: downloaded %1 (100%, %2 MiB)")
            .arg(transfer->file.relativePath).arg(transfer->bytes / (1024 * 1024)));
    const QString signature = localFileSignature(transfer->localPath);
    if (!signature.isEmpty()) {
        const bool wasPlaceholder = m_placeholderPaths.remove(transfer->file.relativePath) > 0;
        m_localSignatures.insert(transfer->file.relativePath, signature);
        const QFileInfo fileInfo(transfer->localPath);
        m_localMetadata.insert(transfer->file.relativePath,
                               {fileInfo.size(), fileInfo.lastModified()});
        if (wasPlaceholder) {
            Q_EMIT placeholderStateChanged(placeholderPaths());
        }
    }
    ++m_downloadedFiles;
    Q_EMIT syncProgress(100, transfer->file.relativePath);
    logProgress(QStringLiteral("Graph sync: continuing download queue (%1 pending, %2 active)")
                    .arg(m_pendingFiles.size()).arg(m_activeDownloads.size()));
    // Large-file callbacks can finish while Qt is still unwinding the network
    // reply. Schedule the next queue step after that callback has returned.
    QTimer::singleShot(0, this, &GraphClient::processNextFile);
}

void GraphClient::scanLocalChanges()
{
    if (m_renameInProgress || m_syncDirectory.isEmpty()) {
        return;
    }
    // Scanning must continue while transfers are active: a newly copied small
    // file should be added to the pending queue immediately instead of waiting
    // for every earlier large upload to finish. m_pendingUploadPaths makes the
    // scan idempotent for files already queued or in flight.
    QDirIterator iterator(m_syncDirectory, QDir::Files, QDirIterator::Subdirectories);
    QSet<QString> currentPaths;
    QHash<QString, QString> currentSignatures;
    while (iterator.hasNext()) {
        const QString localPath = iterator.next();
        const QString relativePath = QDir(m_syncDirectory).relativeFilePath(localPath);
        if (!isIncluded(relativePath) || m_placeholderPaths.contains(relativePath)) {
            continue;
        }
        currentPaths.insert(relativePath);
        if (m_pendingUploadPaths.contains(relativePath)
            || m_activeUploads.contains(relativePath)) {
            // A queued or active upload already owns this snapshot. Rehashing
            // large videos on every ten-second scan would block the Qt event
            // loop, delaying transfer callbacks and D-Bus status requests.
            continue;
        }
        const QFileInfo fileInfo(localPath);
        const QPair<qint64, QDateTime> metadata{fileInfo.size(), fileInfo.lastModified()};
        // Hashing is intentionally the correctness fallback. For unchanged
        // files, size+mtime lets the 10-second polling loop avoid rereading
        // large videos from disk on every pass.
        if (m_localSignatures.contains(relativePath)
            && m_localMetadata.value(relativePath) == metadata) {
            currentSignatures.insert(relativePath, m_localSignatures.value(relativePath));
            continue;
        }
        const QString signature = localFileSignature(localPath);
        if (signature.isEmpty()) {
            continue;
        }
        currentSignatures.insert(relativePath, signature);
        m_localMetadata.insert(relativePath, metadata);
    }
    QSet<QString> renamedOldPaths;
    QSet<QString> renamedNewPaths;
    for (auto it = m_localSignatures.cbegin(); it != m_localSignatures.cend(); ++it) {
        if (currentPaths.contains(it.key()) || !isIncluded(it.key())) {
            continue;
        }
        for (auto currentIt = currentSignatures.cbegin(); currentIt != currentSignatures.cend();
             ++currentIt) {
            if (currentIt.value() != it.value() || currentPaths.contains(it.key())) {
                continue;
            }
            if (QFileInfo(it.key()).path() != QFileInfo(currentIt.key()).path()) {
                continue;
            }
            const QString itemId = m_remoteItemIds.value(it.key());
            if (itemId.isEmpty() || m_pendingRemoteRenamePaths.contains(it.key())
                || m_pendingRemoteRenamePaths.contains(currentIt.key())) {
                continue;
            }
            m_pendingRemoteRenames.enqueue({it.key(), currentIt.key(), itemId});
            m_pendingRemoteRenamePaths.insert(it.key());
            m_pendingRemoteRenamePaths.insert(currentIt.key());
            renamedOldPaths.insert(it.key());
            renamedNewPaths.insert(currentIt.key());
            break;
        }
    }
    for (auto it = currentSignatures.cbegin(); it != currentSignatures.cend(); ++it) {
        if (renamedNewPaths.contains(it.key())
            || m_localSignatures.value(it.key()) == it.value()) {
            continue;
        }
        if (!m_pendingUploadPaths.contains(it.key())) {
            m_pendingUploadPaths.insert(it.key());
            m_pendingUploads.enqueue({it.key(), QDir(m_syncDirectory).filePath(it.key())});
        }
    }
    for (auto it = m_localSignatures.cbegin(); it != m_localSignatures.cend(); ++it) {
        if (renamedOldPaths.contains(it.key()) || currentPaths.contains(it.key()) || !isIncluded(it.key())
            || QFileInfo(safeLocalPath(it.key())).exists()) {
            continue;
        }
        const QString itemId = m_remoteItemIds.value(it.key());
        if (!itemId.isEmpty() && !m_pendingRemoteDeletePaths.contains(it.key())) {
            m_pendingRemoteDeletes.enqueue({it.key(), itemId});
            m_pendingRemoteDeletePaths.insert(it.key());
        }
    }
    processPendingLocalOperations();
}

void GraphClient::processPendingLocalOperations()
{
    if (m_renameInProgress || !m_pendingRemoteRenames.isEmpty()) {
        renameNextRemoteFile();
        return;
    }
    deleteNextRemoteFile();
    uploadNextLocalFile();
}

void GraphClient::renameNextRemoteFile()
{
    if (m_renameInProgress || m_pendingRemoteRenames.isEmpty()) {
        return;
    }
    m_renameInProgress = true;
    const GraphRemoteRename rename = m_pendingRemoteRenames.dequeue();
    const QUrl url(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1/items/%2")
                       .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_syncDriveId)),
                            QString::fromUtf8(QUrl::toPercentEncoding(rename.itemId))));
    QNetworkRequest request = graphRequest(url, m_syncToken.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray body = QJsonDocument(QJsonObject{
        {QStringLiteral("name"), QFileInfo(rename.newPath).fileName()}}).toJson(QJsonDocument::Compact);
    log(QStringLiteral("Graph sync: renaming remote %1 → %2")
            .arg(rename.oldPath, rename.newPath));
    auto *reply = m_network.sendCustomRequest(request, QByteArrayLiteral("PATCH"), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, rename] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
        if (reply->error() != QNetworkReply::NoError) {
            m_pendingRemoteRenamePaths.remove(rename.oldPath);
            m_pendingRemoteRenamePaths.remove(rename.newPath);
            Q_EMIT errorOccurred(networkError(reply, QStringLiteral("Could not rename remote file: %1")
                                             .arg(rename.oldPath)));
        } else {
            log(QStringLiteral("Graph sync: renamed remote %1 → %2")
                    .arg(rename.oldPath, rename.newPath));
            const QString signature = m_localSignatures.take(rename.oldPath);
            m_localSignatures.insert(rename.newPath, signature);
            if (m_localMetadata.contains(rename.oldPath)) {
                m_localMetadata.insert(rename.newPath, m_localMetadata.take(rename.oldPath));
            }
            m_remoteItemIds.remove(rename.oldPath);
            m_remoteItemIds.insert(rename.newPath, rename.itemId);
            m_remotePathsById.insert(rename.itemId, rename.newPath);
            // Graph commonly assigns a new eTag to a renamed item even when
            // its bytes are unchanged. Suppress the echoed delta by identity
            // and path; that delta will still refresh the stored eTag without
            // causing an unnecessary content download.
            m_suppressedRemoteItemIds.insert(rename.itemId);
            m_suppressedRemotePaths.insert(rename.newPath);
            m_pendingRemoteRenamePaths.remove(rename.oldPath);
            m_pendingRemoteRenamePaths.remove(rename.newPath);
            Q_EMIT localStateChanged(localSignatures(), remotePaths());
        }
        m_renameInProgress = false;
        processPendingLocalOperations();
    });
}

void GraphClient::deleteNextRemoteFile()
{
    if (m_deleteInProgress || m_pendingRemoteDeletes.isEmpty()) {
        return;
    }
    m_deleteInProgress = true;
    const GraphRemoteDelete file = m_pendingRemoteDeletes.dequeue();
    const QUrl url(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1/items/%2")
                       .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_syncDriveId)),
                            QString::fromUtf8(QUrl::toPercentEncoding(file.itemId))));
    QNetworkRequest request = graphRequest(url, m_syncToken.toUtf8());
    log(QStringLiteral("Graph sync: deleting remote %1 (item %2)")
            .arg(file.relativePath, file.itemId));
    auto *reply = m_network.deleteResource(request);
    m_activeDeleteReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, file] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
        if (m_activeDeleteReply == reply) {
            m_activeDeleteReply.clear();
        }
        if (reply->error() != QNetworkReply::NoError) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 404) {
                // A persisted item ID can become stale when the remote item was
                // deleted and recreated with the same name. The desired state
                // is already satisfied for this ID, so do not retry forever.
                log(QStringLiteral("Graph sync: remote %1 was already absent; dropping stale item ID")
                        .arg(file.relativePath));
                m_pendingRemoteDeletePaths.remove(file.relativePath);
                m_remotePathsById.remove(file.itemId);
                m_remoteItemIds.remove(file.relativePath);
                if (!QFileInfo(safeLocalPath(file.relativePath)).exists()) {
                    m_localSignatures.remove(file.relativePath);
                    m_localMetadata.remove(file.relativePath);
                }
                Q_EMIT localStateChanged(localSignatures(), remotePaths());
                m_deleteInProgress = false;
                deleteNextRemoteFile();
                return;
            }
            m_pendingRemoteDeletePaths.remove(file.relativePath);
            Q_EMIT errorOccurred(networkError(reply, QStringLiteral("Could not delete remote file: %1")
                                             .arg(file.relativePath)));
        } else {
            log(QStringLiteral("Graph sync: deleted remote %1").arg(file.relativePath));
            m_localSignatures.remove(file.relativePath);
            m_localMetadata.remove(file.relativePath);
            m_remotePathsById.remove(file.itemId);
            m_remoteItemIds.remove(file.relativePath);
            m_pendingRemoteDeletePaths.remove(file.relativePath);
            Q_EMIT localStateChanged(localSignatures(), remotePaths());
        }
        m_deleteInProgress = false;
        deleteNextRemoteFile();
    });
}

void GraphClient::uploadNextLocalFile()
{
    startPendingUploads();
    if (m_uploadBatchActive && m_pendingUploads.isEmpty() && m_activeUploads.isEmpty()) {
        m_uploadBatchActive = false;
        m_uploadInProgress = false;
        Q_EMIT syncProgress(100, {});
        Q_EMIT syncFinished();
        Q_EMIT localStateChanged(localSignatures(), remotePaths());
    }
}

void GraphClient::startPendingUploads()
{
    if (!m_monitoringEnabled) {
        return;
    }
    if (!m_pendingUploads.isEmpty()) {
        m_uploadBatchActive = true;
    }
    while (m_activeUploads.size() < m_maxConcurrentUploads && !m_pendingUploads.isEmpty()) {
        int activeLarge = 0;
        for (const auto &active : m_activeUploads) {
            if (QFileInfo(active->file.localPath).size() > largeUploadThreshold) {
                ++activeLarge;
            }
        }

        int selectedIndex = -1;
        for (int index = 0; index < m_pendingUploads.size(); ++index) {
            const bool large = QFileInfo(m_pendingUploads.at(index).localPath).size()
                > largeUploadThreshold;
            if (!large || activeLarge < m_maxConcurrentLargeTransfers) {
                selectedIndex = index;
                break;
            }
        }
        if (selectedIndex < 0) {
            // Keep large files queued until a large-transfer slot is freed;
            // the next upload completion re-enters this scheduler.
            log(QStringLiteral("Graph sync: large-upload limit reached; %1 file(s) remain queued")
                    .arg(m_pendingUploads.size()));
            return;
        }

        // Remove exactly the selected item. Keeping the tail queue intact is
        // essential when a folder scan discovers more files than the active
        // upload limit.
        const GraphLocalFile selected = m_pendingUploads.takeAt(selectedIndex);
        m_pendingUploadPaths.remove(selected.relativePath);
        log(QStringLiteral("Graph sync: scheduling upload %1 (%2 queued after selection)")
                .arg(selected.relativePath).arg(m_pendingUploads.size()));
        startUpload(selected);
    }
}

void GraphClient::startUpload(const GraphLocalFile &file)
{
    // Each upload owns its network reply and state, allowing small files to
    // continue while a large upload session is sending another chunk.
    const QFileInfo info(file.localPath);
    if (!info.exists()) {
        Q_EMIT errorOccurred(QStringLiteral("Local file cannot be uploaded (missing): %1")
                                 .arg(file.relativePath));
        return;
    }

    auto transfer = std::make_shared<UploadTransfer>();
    transfer->file = file;
    m_activeUploads.insert(file.relativePath, transfer);
    m_uploadInProgress = true;
    if (info.size() > largeUploadThreshold) {
        createUploadSession(transfer);
        return;
    }

    QFile input(file.localPath);
    if (!input.open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(QStringLiteral("Could not read local file for upload: %1")
                                 .arg(file.relativePath));
        m_activeUploads.remove(file.relativePath);
        return;
    }
    const QByteArray contents = input.readAll();
    const QString encodedPath = QString::fromUtf8(QUrl::toPercentEncoding(file.relativePath, "/"));
    const QUrl url(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1/root:/%2:/content")
                       .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_syncDriveId)), encodedPath));
    QNetworkRequest request = graphRequest(url, m_syncToken.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    logProgress(QStringLiteral("Graph sync: uploading %1").arg(file.relativePath));
    Q_EMIT syncProgress(0, QStringLiteral("Uploading %1").arg(file.relativePath));
    auto *reply = m_network.put(request, contents);
    connect(reply, &QNetworkReply::finished, this, [this, reply, transfer] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
        if (reply->error() != QNetworkReply::NoError) {
            m_localSignatures.remove(transfer->file.relativePath);
            m_localMetadata.remove(transfer->file.relativePath);
            Q_EMIT errorOccurred(networkError(reply, QStringLiteral("Could not upload local file: %1")
                                             .arg(transfer->file.relativePath)));
            m_activeUploads.remove(transfer->file.relativePath);
            uploadNextLocalFile();
            return;
        }
        finishUpload(transfer, QJsonDocument::fromJson(reply->readAll()).object());
    });
}

void GraphClient::createUploadSession(const std::shared_ptr<UploadTransfer> &transfer)
{
    const QString encodedPath = QString::fromUtf8(QUrl::toPercentEncoding(
        transfer->file.relativePath, "/"));
    const QUrl url(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1/root:/%2:/createUploadSession")
                       .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_syncDriveId)), encodedPath));
    QNetworkRequest request = graphRequest(url, m_syncToken.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QJsonObject body{
        {QStringLiteral("item"), QJsonObject{
            {QStringLiteral("@microsoft.graph.conflictBehavior"), QStringLiteral("replace")},
            {QStringLiteral("name"), QFileInfo(transfer->file.relativePath).fileName()}}}};
    logProgress(QStringLiteral("Graph sync: creating upload session for %1")
                    .arg(transfer->file.relativePath));
    auto *reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, transfer] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT errorOccurred(networkError(reply, QStringLiteral("Could not create upload session: %1")
                                             .arg(transfer->file.relativePath)));
            m_activeUploads.remove(transfer->file.relativePath);
            uploadNextLocalFile();
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        const QString uploadUrl = document.object().value(QStringLiteral("uploadUrl")).toString();
        if (parseError.error != QJsonParseError::NoError || uploadUrl.isEmpty()) {
            Q_EMIT errorOccurred(QStringLiteral("Graph returned an invalid upload session for %1")
                                     .arg(transfer->file.relativePath));
            m_activeUploads.remove(transfer->file.relativePath);
            uploadNextLocalFile();
            return;
        }
        transfer->uploadUrl = QUrl(uploadUrl);
        transfer->size = QFileInfo(transfer->file.localPath).size();
        transfer->offset = 0;
        transfer->lastProgress = -1;
        uploadNextChunk(transfer);
    });
}

void GraphClient::uploadNextChunk(const std::shared_ptr<UploadTransfer> &transfer)
{
    QFile input(transfer->file.localPath);
    if (!input.open(QIODevice::ReadOnly) || !input.seek(transfer->offset)) {
        Q_EMIT errorOccurred(QStringLiteral("Could not read large local file: %1")
                                 .arg(transfer->file.relativePath));
        m_activeUploads.remove(transfer->file.relativePath);
        uploadNextLocalFile();
        return;
    }
    const qint64 remaining = transfer->size - transfer->offset;
    const qint64 chunkSize = qMin(uploadChunkSize, remaining);
    const QByteArray chunk = input.read(chunkSize);
    if (chunk.size() != chunkSize) {
        Q_EMIT errorOccurred(QStringLiteral("Could not read the complete upload chunk: %1")
                                 .arg(transfer->file.relativePath));
        m_activeUploads.remove(transfer->file.relativePath);
        uploadNextLocalFile();
        return;
    }
    const qint64 end = transfer->offset + chunk.size() - 1;
    QNetworkRequest request(transfer->uploadUrl);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setHeader(QNetworkRequest::ContentLengthHeader, chunk.size());
    request.setRawHeader("Content-Range", QByteArray("bytes ")
                         + QByteArray::number(transfer->offset) + '-'
                         + QByteArray::number(end) + '/'
                         + QByteArray::number(transfer->size));
    if (transfer->offset == 0) {
        logProgress(QStringLiteral("Graph sync: uploading large file %1")
                .arg(transfer->file.relativePath));
    }
    const int progress = static_cast<int>((transfer->offset * 100) / transfer->size);
    if (progress >= 100 || progress >= transfer->lastProgress + 5) {
        transfer->lastProgress = progress;
        logProgress(QStringLiteral("Graph sync: uploading %1 (%2%, %3/%4 MiB)")
                .arg(transfer->file.relativePath)
                .arg(progress)
                .arg(transfer->offset / (1024 * 1024))
                .arg(transfer->size / (1024 * 1024)));
    }
    Q_EMIT syncProgress(progress, QStringLiteral("Uploading %1").arg(transfer->file.relativePath));
    auto *reply = m_network.put(request, chunk);
    connect(reply, &QNetworkReply::finished, this, [this, reply, transfer, end] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && status != 200 && status != 201) {
            Q_EMIT errorOccurred(networkError(reply, QStringLiteral("Could not upload large file: %1")
                                             .arg(transfer->file.relativePath)));
            m_activeUploads.remove(transfer->file.relativePath);
            uploadNextLocalFile();
            return;
        }
        const QByteArray response = reply->readAll();
        if (status == 200 || status == 201) {
            finishUpload(transfer, QJsonDocument::fromJson(response).object());
            return;
        }
        transfer->offset = end + 1;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            const QJsonArray ranges = document.object().value(QStringLiteral("nextExpectedRanges"))
                                           .toArray();
            if (!ranges.isEmpty()) {
                const QString range = ranges.first().toString();
                transfer->offset = range.section(QLatin1Char('-'), 0, 0).toLongLong();
            }
        }
        uploadNextChunk(transfer);
    });
}

void GraphClient::finishUpload(const std::shared_ptr<UploadTransfer> &transfer,
                               const QJsonObject &uploaded)
{
    // Update the baseline before releasing the active slot. The following
    // delta therefore recognizes Graph's echo as already handled.
    const QString signature = localFileSignature(transfer->file.localPath);
    if (!signature.isEmpty()) {
        m_localSignatures.insert(transfer->file.relativePath, signature);
        const QFileInfo fileInfo(transfer->file.localPath);
        m_localMetadata.insert(transfer->file.relativePath,
                               {fileInfo.size(), fileInfo.lastModified()});
    }
    const QString remoteId = uploaded.value(QStringLiteral("id")).toString();
    if (!remoteId.isEmpty()) {
        m_remoteItemIds.insert(transfer->file.relativePath, remoteId);
        m_remotePathsById.insert(remoteId, transfer->file.relativePath);
        m_remoteEtags.insert(remoteId, uploaded.value(QStringLiteral("eTag")).toString());
        m_suppressedRemoteItemIds.insert(remoteId);
    }
    m_suppressedRemotePaths.insert(transfer->file.relativePath);
    Q_EMIT syncProgress(100, QStringLiteral("Uploading %1").arg(transfer->file.relativePath));
    logProgress(QStringLiteral("Graph sync: uploaded %1").arg(transfer->file.relativePath));
    m_activeUploads.remove(transfer->file.relativePath);
    uploadNextLocalFile();
}

bool GraphClient::isIncluded(const QString &relativePath) const
{
    for (const QString &excluded : m_excludedFolders) {
        const QString normalized = normalizeFolderFilter(excluded);
        if (!normalized.isEmpty() && (relativePath == normalized
                                      || relativePath.startsWith(normalized + QLatin1Char('/')))) {
            return false;
        }
    }
    if (m_includedFolders.isEmpty()) {
        return true;
    }
    return std::any_of(m_includedFolders.cbegin(), m_includedFolders.cend(),
                       [&relativePath](const QString &included) {
        const QString normalized = normalizeFolderFilter(included);
        return relativePath == normalized
            || relativePath.startsWith(normalized + QLatin1Char('/'));
    });
}

bool GraphClient::shouldTraverse(const QString &relativePath) const
{
    if (!isIncluded(relativePath)) {
        return false;
    }
    if (m_includedFolders.isEmpty()) {
        return true;
    }
    return std::any_of(m_includedFolders.cbegin(), m_includedFolders.cend(),
                       [&relativePath](const QString &included) {
        const QString normalized = normalizeFolderFilter(included);
        // Traverse both when the selected folder is below the current node
        // and when the current node is already an included ancestor. The
        // latter is what makes Documentos/Videos visible when Documentos is
        // selected at the first level.
        return normalized == relativePath
            || normalized.startsWith(relativePath + QLatin1Char('/'))
            || relativePath.startsWith(normalized + QLatin1Char('/'));
    });
}

bool GraphClient::isRemoteFolderPath(const QString &relativePath) const
{
    if (m_remoteFolders.contains(relativePath) || m_remoteFolderIds.contains(relativePath)) {
        return true;
    }
    const QString localPath = safeLocalPath(relativePath);
    return !localPath.isEmpty() && QFileInfo(localPath).isDir();
}

QString GraphClient::safeLocalPath(const QString &relativePath) const
{
    const QString root = QDir::cleanPath(m_syncDirectory);
    const QString candidate = QDir::cleanPath(QDir(root).filePath(relativePath));
    if (candidate == root || !candidate.startsWith(root + QDir::separator())) {
        return {};
    }
    return candidate;
}
