// SPDX-License-Identifier: GPL-3.0-only

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <QMap>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#define FUSE_USE_VERSION 35
#include <fuse3/fuse.h>

#include <cerrno>
#include <ctime>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr auto serviceName = "io.github.clmates.DriveBeacon";
constexpr auto objectPath = "/io/github/clmates/DriveBeacon";
constexpr auto interfaceName = "io.github.clmates.DriveBeacon1";

struct FileSystemContext {
    FileSystemContext(const QString &profileValue, const QString &backingDirectoryValue)
        : profile(profileValue)
        , backingDirectory(backingDirectoryValue)
        , service(QString::fromLatin1(serviceName),
                  QString::fromLatin1(objectPath),
                  QString::fromLatin1(interfaceName),
                  QDBusConnection::sessionBus())
    {
    }

    QString profile;
    QString backingDirectory;
    QDBusInterface service;
    QVariantList entrySnapshot;
    QHash<QString, QVariantMap> entriesByPath;
    QHash<QString, QList<QPair<QString, bool>>> childrenByPath;
    QMutex snapshotMutex;
    qint64 entrySnapshotTimestampMs = 0;
    bool entrySnapshotValid = false;
};

/** Defers remote materialization until a caller actually requests bytes. */
struct OpenFile {
    QString relativePath;
    qint64 expectedSize = -1;
    std::unique_ptr<QFile> file;
    bool writable = false;
    bool dirty = false;
};

FileSystemContext *context()
{
    return static_cast<FileSystemContext *>(fuse_get_context()->private_data);
}

/** Invalidates metadata after a materialization so later lookups refresh it. */
void invalidateEntrySnapshot()
{
    auto *fs = context();
    QMutexLocker locker(&fs->snapshotMutex);
    fs->entrySnapshotValid = false;
}

/** Reads snapshot validity under the same lock used by snapshot replacement. */
bool hasEntrySnapshot()
{
    auto *fs = context();
    QMutexLocker locker(&fs->snapshotMutex);
    return fs->entrySnapshotValid;
}

/** Creates a thread-local D-Bus proxy because FUSE callbacks are multithreaded. */
QDBusInterface serviceInterface()
{
    return QDBusInterface(QString::fromLatin1(serviceName),
                          QString::fromLatin1(objectPath),
                          QString::fromLatin1(interfaceName),
                          QDBusConnection::sessionBus());
}

/** Normalizes a FUSE path before using it as a service-relative path. */
QString relativePath(const char *path)
{
    QString value = QString::fromUtf8(path).trimmed();
    while (value.startsWith(QLatin1Char('/'))) {
        value.remove(0, 1);
    }
    return QDir::cleanPath(value);
}

/** Resolves the service cache path; GraphClient performs the final confinement check. */
QString localPath(const QString &relative)
{
    return QDir(context()->backingDirectory).filePath(relative);
}

/** Converts one D-Bus a{sv} record into a map usable by the FUSE tree. */
QVariantMap entryMap(const QVariant &value);

/** Reads the last enumerated remote tree from the headless service. */
QVariantList remoteEntries()
{
    auto *fs = context();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Dolphin commonly asks for the same directory metadata repeatedly while
    // opening a view. Keep one short-lived snapshot so those callbacks do not
    // serialize a D-Bus round trip for every file.
    {
        QMutexLocker locker(&fs->snapshotMutex);
        if (fs->entrySnapshotValid && now - fs->entrySnapshotTimestampMs < 2000) {
            return fs->entrySnapshot;
        }
    }

    QDBusMessage request = QDBusMessage::createMethodCall(
        QString::fromLatin1(serviceName), QString::fromLatin1(objectPath),
        QString::fromLatin1(interfaceName), QStringLiteral("remoteEntries"));
    request << fs->profile;
    const QDBusMessage response = QDBusConnection::sessionBus().call(
        request, QDBus::Block, 3000);
    const QDBusReply<QVariantList> reply(response);
    if (!reply.isValid()) {
        qWarning().noquote() << "DriveBeacon FUSE: remoteEntries failed:"
                             << reply.error().message();
        return {};
    }
    const QVariantList entries = reply.value();
    QHash<QString, QVariantMap> entriesByPath;
    QHash<QString, QMap<QString, bool>> childrenByPath;
    for (const QVariant &value : entries) {
        const QVariantMap entry = entryMap(value);
        const QString path = entry.value(QStringLiteral("path")).toString();
        if (path.isEmpty() || path == QLatin1String("/")) {
            continue;
        }
        entriesByPath.insert(path, entry);
        QString parent;
        QString remainder = path;
        while (true) {
            const int slash = remainder.lastIndexOf(QLatin1Char('/'));
            if (slash < 0) {
                parent.clear();
            } else {
                parent = remainder.left(slash);
            }
            const QString child = remainder.sliced(slash + 1);
            if (!child.isEmpty()) {
                const bool childIsFolder = remainder != path
                    || entry.value(QStringLiteral("folder")).toBool();
                childrenByPath[parent].insert(child, childIsFolder);
            }
            if (slash < 0) {
                break;
            }
            remainder.truncate(slash);
        }
    }
    QHash<QString, QList<QPair<QString, bool>>> indexedChildren;
    for (auto parent = childrenByPath.cbegin(); parent != childrenByPath.cend(); ++parent) {
        QList<QPair<QString, bool>> children;
        for (auto child = parent.value().cbegin(); child != parent.value().cend(); ++child) {
            children.append({child.key(), child.value()});
        }
        indexedChildren.insert(parent.key(), children);
    }
    {
        QMutexLocker locker(&fs->snapshotMutex);
        fs->entrySnapshot = entries;
        fs->entriesByPath = std::move(entriesByPath);
        fs->childrenByPath = std::move(indexedChildren);
        fs->entrySnapshotTimestampMs = now;
        fs->entrySnapshotValid = true;
        return fs->entrySnapshot;
    }
}

/** Converts one D-Bus a{sv} record into a map usable by the FUSE tree. */
QVariantMap entryMap(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QVariantMap>()) {
        return value.toMap();
    }
    if (value.metaType() != QMetaType::fromType<QDBusArgument>()) {
        return {};
    }
    const QDBusArgument argument = value.value<QDBusArgument>();
    QVariantMap result;
    argument.beginMap();
    while (!argument.atEnd()) {
        QString key;
        QVariant item;
        argument.beginMapEntry();
        argument >> key >> item;
        argument.endMapEntry();
        result.insert(key, item);
    }
    argument.endMap();
    return result;
}

/** Looks up metadata in the indexed snapshot without scanning every object. */
QVariantMap findIndexedEntry(const QString &relative)
{
    auto *fs = context();
    QMutexLocker locker(&fs->snapshotMutex);
    const auto it = fs->entriesByPath.constFind(relative);
    if (it != fs->entriesByPath.cend()) {
        return it.value();
    }
    const auto children = fs->childrenByPath.constFind(relative);
    if (children != fs->childrenByPath.cend()) {
        return {{QStringLiteral("path"), relative},
                {QStringLiteral("folder"), true},
                {QStringLiteral("size"), 0LL}};
    }
    return {};
}

/** Fetches the service snapshot and finds one remote entry. */
QVariantMap findEntry(const QString &relative)
{
    remoteEntries();
    return findIndexedEntry(relative);
}

/** Returns immediate children so FUSE can expose a stable remote directory. */
QList<QPair<QString, bool>> childrenOf(const QVariantList &entries, const QString &parent)
{
    Q_UNUSED(entries)
    remoteEntries();
    QList<QPair<QString, bool>> result;
    {
        auto *fs = context();
        QMutexLocker locker(&fs->snapshotMutex);
        const auto indexed = fs->childrenByPath.constFind(parent);
        if (indexed != fs->childrenByPath.cend()) {
            result = indexed.value();
        }
    }
    // Include files created in the private cache before Graph has assigned
    // them an item ID. This keeps a newly written FUSE file visible during
    // the short interval before the next local scan/upload.
    const QFileInfoList localChildren = QDir(localPath(parent)).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &child : localChildren) {
        const QString name = child.fileName();
        // The cache can contain desktop trash and editor lock files. They are
        // local implementation details, not DriveBeacon remote entries; if
        // exposed here, LibreOffice immediately asks Graph to materialize them.
        if (name == QStringLiteral(".Trash")
            || name.startsWith(QStringLiteral(".Trash-"), Qt::CaseInsensitive)
            || name.endsWith(QStringLiteral(".part"), Qt::CaseInsensitive)
            || name.endsWith(QStringLiteral(".tmp"), Qt::CaseInsensitive)
            || name.startsWith(QStringLiteral(".~"))) {
            continue;
        }
        bool alreadyPresent = false;
        for (const auto &remoteChild : result) {
            if (remoteChild.first == child.fileName()) {
                alreadyPresent = true;
                break;
            }
        }
        if (alreadyPresent) {
            continue;
        }
        result.append({name, child.isDir()});
    }
    qInfo().noquote() << "DriveBeacon FUSE: children" << parent << result.size();
    return result;
}

/** Supplies stat data from the same snapshot used to fill a directory. */
bool statFromEntry(const QString &relative, const QVariantMap &entry, struct stat *st)
{
    if (entry.isEmpty()) {
        return false;
    }
    std::memset(st, 0, sizeof(*st));
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_atime = st->st_mtime = st->st_ctime = std::time(nullptr);
    if (entry.value(QStringLiteral("folder")).toBool()) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_size = 4096;
    } else {
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        qint64 size = entry.value(QStringLiteral("size")).toLongLong();
        if (!entry.value(QStringLiteral("sizeKnown")).toBool()
            && !entry.value(QStringLiteral("placeholder")).toBool()) {
            // A legacy baseline may not contain remote sizes. When the cache
            // already has materialized content, expose that size to clients.
            const QFileInfo cached(localPath(relative));
            if (cached.isFile()) {
                size = cached.size();
            }
        }
        st->st_size = size;
    }
    return true;
}

/** Reports a completed local mutation so the service starts upload/delete detection. */
int notifyLocalChange()
{
    const QDBusMessage reply = serviceInterface().call(
        QStringLiteral("notifyLocalChange"), context()->profile);
    return reply.type() == QDBusMessage::ErrorMessage ? -EIO : 0;
}

/** Reports a local rename explicitly so folders retain their remote identity. */
int notifyLocalRename(const QString &oldPath, const QString &newPath)
{
    const QDBusMessage reply = serviceInterface().call(
        QStringLiteral("renameLocalPath"), context()->profile, oldPath, newPath);
    return reply.type() == QDBusMessage::ErrorMessage ? -EIO : 0;
}

int requestMaterialization(const QString &relative, qint64 expectedSize)
{
    // The service may discover that a legacy zero-byte entry is a folder while
    // handling this request. Do not keep using the pre-request FUSE snapshot.
    invalidateEntrySnapshot();
    const QDBusMessage reply = serviceInterface().call(
        QStringLiteral("materializeFile"), context()->profile, relative);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return -EIO;
    }
    const QString path = localPath(relative);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 30 * 60 * 1000) {
        const QFileInfo info(path);
        const QVariantMap entry = findEntry(relative);
        if (entry.value(QStringLiteral("folder")).toBool()) {
            // Dolphin may have attempted to open a stale regular-file view of
            // an empty remote folder. Return promptly so it can re-stat it as
            // a directory instead of waiting for the materialization timeout.
            return -EISDIR;
        }
        const bool placeholder = entry.value(QStringLiteral("placeholder")).toBool();
        if (info.isFile() && !placeholder
            && (expectedSize < 0 || info.size() == expectedSize)) {
            return 0;
        }
        QThread::msleep(100);
    }
    return -ETIMEDOUT;
}

/** Supplies remote size and directory mode to applications without downloading data. */
int fsGetattr(const char *path, struct stat *st, struct fuse_file_info *)
{
    const QString relative = relativePath(path);
    qInfo().noquote() << "DriveBeacon FUSE: getattr"
                      << (relative.isEmpty() ? QStringLiteral("/") : relative);
    if (relative.isEmpty()) {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    }
    QVariantMap entry = findEntry(relative);
    if (entry.isEmpty()) {
        const QFileInfo local(localPath(relative));
        if (local.isDir() || local.isFile()) {
            entry = {{QStringLiteral("path"), relative},
                     {QStringLiteral("folder"), local.isDir()},
                     {QStringLiteral("size"), local.isDir() ? 0LL : local.size()},
                     {QStringLiteral("sizeKnown"), true}};
        }
    }
    if (entry.isEmpty()) {
        qWarning().noquote() << "DriveBeacon FUSE: getattr missing remote path" << relative;
        return -ENOENT;
    }
    return statFromEntry(relative, entry, st) ? 0 : -ENOENT;
}

/**
 * Reports the backing filesystem capacity to copy tools such as Dolphin/KIO.
 *
 * FUSE otherwise exposes no usable capacity for this virtual tree and some
 * clients interpret that missing result as zero free bytes before creating a
 * destination file. Remote quota remains authoritative for Graph uploads;
 * this callback only describes the local cache space available for writes.
 */
int fsStatfs(const char *, struct statvfs *stat)
{
    auto *fs = context();
    if (!statvfs(fs->backingDirectory.toLocal8Bit().constData(), stat)) {
        return 0;
    }
    return -errno;
}

/** Lists only the immediate children of a remote directory. */
int fsReaddir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t,
              struct fuse_file_info *, enum fuse_readdir_flags)
{
    const QString relative = relativePath(path);
    // Reuse one snapshot for the complete directory response. Dolphin can
    // request hundreds of child attributes while opening a folder; issuing a
    // synchronous D-Bus call for every child made the UI appear frozen.
    const QVariantList entries = remoteEntries();
    const QVariantMap directoryEntry = findIndexedEntry(relative);
    if (!relative.isEmpty() && !directoryEntry.value(QStringLiteral("folder")).toBool()
        && !QFileInfo(localPath(relative)).isDir()) {
        qWarning().noquote() << "DriveBeacon FUSE: readdir requested for non-directory" << relative;
        return -ENOTDIR;
    }
    qInfo().noquote() << "DriveBeacon FUSE: readdir" << (relative.isEmpty() ? QStringLiteral("/") : relative);
    if (filler(buffer, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0)) != 0 ||
        filler(buffer, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0)) != 0) {
        return 0;
    }
    for (const auto &[name, folder] : childrenOf(entries, relative)) {
        Q_UNUSED(folder)
        const QString childPath = relative.isEmpty()
            ? name : relative + QLatin1Char('/') + name;
        struct stat childStat{};
        const bool hasMetadata = statFromEntry(childPath, findIndexedEntry(childPath), &childStat);
        const QByteArray encodedName = name.toUtf8();
        qInfo().noquote() << "DriveBeacon FUSE: readdir entry" << childPath
                          << "metadata" << hasMetadata;
        const auto flags = hasMetadata
            ? static_cast<fuse_fill_dir_flags>(FUSE_FILL_DIR_PLUS)
            : static_cast<fuse_fill_dir_flags>(0);
        if (filler(buffer, encodedName.constData(), hasMetadata ? &childStat : nullptr,
                   0, flags) != 0) {
            break;
        }
    }
    return 0;
}

/** Validates a remote file and creates a lazy, read-only FUSE handle. */
int fsOpen(const char *path, struct fuse_file_info *info)
{
    const QString relative = relativePath(path);
    // Keep the flags in the diagnostic: desktop environments may open a
    // placeholder merely to sniff its MIME type, which is distinct from an
    // application opening it for actual content access.
    qInfo().noquote() << "DriveBeacon FUSE: open" << relative
                      << QStringLiteral("flags=0x") + QString::number(info->flags, 16);
    QVariantMap entry = findEntry(relative);
    if (entry.isEmpty()) {
        // Editors commonly create a private temporary file and reopen it
        // before the service's local scan has assigned a remote item ID.
        // FUSE must treat that cache entry as a valid local file instead of
        // returning ENOENT merely because it is not remote yet.
        const QFileInfo local(localPath(relative));
        if (local.isFile()) {
            entry = {{QStringLiteral("path"), relative},
                     {QStringLiteral("folder"), false},
                     {QStringLiteral("size"), local.size()},
                     {QStringLiteral("sizeKnown"), true}};
        }
    }
    if (entry.isEmpty() || entry.value(QStringLiteral("folder")).toBool()) {
        qWarning().noquote() << "DriveBeacon FUSE: open missing or directory" << relative;
        return -ENOENT;
    }
    const int accessMode = info->flags & O_ACCMODE;
    if (accessMode != O_RDONLY) {
        const QString cachedPath = localPath(relative);
        if (info->flags & O_TRUNC) {
            QFile::remove(cachedPath);
        } else if (!QFileInfo(cachedPath).isFile()
                   || (hasEntrySnapshot()
                       && findEntry(relative).value(QStringLiteral("placeholder")).toBool())) {
            const int result = requestMaterialization(
                relative, entry.value(QStringLiteral("size")).toLongLong());
            if (result != 0) {
                return result;
            }
        }
        auto file = std::make_unique<QFile>(cachedPath);
        if (!file->open(QIODevice::ReadWrite)) {
            return -EACCES;
        }
        auto *openFile = new OpenFile{relative, file->size(), std::move(file), true, false};
        info->fh = reinterpret_cast<quintptr>(openFile);
        return 0;
    }
    const qint64 size = entry.value(QStringLiteral("sizeKnown")).toBool()
        ? entry.value(QStringLiteral("size")).toLongLong() : -1;
    // Dolphin may open a placeholder only to inspect it before constructing a
    // context menu. Delaying the network operation until fsRead prevents that
    // metadata probe from unexpectedly downloading the selected file.
    auto *file = new OpenFile{relative, size, nullptr, false, false};
    info->fh = reinterpret_cast<quintptr>(file);
    return 0;
}

/** Creates a writable cache file for O_CREAT before Graph sees the new item. */
int fsCreate(const char *path, mode_t mode, struct fuse_file_info *info)
{
    Q_UNUSED(mode)
    const QString relative = relativePath(path);
    const QString cachedPath = localPath(relative);
    if (cachedPath.isEmpty() || !QDir().mkpath(QFileInfo(cachedPath).absolutePath())) {
        return -EACCES;
    }
    auto file = std::make_unique<QFile>(cachedPath);
    if (!file->open(QIODevice::ReadWrite | QIODevice::Truncate)) {
        return -EACCES;
    }
    auto *openFile = new OpenFile{relative, 0, std::move(file), true, true};
    info->fh = reinterpret_cast<quintptr>(openFile);
    return 0;
}

/** Reads from the service-backed cache after FUSE has validated the offset. */
int fsRead(const char *, char *buffer, size_t size, off_t offset, struct fuse_file_info *info)
{
    auto *openFile = reinterpret_cast<OpenFile *>(info->fh);
    if (!openFile) {
        return -EIO;
    }
    if (!openFile->file) {
        const int result = requestMaterialization(openFile->relativePath, openFile->expectedSize);
        if (result != 0) {
            qWarning().noquote() << "DriveBeacon FUSE: materialization failed"
                                 << openFile->relativePath << result;
            return result;
        }
        auto file = std::make_unique<QFile>(localPath(openFile->relativePath));
        if (!file->open(QIODevice::ReadOnly)) {
            qWarning().noquote() << "DriveBeacon FUSE: cache open failed"
                                 << localPath(openFile->relativePath) << file->errorString();
            return -EIO;
        }
        qInfo().noquote() << "DriveBeacon FUSE: opened cache" << openFile->relativePath
                          << file->size();
        openFile->file = std::move(file);
    }
    if (!openFile->file->seek(offset)) {
        return -EIO;
    }
    return static_cast<int>(openFile->file->read(buffer, static_cast<qint64>(size)));
}

/** Writes bytes into the private cache; remote upload starts after flush/close. */
int fsWrite(const char *, const char *buffer, size_t size, off_t offset,
            struct fuse_file_info *info)
{
    auto *openFile = reinterpret_cast<OpenFile *>(info->fh);
    if (!openFile || !openFile->writable || !openFile->file) {
        return -EBADF;
    }
    if (!openFile->file->seek(offset)) {
        return -EIO;
    }
    const qint64 written = openFile->file->write(buffer, static_cast<qint64>(size));
    if (written > 0) {
        openFile->dirty = true;
    }
    return written < 0 ? -EIO : static_cast<int>(written);
}

/** Flushes cache bytes and wakes the service local mutation scanner. */
int fsFlush(const char *, struct fuse_file_info *info)
{
    auto *openFile = reinterpret_cast<OpenFile *>(info->fh);
    if (!openFile || !openFile->file) {
        return -EBADF;
    }
    if (!openFile->file->flush()) {
        return -EIO;
    }
    if (openFile->dirty) {
        const int result = notifyLocalChange();
        if (result != 0) {
            return result;
        }
        openFile->dirty = false;
    }
    return 0;
}

/** Provides fsync semantics for applications that explicitly request durability. */
int fsFsync(const char *, int, struct fuse_file_info *info)
{
    return fsFlush(nullptr, info);
}

/** Removes cached content and lets the local scanner decide remote deletion. */
int fsUnlink(const char *path)
{
    const QString relative = relativePath(path);
    const QVariantMap entry = findEntry(relative);
    const QString cachedPath = localPath(relative);
    if (entry.value(QStringLiteral("placeholder")).toBool()
        && !QFileInfo(cachedPath).isFile()) {
        // A zero-byte marker is not user content. Refusing this operation
        // prevents a normal filesystem delete from deleting the remote item.
        return -EROFS;
    }
    if (!QFile::remove(cachedPath)) {
        return -ENOENT;
    }
    return notifyLocalChange();
}

/** Removes an empty local directory without recursively deleting remote data. */
int fsRmdir(const char *path)
{
    const QString relative = relativePath(path);
    const QString directory = localPath(relative);
    if (relative.isEmpty() || directory.isEmpty() || !QDir(directory).isEmpty()) {
        return -ENOTEMPTY;
    }
    if (!QDir().rmdir(directory)) {
        return -ENOENT;
    }
    return notifyLocalChange();
}

/** Renames cache content; GraphClient preserves the remote item ID when possible. */
int fsRename(const char *from, const char *to, unsigned int flags)
{
    if (flags != 0) {
        return -EINVAL;
    }
    const QString oldRelative = relativePath(from);
    const QString newRelative = relativePath(to);
    const QString oldPath = localPath(oldRelative);
    const QString newPath = localPath(newRelative);
    if (oldRelative.isEmpty() || newRelative.isEmpty() || newPath.isEmpty()) {
        return -EINVAL;
    }
    if (!QDir().mkpath(QFileInfo(newPath).absolutePath())
        || !QFile::rename(oldPath, newPath)) {
        return -EIO;
    }
    // Tell Graph the exact old/new paths. A generic scan cannot reliably
    // infer a directory rename because its descendants keep their content.
    return notifyLocalRename(oldRelative, newRelative);
}

/** Creates a local cache directory; the service queues its Graph counterpart. */
int fsMkdir(const char *path, mode_t mode)
{
    Q_UNUSED(mode)
    const QString relative = relativePath(path);
    const QString directory = localPath(relative);
    if (relative.isEmpty() || directory.isEmpty() || !QDir().mkpath(directory)) {
        return -EIO;
    }
    return notifyLocalChange();
}

/** Resizes a cached file and marks it dirty for the normal upload pipeline. */
int fsTruncate(const char *path, off_t size, struct fuse_file_info *info)
{
    OpenFile *openFile = info ? reinterpret_cast<OpenFile *>(info->fh) : nullptr;
    if (openFile && openFile->file) {
        if (!openFile->file->resize(size)) {
            return -EIO;
        }
        openFile->dirty = true;
        return 0;
    }
    const QString relative = relativePath(path);
    const QString cachedPath = localPath(relative);
    QFile file(cachedPath);
    if (!file.open(QIODevice::ReadWrite) || !file.resize(size)) {
        return -EIO;
    }
    file.close();
    return notifyLocalChange();
}

/** Releases the cache handle without deleting or mutating remote content. */
int fsRelease(const char *, struct fuse_file_info *info)
{
    auto *openFile = reinterpret_cast<OpenFile *>(info->fh);
    int result = 0;
    if (openFile && openFile->dirty) {
        result = fsFlush(nullptr, info);
    }
    delete openFile;
    info->fh = 0;
    return result;
}

/** Enables short-lived kernel caching for stable metadata and directory reads. */
void *fsInit(struct fuse_conn_info *, struct fuse_config *config)
{
    config->attr_timeout = 1.0;
    config->entry_timeout = 1.0;
    config->negative_timeout = 1.0;
    // libfuse uses the init return value as the operation private data.
    // Preserve the FileSystemContext supplied to fuse_main().
    return fuse_get_context()->private_data;
}

/** Lets the kernel reuse a completed directory enumeration briefly. */
int fsOpenDir(const char *, struct fuse_file_info *info)
{
    info->cache_readdir = 1;
    return 0;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("profile"), QStringLiteral("Graph profile."), QStringLiteral("name")});
    parser.addOption({QStringLiteral("backing-directory"), QStringLiteral("Service cache directory."), QStringLiteral("path")});
    parser.process(application);
    const QString profile = parser.value(QStringLiteral("profile"));
    const QString backingDirectory = parser.value(QStringLiteral("backing-directory"));
    if (profile.isEmpty() || backingDirectory.isEmpty() || parser.positionalArguments().isEmpty()) {
        parser.showHelp(2);
    }
    FileSystemContext fs(profile, QDir::cleanPath(backingDirectory));
    if (!fs.service.isValid()) {
        return 1;
    }
    struct fuse_operations operations{};
    operations.init = fsInit;
    operations.opendir = fsOpenDir;
    operations.getattr = fsGetattr;
    operations.statfs = fsStatfs;
    operations.readdir = fsReaddir;
    operations.open = fsOpen;
    operations.create = fsCreate;
    operations.read = fsRead;
    operations.write = fsWrite;
    operations.flush = fsFlush;
    operations.fsync = fsFsync;
    operations.unlink = fsUnlink;
    operations.rmdir = fsRmdir;
    operations.rename = fsRename;
    operations.mkdir = fsMkdir;
    operations.truncate = fsTruncate;
    operations.release = fsRelease;
    std::vector<char *> fuseArgs;
    fuseArgs.reserve(static_cast<size_t>(argc) + 1);
    fuseArgs.push_back(argv[0]);
    fuseArgs.push_back(const_cast<char *>("-f"));
    // Use libfuse's multithreaded loop so one materialization wait does not
    // block metadata and I/O requests for unrelated mounted files. D-Bus
    // proxies are created per callback thread by serviceInterface().
    fuseArgs.push_back(argv[argc - 1]);
    // fuse_main() is the public libfuse entry point. Calling the lower-level
    // fuse_main_real() leaves a diagnostic and can create a mount that returns
    // EIO with current libfuse releases.
    return fuse_main(static_cast<int>(fuseArgs.size()), fuseArgs.data(), &operations, &fs);
}
