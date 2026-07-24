// SPDX-License-Identifier: GPL-3.0-only

#include "graphclient.h"

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
#include <QScopeGuard>
#include <QUrl>
#include <QSet>
#include <systemd/sd-journal.h>

#include <algorithm>

namespace {
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

GraphClient::GraphClient(QObject *parent)
    : QObject(parent)
    , m_network(this)
    , m_uploadTimer(this)
    , m_remoteTimer(this)
{
    m_uploadTimer.setInterval(10000);
    connect(&m_uploadTimer, &QTimer::timeout, this, &GraphClient::scanLocalChanges);
    connect(&m_remoteTimer, &QTimer::timeout, this, [this] {
        // Delta is a persisted pull cursor, not a complete listing. A page can
        // contain both a tombstone and the live item for a rename, and the same
        // item may be repeated; the sets below make each identity idempotent.
        log(QStringLiteral("Graph sync: checking remote delta"));
        const QUrl url = m_deltaLink.isEmpty()
            ? QUrl(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1/root/delta?token=latest")
                       .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_deltaDriveId))))
            : QUrl(m_deltaLink);
        auto *reply = m_network.get(graphRequest(url, m_deltaToken.toUtf8()));
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
            if (reply->error() != QNetworkReply::NoError) {
                Q_EMIT errorOccurred(graphError(reply, QStringLiteral("Could not query remote changes.")));
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
                m_deltaLink = deltaLink;
                Q_EMIT deltaLinkChanged(deltaLink);
            } else if (!nextLink.isEmpty()) {
                m_deltaLink = nextLink;
                return;
            }
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
                        if (item.contains(QStringLiteral("file")) || !QFileInfo(localPath).isDir()) {
                            QFile::remove(localPath);
                        } else {
                            QDir().rmdir(localPath);
                        }
                        m_localSignatures.remove(oldPath);
                        m_pendingRemoteDeletePaths.remove(oldPath);
                        QQueue<GraphLocalFile> remainingUploads;
                        while (!m_pendingUploads.isEmpty()) {
                            const GraphLocalFile pending = m_pendingUploads.dequeue();
                            if (pending.relativePath != oldPath) {
                                remainingUploads.enqueue(pending);
                            }
                        }
                        m_pendingUploads = std::move(remainingUploads);
                    }
                    m_remotePathsById.remove(itemId);
                    m_remoteItemIds.remove(oldPath);
                    handledDeltaIds.insert(itemId);
                    continue;
                }
                if (!item.contains(QStringLiteral("file"))) {
                    // Delta commonly includes a parent folder when a child
                    // changes; that is metadata context, not a folder change.
                    continue;
                }
                const QString parentId = item.value(QStringLiteral("parentReference")).toObject()
                                             .value(QStringLiteral("id")).toString();
                const QString parentPath = m_remotePathsById.value(parentId);
                const QString name = item.value(QStringLiteral("name")).toString();
                if (name.isEmpty() || (parentId.isEmpty() && oldPath.isEmpty())) {
                    structureChanged = true;
                    continue;
                }
                const QString relativePath = oldPath.isEmpty()
                    ? (parentPath.isEmpty() ? name : parentPath + QLatin1Char('/') + name)
                    : oldPath;
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
                    if (QFileInfo(oldLocalPath).exists() && !QFileInfo(newLocalPath).exists()) {
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
                if (isIncluded(relativePath)) {
                    m_pendingFiles.enqueue({itemId, relativePath});
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
            }
        });
    });
}

void GraphClient::log(const QString &message)
{
    graphLog(message);
    Q_EMIT logMessage(message);
}

void GraphClient::startRemoteMonitoring(const QString &driveId, const QString &accessToken,
                                        int intervalSeconds, const QString &deltaLink)
{
    // This path is also used after restoring persisted state, without calling
    // synchronize(); keep all Graph operations supplied with the active drive
    // and bearer token in that case too.
    m_syncDriveId = driveId;
    m_syncToken = accessToken;
    m_deltaDriveId = driveId;
    m_deltaToken = accessToken;
    m_deltaLink = deltaLink;
    m_remoteTimer.setInterval(qBound(10, intervalSeconds, 3600) * 1000);
    m_remoteTimer.start();
}

void GraphClient::initializeLocalMonitoring(const QStringList &signatures,
                                            const QStringList &remotePaths,
                                            const QString &localDirectory)
{
    m_syncDirectory = QDir::cleanPath(QFileInfo(localDirectory).absoluteFilePath());
    m_localSignatures.clear();
    for (const QString &entry : signatures) {
        const int separator = entry.indexOf(QLatin1Char('\t'));
        if (separator > 0) {
            // Versions of the baseline writer briefly appended an empty eTag
            // field here. Accept that historical form, but keep local hashes
            // strictly two-field records from now on.
            const QString storedHash = entry.sliced(separator + 1);
            const int legacyEtags = storedHash.indexOf(QLatin1Char('\t'));
            m_localSignatures.insert(entry.left(separator), legacyEtags < 0
                                         ? storedHash
                                         : storedHash.left(legacyEtags));
        }
    }
    m_remoteItemIds.clear();
    m_remotePathsById.clear();
    m_remoteEtags.clear();
    for (const QString &entry : remotePaths) {
        const int separator = entry.indexOf(QLatin1Char('\t'));
        const int etagSeparator = entry.indexOf(QLatin1Char('\t'), separator + 1);
        if (separator > 0) {
            const QString itemId = entry.left(separator);
            const QString path = etagSeparator < 0
                ? entry.sliced(separator + 1)
                : entry.sliced(separator + 1, etagSeparator - separator - 1);
            const QString etag = etagSeparator < 0 ? QString() : entry.sliced(etagSeparator + 1);
            m_remotePathsById.insert(itemId, path);
            m_remoteEtags.insert(itemId, etag);
            if (!path.isEmpty() && path != QStringLiteral("/")) {
                m_remoteItemIds.insert(path, itemId);
            }
        }
    }
    m_uploadTimer.start();
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
        result.append(it.key() + QLatin1Char('\t') + it.value()
                      + QLatin1Char('\t') + m_remoteEtags.value(it.key()));
    }
    return result;
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
            Q_EMIT errorOccurred(graphError(reply, QStringLiteral("Could not read drive quota.")));
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
            Q_EMIT errorOccurred(graphError(reply, QStringLiteral("Could not discover the current drive.")));
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
    request.setUrl(QUrl(url.toString() + QStringLiteral("?$select=id,name,folder,deleted,eTag,parentReference")));
    auto *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT errorOccurred(graphError(reply, QStringLiteral("Could not list remote folders.")));
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

void GraphClient::processNextFolder()
{
    if (m_pendingFolders.isEmpty()) {
        m_totalFiles = m_pendingFiles.size();
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
    request.setUrl(QUrl(url.toString() + QStringLiteral("?$select=id,name,folder,file,deleted,eTag,parentReference")));
    const QString displayPath = folder.relativePath.isEmpty()
        ? QStringLiteral("/") : folder.relativePath;
    log(QStringLiteral("Graph sync: listing %1").arg(displayPath));
    Q_EMIT syncProgress(0, QStringLiteral("Listing %1").arg(displayPath));
    auto *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, folder, displayPath] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT errorOccurred(graphError(reply, QStringLiteral("Could not read a remote folder.")));
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
                if (!localPath.isEmpty() && !QDir().mkpath(localPath)) {
                    Q_EMIT errorOccurred(QStringLiteral("Could not create local folder: %1").arg(relativePath));
                    return;
                }
                if (shouldTraverse(relativePath)) {
                    m_pendingFolders.enqueue({item.value(QStringLiteral("id")).toString(), relativePath});
                }
            } else if (item.contains(QStringLiteral("file"))) {
                ++filesFound;
                m_remotePathsById.insert(item.value(QStringLiteral("id")).toString(), relativePath);
                m_remoteEtags.insert(item.value(QStringLiteral("id")).toString(),
                                     item.value(QStringLiteral("eTag")).toString());
                m_remoteItemIds.insert(relativePath, item.value(QStringLiteral("id")).toString());
                m_pendingFiles.enqueue({item.value(QStringLiteral("id")).toString(), relativePath});
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
                    Q_EMIT errorOccurred(graphError(
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
                        m_remoteFolderIds.insert(relativePath, item.value(QStringLiteral("id")).toString());
                        if (isIncluded(relativePath)) {
                            const QString localPath = safeLocalPath(relativePath);
                            if (!localPath.isEmpty()) {
                                QDir().mkpath(localPath);
                            }
                            if (shouldTraverse(relativePath)) {
                                m_pendingFolders.enqueue({item.value(QStringLiteral("id")).toString(), relativePath});
                            }
                        }
                    } else if (item.contains(QStringLiteral("file")) && isIncluded(relativePath)) {
                        m_remoteItemIds.insert(relativePath, item.value(QStringLiteral("id")).toString());
                        m_pendingFiles.enqueue({item.value(QStringLiteral("id")).toString(), relativePath});
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
    if (m_pendingFiles.isEmpty()) {
        Q_EMIT syncProgress(100, {});
        Q_EMIT syncFinished();
        Q_EMIT localStateChanged(localSignatures(), remotePaths());
        if (!m_uploadTimer.isActive()) {
            scanLocalChanges();
            m_uploadTimer.start();
        }
        return;
    }
    const GraphSyncFile file = m_pendingFiles.dequeue();
    log(QStringLiteral("Graph sync: downloading %1 (%2/%3)")
                 .arg(file.relativePath).arg(m_downloadedFiles + 1).arg(m_totalFiles));
    const QString localPath = safeLocalPath(file.relativePath);
    if (localPath.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Unsafe remote path rejected: %1").arg(file.relativePath));
        return;
    }
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
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, localPath] {
        processDownloadedReply(reply, file, localPath);
    });
}

void GraphClient::processDownloadedReply(QNetworkReply *reply, const GraphSyncFile &file,
                                         const QString &localPath)
{
    const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) {
        const QUrl location = reply->url().resolved(
            reply->header(QNetworkRequest::LocationHeader).toUrl());
        if (!location.isValid() || location.scheme() != QLatin1String("https")) {
            Q_EMIT errorOccurred(QStringLiteral("Graph returned an invalid file download URL."));
            return;
        }
        log(QStringLiteral("Graph sync: following temporary download URL for %1")
                     .arg(file.relativePath));
        QNetworkRequest downloadRequest(location);
        downloadRequest.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        auto *downloadReply = m_network.get(downloadRequest);
        connect(downloadReply, &QNetworkReply::finished, this,
                [this, downloadReply, file, localPath] {
            processDownloadedReply(downloadReply, file, localPath);
        });
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        Q_EMIT errorOccurred(graphError(reply, QStringLiteral("Could not download a remote file.")));
        return;
    }
        QFile output(localPath);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || output.write(reply->readAll()) < 0) {
            Q_EMIT errorOccurred(QStringLiteral("Could not write local file: %1").arg(file.relativePath));
            return;
        }
        output.close();
        const QString signature = localFileSignature(localPath);
        if (!signature.isEmpty()) {
            m_localSignatures.insert(file.relativePath, signature);
        }
        ++m_downloadedFiles;
        const int percent = m_totalFiles > 0
            ? (m_downloadedFiles * 100) / m_totalFiles
            : 100;
        Q_EMIT syncProgress(percent, file.relativePath);
        processNextFile();
}

void GraphClient::scanLocalChanges()
{
    if (m_uploadInProgress || m_renameInProgress || m_syncDirectory.isEmpty()) {
        return;
    }
    QDirIterator iterator(m_syncDirectory, QDir::Files, QDirIterator::Subdirectories);
    QSet<QString> currentPaths;
    QHash<QString, QString> currentSignatures;
    while (iterator.hasNext()) {
        const QString localPath = iterator.next();
        const QString relativePath = QDir(m_syncDirectory).relativeFilePath(localPath);
        if (!isIncluded(relativePath)) {
            continue;
        }
        currentPaths.insert(relativePath);
        const QString signature = localFileSignature(localPath);
        if (signature.isEmpty()) {
            continue;
        }
        currentSignatures.insert(relativePath, signature);
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
            Q_EMIT errorOccurred(graphError(reply, QStringLiteral("Could not rename remote file: %1")
                                             .arg(rename.oldPath)));
        } else {
            log(QStringLiteral("Graph sync: renamed remote %1 → %2")
                    .arg(rename.oldPath, rename.newPath));
            const QString signature = m_localSignatures.take(rename.oldPath);
            m_localSignatures.insert(rename.newPath, signature);
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
    connect(reply, &QNetworkReply::finished, this, [this, reply, file] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
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
                }
                Q_EMIT localStateChanged(localSignatures(), remotePaths());
                m_deleteInProgress = false;
                deleteNextRemoteFile();
                return;
            }
            m_pendingRemoteDeletePaths.remove(file.relativePath);
            Q_EMIT errorOccurred(graphError(reply, QStringLiteral("Could not delete remote file: %1")
                                             .arg(file.relativePath)));
        } else {
            log(QStringLiteral("Graph sync: deleted remote %1").arg(file.relativePath));
            m_localSignatures.remove(file.relativePath);
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
    if (m_pendingUploads.isEmpty()) {
        const bool completedUpload = m_uploadInProgress;
        m_uploadInProgress = false;
        if (completedUpload) {
            Q_EMIT syncProgress(100, {});
            Q_EMIT syncFinished();
            Q_EMIT localStateChanged(localSignatures(), remotePaths());
        }
        return;
    }
    m_uploadInProgress = true;
    const GraphLocalFile file = m_pendingUploads.dequeue();
    m_pendingUploadPaths.remove(file.relativePath);
    const QFileInfo info(file.localPath);
    if (!info.exists() || info.size() > 250 * 1024 * 1024) {
        Q_EMIT errorOccurred(QStringLiteral("Local file cannot be uploaded directly (missing or larger than 250 MiB): %1")
                                 .arg(file.relativePath));
        uploadNextLocalFile();
        return;
    }
    QFile input(file.localPath);
    if (!input.open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(QStringLiteral("Could not read local file for upload: %1")
                                 .arg(file.relativePath));
        uploadNextLocalFile();
        return;
    }
    const QByteArray contents = input.readAll();
    const QString encodedPath = QString::fromUtf8(QUrl::toPercentEncoding(file.relativePath, "/"));
    const QUrl url(QStringLiteral("https://graph.microsoft.com/v1.0/drives/%1/root:/%2:/content")
                       .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_syncDriveId)), encodedPath));
    QNetworkRequest request = graphRequest(url, m_syncToken.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
        log(QStringLiteral("Graph sync: uploading %1").arg(file.relativePath));
    Q_EMIT syncProgress(0, QStringLiteral("Uploading %1").arg(file.relativePath));
    QNetworkReply *reply = m_network.put(request, contents);
    connect(reply, &QNetworkReply::finished, this, [this, reply, file] {
        const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
        if (reply->error() != QNetworkReply::NoError) {
            m_localSignatures.remove(file.relativePath);
            Q_EMIT errorOccurred(graphError(reply, QStringLiteral("Could not upload local file: %1")
                                             .arg(file.relativePath)));
        } else {
            const QString signature = localFileSignature(file.localPath);
            if (!signature.isEmpty()) {
                m_localSignatures.insert(file.relativePath, signature);
            }
            const QJsonObject uploaded = QJsonDocument::fromJson(reply->readAll()).object();
            const QString remoteId = uploaded.value(QStringLiteral("id")).toString();
            if (!remoteId.isEmpty()) {
                m_remoteItemIds.insert(file.relativePath, remoteId);
                m_remotePathsById.insert(remoteId, file.relativePath);
                m_remoteEtags.insert(remoteId, uploaded.value(QStringLiteral("eTag")).toString());
                m_suppressedRemoteItemIds.insert(remoteId);
            }
            m_suppressedRemotePaths.insert(file.relativePath);
            log(QStringLiteral("Graph sync: uploaded %1").arg(file.relativePath));
        }
        uploadNextLocalFile();
    });
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
    if (!isIncluded(relativePath) && m_includedFolders.isEmpty()) {
        return false;
    }
    if (m_includedFolders.isEmpty()) {
        return true;
    }
    return std::any_of(m_includedFolders.cbegin(), m_includedFolders.cend(),
                       [&relativePath](const QString &included) {
        const QString normalized = normalizeFolderFilter(included);
        return normalized.startsWith(relativePath + QLatin1Char('/'))
            || normalized == relativePath;
    });
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
