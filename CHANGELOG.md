# Changelog

All notable user-visible changes to DriveBeacon are documented here. Entries are
grouped by release tag; unreleased work belongs under `Unreleased` until the next
version is tagged.

## [Unreleased]

No changes yet.

## [1.0.15] — 2026-08-23

- Improved the FUSE metadata path used by Dolphin.
- Persisted remote folders adopted during synchronization so restarts do not
  repeat folder creation attempts.
- Synchronized the About dialog, Dolphin plugin metadata, and AppStream release
  information with the release version.

## [1.0.14] — 2026-08-23

- Persisted the identity of existing remote folders discovered during local
  reconciliation.

## [1.0.13] — 2026-08-23

- Blocked local mutations while the initial synchronization baseline is being
  recovered, preventing spurious uploads and folder operations after restart.

## [1.0.11] — 2026-08-23

- Paused local scans while remote downloads are active to avoid treating
  incomplete files as local changes.

## [1.0.10] — 2026-08-23

- Prevented uploads while the synchronization baseline is being established.

## [1.0.9] — 2026-08-23

- Fixed FUSE context initialization.

## [1.0.8] — 2026-08-23

- Optimized FUSE metadata caching, substantially improving directory navigation
  in Dolphin for large libraries.

## [1.0.7] — 2026-07-30

- Kept remote folders visible while a profile is paused.

## [1.0.6] — 2026-07-29

- Started new profiles paused so synchronization can be reviewed before it runs.

## [1.0.5] — 2026-07-29

- Added an on-demand cache policy action.

## [1.0.4] — 2026-07-29

- Recovered invalid Microsoft Graph delta cursors automatically.

## [1.0.3] — 2026-07-29

- Fixed reconciliation of cache folders.

## [1.0.2] — 2026-07-28

- Fixed profile folder enumeration.

## [1.0.1] — 2026-07-28

- Connected profiles without switching the tray application to another profile.

## [1.0.0] — 2026-07-28

- Added the native Graph synchronization service and command-line control.
- Added multi-account profiles, pause controls, transfer activity, and service
  status in the KDE tray.
- Added the FUSE-backed on-demand storage view.
- Added Dolphin actions and cache-state overlays.
- Improved local change detection and concurrent Graph synchronization.

## [0.1.2] — 2026-07-21

- Anchored the native tray menu correctly.

## [0.1.1] — 2026-07-19

- Added activity reporting for moved and deleted files.

## [0.1.0] — 2026-07-19

- Initial packaged MVP release.
