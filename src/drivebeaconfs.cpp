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

#define FUSE_USE_VERSION 35
#include <fuse3/fuse.h>

#include <cerrno>
#include <ctime>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr auto serviceName = "io.github.clmates.DriveBeacon";
constexpr auto objectPath = "/io/github/clmates/DriveBeacon";
constexpr auto interfaceName = "io.github.clmates.DriveBeacon1";

struct FileSystemContext {
    QString profile;
    QString backingDirectory;
    QDBusInterface service;
    QVariantList entrySnapshot;
    qint64 entrySnapshotTimestampMs = 0;
    bool entrySnapshotValid = false;
};

/** Defers remote materialization until a caller actually requests bytes. */
struct OpenFile {
    QString relativePath;
    qint64 expectedSize = -1;
    std::unique_ptr<QFile> file;
};

FileSystemContext *context()
{
    return static_cast<FileSystemContext *>(fuse_get_context()->private_data);
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

/** Reads the last enumerated remote tree from the headless service. */
QVariantList remoteEntries()
{
    auto *fs = context();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Dolphin commonly asks for the same directory metadata repeatedly while
    // opening a view. Keep one short-lived snapshot so those callbacks do not
    // serialize a D-Bus round trip for every file.
    if (fs->entrySnapshotValid && now - fs->entrySnapshotTimestampMs < 2000) {
        return fs->entrySnapshot;
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
    fs->entrySnapshot = reply.value();
    fs->entrySnapshotTimestampMs = now;
    fs->entrySnapshotValid = true;
    return fs->entrySnapshot;
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

/** Finds one file or folder metadata record in an already fetched snapshot. */
QVariantMap findEntryIn(const QVariantList &entries, const QString &relative)
{
    for (const QVariant &value : entries) {
        const QVariantMap entry = entryMap(value);
        if (entry.value(QStringLiteral("path")).toString() == relative) {
            return entry;
        }
    }
    // Some persisted baselines contain files below a folder but no separate
    // record for that intermediate folder. Synthesize its directory metadata
    // so traversal remains valid after a restart.
    const QString descendantPrefix = relative + QLatin1Char('/');
    for (const QVariant &value : entries) {
        const QVariantMap entry = entryMap(value);
        if (entry.value(QStringLiteral("path")).toString().startsWith(descendantPrefix)) {
            return {{QStringLiteral("path"), relative},
                    {QStringLiteral("folder"), true},
                    {QStringLiteral("size"), 0LL}};
        }
    }
    return {};
}

/** Fetches the service snapshot and finds one remote entry. */
QVariantMap findEntry(const QString &relative)
{
    return findEntryIn(remoteEntries(), relative);
}

/** Returns immediate children so FUSE can expose a stable remote directory. */
QList<QPair<QString, bool>> childrenOf(const QVariantList &entries, const QString &parent)
{
    QMap<QString, bool> children;
    const QString prefix = parent.isEmpty() ? QString() : parent + QLatin1Char('/');
    for (const QVariant &value : entries) {
        const QVariantMap entry = entryMap(value);
        const QString path = entry.value(QStringLiteral("path")).toString();
        if (!path.startsWith(prefix)) {
            continue;
        }
        const QString remainder = path.sliced(prefix.size());
        // Graph also returns a synthetic root item with an empty path; it is
        // represented by the FUSE mount itself and must not become an empty
        // directory entry.
        if (remainder.isEmpty()) {
            continue;
        }
        const int slash = remainder.indexOf(QLatin1Char('/'));
        if (slash < 0) {
            children.insert(remainder, entry.value(QStringLiteral("folder")).toBool());
        } else {
            children.insert(remainder.left(slash), true);
        }
    }
    QList<QPair<QString, bool>> result;
    for (auto it = children.cbegin(); it != children.cend(); ++it) {
        result.append({it.key(), it.value()});
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
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        st->st_size = 4096;
    } else {
        st->st_mode = S_IFREG | 0444;
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

int requestMaterialization(const QString &relative, qint64 expectedSize)
{
    // The service may discover that a legacy zero-byte entry is a folder while
    // handling this request. Do not keep using the pre-request FUSE snapshot.
    context()->entrySnapshotValid = false;
    const QDBusMessage reply = context()->service.call(
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
    const QVariantMap entry = findEntry(relative);
    if (entry.isEmpty()) {
        qWarning().noquote() << "DriveBeacon FUSE: getattr missing remote path" << relative;
        return -ENOENT;
    }
    return statFromEntry(relative, entry, st) ? 0 : -ENOENT;
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
    if (!relative.isEmpty() && !findEntryIn(entries, relative).value(QStringLiteral("folder")).toBool()) {
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
        const bool hasMetadata = statFromEntry(childPath, findEntryIn(entries, childPath), &childStat);
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
    const QVariantMap entry = findEntry(relative);
    if (entry.isEmpty() || entry.value(QStringLiteral("folder")).toBool()) {
        qWarning().noquote() << "DriveBeacon FUSE: open missing or directory" << relative;
        return -ENOENT;
    }
    if ((info->flags & O_ACCMODE) != O_RDONLY) {
        qWarning().noquote() << "DriveBeacon FUSE: write access rejected" << relative;
        return -EROFS;
    }
    const qint64 size = entry.value(QStringLiteral("sizeKnown")).toBool()
        ? entry.value(QStringLiteral("size")).toLongLong() : -1;
    // Dolphin may open a placeholder only to inspect it before constructing a
    // context menu. Delaying the network operation until fsRead prevents that
    // metadata probe from unexpectedly downloading the selected file.
    auto *file = new OpenFile{relative, size, nullptr};
    info->fh = reinterpret_cast<quintptr>(file);
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

/** Releases the cache handle without deleting or mutating remote content. */
int fsRelease(const char *, struct fuse_file_info *info)
{
    delete reinterpret_cast<OpenFile *>(info->fh);
    info->fh = 0;
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
    FileSystemContext fs{profile, QDir::cleanPath(backingDirectory),
                         QDBusInterface(QString::fromLatin1(serviceName),
                                        QString::fromLatin1(objectPath),
                                        QString::fromLatin1(interfaceName),
                                        QDBusConnection::sessionBus()),
                         {}, 0, false};
    if (!fs.service.isValid()) {
        return 1;
    }
    struct fuse_operations operations{};
    operations.getattr = fsGetattr;
    operations.readdir = fsReaddir;
    operations.open = fsOpen;
    operations.read = fsRead;
    operations.release = fsRelease;
    std::vector<char *> fuseArgs;
    fuseArgs.reserve(static_cast<size_t>(argc) + 1);
    fuseArgs.push_back(argv[0]);
    fuseArgs.push_back(const_cast<char *>("-f"));
    // The D-Bus proxy is owned by the main Qt thread. Keep FUSE callbacks in
    // that same thread until the provider has an explicit per-thread proxy.
    fuseArgs.push_back(const_cast<char *>("-s"));
    fuseArgs.push_back(argv[argc - 1]);
    // fuse_main() is the public libfuse entry point. Calling the lower-level
    // fuse_main_real() leaves a diagnostic and can create a mount that returns
    // EIO with current libfuse releases.
    return fuse_main(static_cast<int>(fuseArgs.size()), fuseArgs.data(), &operations, &fs);
}
