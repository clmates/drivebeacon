// SPDX-License-Identifier: GPL-3.0-only

#include <KOverlayIconPlugin>

#include "drivebeaconmountstate.h"

#include <QUrl>

namespace {

} // namespace

/** Adds non-blocking cloud/cache overlays to files in a DriveBeacon mount. */
class DriveBeaconOverlayIcon final : public KOverlayIconPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.kde.overlayicon.drivebeacon")

public:
    QStringList getOverlays(const QUrl &url) override
    {
        for (const DriveBeaconMountState &profile : driveBeaconMountStates()) {
            const QString path = driveBeaconRelativePath(profile.mountDirectory, url);
            if (path.isEmpty()) {
                continue;
            }
            if (profile.placeholderPaths.contains(path)) {
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
