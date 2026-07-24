# DriveBeacon

DriveBeacon is a system tray indicator for KDE Plasma that observes and controls
the abraunegg `onedrive` user service. The first MVP provides a tray icon, service
start/stop/restart actions, recent upload, download, move, and deletion activity
from the systemd journal, and navigation to affected local folders.

The application supports the existing journal backend and an incremental native
Microsoft Graph backend. Graph authenticates, restores sessions from the
desktop wallet, reports quota, and synchronizes selected files in both
directions for profiles that keep local copies.

The tray model includes remote storage capacity fields (used, available, and
total). They remain unavailable until a Graph profile has authenticated and
refreshed its drive quota.

## Isolated test profiles

Profiles are stored independently under the DriveBeacon settings namespace. A
profile can be selected or created from the command line without changing the
abraunegg profile:

```bash
build/bin/DriveBeacon \
  --profile graph-test \
  --backend graph \
  --local-directory "$HOME/OneDrive-Graph-Test"
```

The profile name is also included in the tray identifier, so a Graph test
instance and the normal abraunegg instance can run at the same time. A Graph
profile never starts, stops, restarts, or follows `onedrive.service`.

On the first run after upgrading from the 0.1.x layout, DriveBeacon reads the
effective `sync_dir` from `onedrive --display-config` and opens a confirmation
dialog. The proposed profile name is `abraunegg`; accepting it creates the new
profile with the detected directory. Cancelling continues without conversion
and allows the dialog to be shown again on a later run.

Use **Configure profiles…** from the tray menu to create or edit profiles. For
a Graph profile, enter the public application client ID, save it, select **Use
profile**, and reopen the configuration dialog after the restart. The
**Connect** button starts Microsoft's interactive browser OAuth flow. After
authorization, DriveBeacon captures the localhost redirect automatically when
the application registration includes the callback. The manual response URL
field remains available as a fallback. DriveBeacon then discovers the default
drive, stores its ID and quota, lists the folders directly below the drive root,
and synchronizes files with the local directory. The profile dialog presents
those folders in a tree with independent **Sync** and **Exclude** checkboxes;
selecting one automatically clears the other. The client ID must belong to a
public desktop application. Local files up to 250 MiB are uploaded directly;
larger-file upload sessions and conflict resolution remain future work.
Remote changes are checked with a persisted Microsoft Graph delta cursor and
local SHA-256 baseline. A profile with valid sync state can restart without
replaying the initial download. The profile dialog controls the remote check
interval from 10 seconds to 1 hour, with 30 seconds as the default.

Graph refresh tokens are stored in the encrypted KDE Wallet; access tokens are
never written to the configuration file. On a later launch the wallet token is
renewed automatically. If it is unavailable or revoked, connect the profile
again from the configuration dialog.

The current synchronization slice supports `Keep local copy`. `Remote only`
and `Download on demand` are represented in the profile model and prevent local
downloads until their filesystem-provider implementation is added. Delta state,
conflict resolution, placeholders, and SharePoint libraries remain subsequent
roadmap steps.

DriveBeacon is an independent open-source project. It is not affiliated with or
endorsed by Microsoft Corporation or KDE e.V. OneDrive is a trademark of
Microsoft Corporation. KDE and Plasma are trademarks of KDE e.V.

## Requirements

- Qt 6 with QML, Quick, DBus, Widgets, and Qt Test
- KDE Frameworks 6: CoreAddons, I18n, StatusNotifierItem, Wallet, and XmlGui
- CMake, Extra CMake Modules, and Ninja
- `onedrive.service` running as a systemd user service
- `journalctl` access for the current user

## Build and run

```bash
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target run
```

Closing the window leaves the tray application running. Use **Quit** in its tray
menu to exit completely. The `run` target exposes the build-tree desktop metadata
through `XDG_DATA_DIRS`, avoiding portal registration warnings without installing
the application.

## License

DriveBeacon is licensed under the GNU General Public License version 3.0 only.
See [LICENSE](LICENSE) for the complete terms.
