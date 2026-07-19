# DriveBeacon

DriveBeacon is a system tray indicator for KDE Plasma that observes and controls
the abraunegg `onedrive` user service. The first MVP provides a tray icon, service
start/stop/restart actions, recent upload and download activity from the systemd
journal, and navigation to affected local folders.

The application delegates synchronization to the existing `onedrive` client. It
does not access Microsoft Graph or modify OneDrive configuration.

DriveBeacon is an independent open-source project. It is not affiliated with or
endorsed by Microsoft Corporation or KDE e.V. OneDrive is a trademark of
Microsoft Corporation. KDE and Plasma are trademarks of KDE e.V.

## Requirements

- Qt 6 with QML, Quick, DBus, Widgets, and Qt Test
- KDE Frameworks 6: Kirigami, CoreAddons, I18n, StatusNotifierItem, and XmlGui
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
