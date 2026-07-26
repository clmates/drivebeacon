// SPDX-License-Identifier: GPL-3.0-only

#include <KAbstractFileItemActionPlugin>
#include <KFileItem>
#include <KFileItemListProperties>
#include <KLocalizedString>
#include <KPluginFactory>

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <utility>

namespace {

struct MountedProfile
{
    QString name;
    QString mountDirectory;
    QStringList placeholders;
    QStringList localSignatures;
};

/** Loads only Graph profiles because the plugin operates on the Graph FUSE view. */
QList<MountedProfile> configuredProfiles()
{
    // Dolphin owns the process-wide QSettings identity, so the default
    // constructor would read Dolphin's settings instead of DriveBeacon's.
    const QString configFile = QDir(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
                                   .filePath(QStringLiteral("clmates/drivebeacon.conf"));
    QSettings settings(configFile, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("profiles"));
    QList<MountedProfile> profiles;
    for (const QString &name : settings.childGroups()) {
        settings.beginGroup(name);
        if (settings.value(QStringLiteral("backend")).toString() != QLatin1String("graph")) {
            settings.endGroup();
            continue;
        }

        const QString mountDirectory = settings.value(QStringLiteral("mountDirectory"),
                                                       settings.value(QStringLiteral("localDirectory")))
                                           .toString();
        if (!mountDirectory.isEmpty()) {
            MountedProfile profile;
            profile.name = name;
            profile.mountDirectory = QDir::cleanPath(QFileInfo(mountDirectory).absoluteFilePath());
            profile.placeholders = settings.value(QStringLiteral("graphPlaceholderPaths"))
                                       .toStringList();
            profile.localSignatures = settings.value(QStringLiteral("graphLocalSignatures"))
                                          .toStringList();
            profiles.append(std::move(profile));
        }
        settings.endGroup();
    }
    return profiles;
}

/** Converts a selected local URL into a safe relative Graph path. */
QString relativePath(const QString &mountDirectory, const QUrl &url)
{
    const QString localPath = QDir::cleanPath(QFileInfo(url.toLocalFile()).absoluteFilePath());
    const QDir mount(QDir::cleanPath(QFileInfo(mountDirectory).absoluteFilePath()));
    const QString relative = QDir::cleanPath(mount.relativeFilePath(localPath));
    if (relative.isEmpty() || relative == QLatin1String(".") || relative == QLatin1String("..")
        || relative.startsWith(QStringLiteral("../"))) {
        return {};
    }
    return relative;
}

/** Starts the CLI asynchronously so Dolphin never waits on network activity. */
bool startDriveBeaconCtl(const QString &command, const QString &profile,
                         const QStringList &paths)
{
    QString executable = QStandardPaths::findExecutable(QStringLiteral("drivebeaconctl"));
#ifdef DRIVEBEACON_CTL_PATH
    if (executable.isEmpty()) {
        executable = QStringLiteral(DRIVEBEACON_CTL_PATH);
    }
#endif
    if (executable.isEmpty()) {
        return false;
    }

    for (const QString &path : paths) {
        if (!QProcess::startDetached(executable, {command, profile, path})) {
            return false;
        }
    }
    return true;
}

/** Describes whether the selected path is currently represented by cache data. */
QString pathState(const MountedProfile &profile, const QString &path)
{
    if (profile.placeholders.contains(path)) {
        return i18n("Remote-only placeholder");
    }
    for (const QString &signature : profile.localSignatures) {
        if (signature.startsWith(path + QLatin1Char('\t'))) {
            return i18n("Cached locally");
        }
    }
    return i18n("Available remotely");
}

} // namespace

/** Adds safe, service-backed actions to a Dolphin selection inside a DriveBeacon mount. */
class DriveBeaconFileItemAction final : public KAbstractFileItemActionPlugin
{
    Q_OBJECT

public:
    explicit DriveBeaconFileItemAction(QObject *parent, const QVariantList &args)
        : KAbstractFileItemActionPlugin(parent)
    {
        Q_UNUSED(args)
    }

    QList<QAction *> actions(const KFileItemListProperties &fileItemInfos,
                             QWidget *parentWidget) override
    {
        Q_UNUSED(parentWidget)
        const KFileItemList items = fileItemInfos.items();
        if (items.isEmpty()) {
            return {};
        }

        const QList<MountedProfile> profiles = configuredProfiles();
        const MountedProfile *matchedProfile = nullptr;
        QStringList paths;
        for (const MountedProfile &profile : profiles) {
            QStringList candidatePaths;
            bool matches = true;
            for (const KFileItem &item : items) {
                const QString path = relativePath(profile.mountDirectory, item.url());
                if (path.isEmpty()) {
                    matches = false;
                    break;
                }
                candidatePaths.append(path);
            }
            if (matches) {
                if (matchedProfile != nullptr) {
                    // Overlapping mount points make the profile ambiguous; do
                    // not risk sending a valid path to the wrong account.
                    return {};
                }
                matchedProfile = &profile;
                paths = candidatePaths;
            }
        }
        if (matchedProfile == nullptr) {
            return {};
        }

        QList<QAction *> result;
        const QString state = paths.size() == 1
            ? pathState(*matchedProfile, paths.constFirst())
            : i18n("%1 selected item(s)").arg(paths.size());
        // Both actions are available for files and folders. Keep Local on a
        // folder queues all descendants, while a file is fetched by the
        // explicit Keep Local action or when it is actually opened.
        auto *keepLocal = new QAction(QIcon::fromTheme(QStringLiteral("emblem-synchronized")),
                                      i18n("Keep Local"), this);
        keepLocal->setToolTip(i18n("Keep selected content in the local cache using profile %1 (%2)")
                                  .arg(matchedProfile->name, state));
        QObject::connect(keepLocal, &QAction::triggered, this,
                         [this, profile = matchedProfile->name, paths] {
                         if (!startDriveBeaconCtl(QStringLiteral("keep-local"), profile, paths)) {
                             Q_EMIT error(i18n("Could not start drivebeaconctl."));
                         }
                         });
        result.append(keepLocal);

        auto *evict = new QAction(QIcon::fromTheme(QStringLiteral("drive-harddisk")),
                                  i18n("Release Local cache"), this);
        evict->setToolTip(i18n("Release cached content using profile %1 (%2)")
                              .arg(matchedProfile->name, state));
        QObject::connect(evict, &QAction::triggered, this,
                         [this, profile = matchedProfile->name, paths] {
                             if (!startDriveBeaconCtl(QStringLiteral("evict"), profile, paths)) {
                                 Q_EMIT error(i18n("Could not start drivebeaconctl."));
                             }
                         });
        result.append(evict);
        return result;
    }
};

K_PLUGIN_CLASS_WITH_JSON(DriveBeaconFileItemAction, "drivebeaconfileitemaction.json")

#include "drivebeaconfileitemaction.moc"
