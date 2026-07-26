# DriveBeacon

DriveBeacon is a system tray indicator for KDE Plasma that observes and controls
the abraunegg `onedrive` user service. The first MVP provides a tray icon, service
start/stop/restart actions, recent upload, download, move, and deletion activity
from the systemd journal, and navigation to affected local folders.

The application supports the existing journal backend and an incremental native
Microsoft Graph backend. Graph authenticates, restores sessions from the
desktop wallet, reports quota, and synchronizes selected files in both
directions. The headless service keeps content in a private per-profile cache;
the optional read-only FUSE view exposes it through a user-selected mount path.

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
dialog includes a tooltip and a help button explaining that this ID is public,
how it differs from account credentials, and when a user may provide their own
Microsoft Entra app registration. The release build will provide a default
client ID; users who replace it must configure a public desktop application
with the required account types, permissions, and redirect URI. The
**Connect** button starts Microsoft's interactive browser OAuth flow. After
authorization, DriveBeacon captures the localhost redirect automatically when
the application registration includes the callback. The manual response URL
field remains available as a fallback. DriveBeacon then discovers the default
drive, stores its ID and quota, lists the folders directly below the drive root,
and synchronizes files with the service cache. The profile dialog presents
those folders in a tree with independent **Sync** and **Exclude** checkboxes;
selecting one automatically clears the other. The client ID must belong to a
public desktop application. Large files use resumable Graph upload sessions.
Remote changes are checked with a persisted Microsoft Graph delta cursor and
local SHA-256 baseline. A profile with valid sync state can restart without
replaying the initial download. The profile dialog controls the remote check
interval from 10 seconds to 1 hour, with 30 seconds as the default.

Graph refresh tokens are stored in the encrypted KDE Wallet; access tokens are
never written to the configuration file. On a later launch the wallet token is
renewed automatically. If it is unavailable or revoked, connect the profile
again from the configuration dialog.

`Remote only` now synchronizes the selected remote tree, identities, eTags, and
delta cursor while creating zero-byte placeholders at the remote paths. The
placeholders are persisted and excluded from local change detection; remote
renames and deletions update them without creating remote mutations. Switching
to this policy evicts the corresponding local content, while files outside the
known baseline remain untouched. `Download on demand` materializes a file when
an application reads it through the FUSE view. Folder policies inherit from
their nearest configured parent and can keep selected folders local.

## FAQ: unexpected downloads from the FUSE view

### Why did opening a folder download many files?

Listing a FUSE directory only requests names and metadata. Desktop applications
may nevertheless open files in the background to create previews, thumbnails,
media metadata, search indexes, or playlists. DriveBeacon treats every real
file `open` as a read and materializes that file. This is required for normal
applications to access the filesystem transparently.

### Why does Dolphin preview a FUSE file as local storage?

The FUSE mount is a local filesystem path such as `~/Onedrive-Graph-Test`, so
Dolphin applies its local-storage preview settings rather than its remote-KIO
settings. Disable Dolphin's preview panel and local thumbnails for this mount
if browsing should remain metadata-only. Baloo file indexing should also
exclude the mount:

```bash
balooctl6 config add excludeFolders "$HOME/Onedrive-Graph-Test"
balooctl6 config show excludeFolders
```

### Why did opening one video download all videos in its folder?

Some media players offer an option such as “play all videos in the folder” or
automatically build a playlist. Haruna, VLC, and mpv-based players can then
open every sibling video. Disable that option or open the file in single-file
mode. The FUSE journal shows the difference:

```bash
journalctl --user -t drivebeacon-fs -f
```

`getattr` and `readdir` are metadata operations; repeated `open` and `opened
cache` lines identify the application causing materialization.

### How can I check whether DriveBeacon itself is downloading files?

Inspect the service and FUSE logs separately:

```bash
journalctl --user -u drivebeacon-service.service -f
journalctl --user -t drivebeacon-fs -f
```

A viewer-caused download normally appears as an `open` in the FUSE log before
the Graph transfer begins.

### Where are the cache and visible files?

The visible path is the profile's FUSE mount directory. The service cache is
private and stored below `~/.local/share/drivebeacon/cache/<profile>`. Unmounting
the FUSE view does not delete the cache or remote files.

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

The headless Graph service can also be inspected and controlled without the
tray. It uses the active profile from the shared DriveBeacon configuration:

```bash
drivebeaconctl status
drivebeaconctl sync
drivebeaconctl refresh-folders
drivebeaconctl reload-profiles       # discover profiles saved after service start
drivebeaconctl pause                 # pause all Graph accounts
drivebeaconctl resume                # resume all Graph accounts
drivebeaconctl pause-profile NAME    # pause one account
drivebeaconctl resume-profile NAME   # resume one account
drivebeaconctl set-availability NAME remote-only
drivebeaconctl service start|stop|restart
```

`drivebeaconctl status` reports the active profile and then lists every loaded
Graph profile with its own authentication, pause state, synchronization status,
progress, and error message.

Pausing is non-destructive: DriveBeacon keeps the account's local files,
tokens, delta cursor, and synchronization baselines. Resuming continues from
that persisted state. The profile dialog and tray expose the same per-account
and global controls.

Closing the window leaves the tray application running. Use **Quit** in its tray
menu to exit completely. The `run` target exposes the build-tree desktop metadata
through `XDG_DATA_DIRS`, avoiding portal registration warnings without installing
the application.

## License

DriveBeacon is licensed under the GNU General Public License version 3.0 only.
See [LICENSE](LICENSE) for the complete terms.
