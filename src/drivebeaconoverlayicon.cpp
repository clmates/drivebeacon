// SPDX-License-Identifier: GPL-3.0-only

#include <KOverlayIconPlugin>

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

namespace {

struct ProfileState
{
    QString mountDirectory;
    QStringList placeholders;
    QStringList localSignatures;
};

/** Reads the persisted cache state without contacting Graph or the service. */
QList<ProfileState> profileStates()
{
    const QString configFile = QDir(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
                                   .filePath(QStringLiteral("clmates/drivebeacon.conf"));
    QSettings settings(configFile, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("profiles"));
    QList<ProfileState> result;
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
            result.append({QDir::cleanPath(QFileInfo(mountDirectory).absoluteFilePath()),
                           settings.value(QStringLiteral("graphPlaceholderPaths")).toStringList(),
                           settings.value(QStringLiteral("graphLocalSignatures")).toStringList()});
        }
        settings.endGroup();
    }
    return result;
}

/** Maps a mounted local URL to the relative path stored by GraphClient. */
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

} // namespace

/** Adds non-blocking cloud/cache overlays to files in a DriveBeacon mount. */
class DriveBeaconOverlayIcon final : public KOverlayIconPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.kde.overlayicon.drivebeacon")

public:
    QStringList getOverlays(const QUrl &url) override
    {
        for (const ProfileState &profile : profileStates()) {
            const QString path = relativePath(profile.mountDirectory, url);
            if (path.isEmpty()) {
                continue;
            }
            if (profile.placeholders.contains(path)) {
                return {QStringLiteral("cloud-download")};
            }
            for (const QString &signature : profile.localSignatures) {
                if (signature.startsWith(path + QLatin1Char('\t'))) {
                    return {QStringLiteral("emblem-synchronized")};
                }
            }
        }
        return {};
    }
};

#include "drivebeaconoverlayicon.moc"
