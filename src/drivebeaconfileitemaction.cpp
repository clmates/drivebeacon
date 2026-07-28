// SPDX-License-Identifier: GPL-3.0-only

#include <KAbstractFileItemActionPlugin>
#include <KFileItem>
#include <KFileItemListProperties>
#include <KLocalizedString>
#include <KPluginFactory>

#include "drivebeaconmountstate.h"

#include <QAction>
#include <QIcon>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#include <utility>

namespace {

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
QString pathState(const DriveBeaconMountState &profile, const QString &path)
{
    if (profile.placeholderPaths.contains(path)) {
        return i18n("Remote-only placeholder");
    }
    const QString policy = driveBeaconPathPolicy(profile, path);
    if (policy == QLatin1String("keep-local")) {
        return i18n("Keep Local policy");
    }
    if (policy == QLatin1String("remote-only")) {
        return i18n("Remote-only policy");
    }
    if (policy == QLatin1String("on-demand")) {
        return i18n("On-demand policy");
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

        const QList<DriveBeaconMountState> profiles = driveBeaconMountStates();
        const DriveBeaconMountState *matchedProfile = nullptr;
        QStringList paths;
        for (const DriveBeaconMountState &profile : profiles) {
            QStringList candidatePaths;
            bool matches = true;
            for (const KFileItem &item : items) {
                const QString path = driveBeaconRelativePath(profile.mountDirectory, item.url());
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
    auto *keepLocal = new QAction(QIcon::fromTheme(QStringLiteral("emblem-mounted")),
                                      i18n("DriveBeacon – Keep Local"), this);
        keepLocal->setToolTip(i18n("Keep selected content in the local cache using profile %1 (%2)")
                                  .arg(matchedProfile->profileName, state));
        QObject::connect(keepLocal, &QAction::triggered, this,
                         [this, profile = matchedProfile->profileName, paths] {
                         if (!startDriveBeaconCtl(QStringLiteral("keep-local"), profile, paths)) {
                             Q_EMIT error(i18n("Could not start drivebeaconctl."));
                         }
                         });
        result.append(keepLocal);

        auto *evict = new QAction(QIcon::fromTheme(QStringLiteral("drive-harddisk")),
                                  i18n("DriveBeacon – Release Local cache"), this);
        evict->setToolTip(i18n("Release cached content using profile %1 (%2)")
                              .arg(matchedProfile->profileName, state));
        QObject::connect(evict, &QAction::triggered, this,
                         [this, profile = matchedProfile->profileName, paths] {
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
