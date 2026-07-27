# DriveBeacon development plan

This plan records the current stabilization work and the remaining product work.
Implementation details belong in the source and tests; this document tracks
priorities, expected behaviour, and completion criteria.

## Execution order

The implementation starts with the cleanup and isolation work in section 10.
This establishes which APIs, backends, metadata, and compatibility paths are
still supported before new synchronization behaviour is added. After that,
the numbered stabilization and feature sections are tackled in order, with
regression tests added before changing each confirmed defect.

## Current plan: stabilization after the code audit

### 1. Protect current behaviour with regression tests

- Add deterministic tests for profile-policy persistence, inherited path
  policies, profile reloads, and D-Bus command results.
- Add mocked Graph tests for delta changes, renames, deletions, expired tokens,
  throttling, and transfer scheduling.
- Add FUSE-facing tests for metadata, materialization failures, and concurrent
  access without contacting the live OneDrive service.

Completion criterion: each confirmed audit defect has a failing regression test
before its implementation is changed.

### 2. Make Graph authentication self-renewing

- Renew access tokens before their reported expiry.
- Handle HTTP 401 by pausing the failed operation, refreshing once, and retrying
  without requiring a service restart.
- Honour Graph throttling responses and `Retry-After`, with bounded backoff.
- Keep refresh tokens in the wallet and short-lived access tokens in memory.

Completion criterion: a continuously running service survives access-token
expiry and temporary Graph throttling without user intervention.

### 3. Consolidate the on-demand policy model

- Keep On Demand as the only profile-wide availability mode.
- Remove the obsolete profile-wide Keep Local and Remote Only controls from the
  tray, CLI, and service API after compatibility has been assessed.
- Preserve per-file and per-folder policies whenever a profile is edited.
- Make Keep Local on a folder replace conflicting descendant release policies.
- Retain the invariant that cache eviction never deletes remote content.

Completion criterion: editing a profile cannot discard Dolphin policies, and
the same inherited policy is reported and enforced by the service, FUSE view,
CLI, and Dolphin.

### 4. Make profile reloads complete and atomic

- Apply all changed settings to an existing controller: pause state, selected
  folders, polling interval, transfer limits, mount path, drive identity, and
  path policies.
- Add newly configured profiles and retire removed profiles without altering
  their remote drives.
- Handle mount-path changes safely, including an already mounted profile.
- Coalesce status refreshes so stale asynchronous D-Bus replies cannot overwrite
  newer account state.

Completion criterion: saving or removing a profile is reflected by the running
service without a restart or duplicate synchronization.

### 5. Give D-Bus and CLI operations reliable results

- Return explicit success or failure information for mount, unmount,
  materialize, Keep Local, cache release, resync, and profile operations.
- Propagate validation errors to `drivebeaconctl` and the tray instead of only
  logging them.
- Avoid printing `Requested ...` when the service rejected an operation.

Completion criterion: command exit status and user-visible messages describe
the operation's actual outcome.

### 6. Implement a writable FUSE synchronization model

- Allow applications to create files and directories directly inside the
  user-selected FUSE mount.
- Represent a newly created file immediately in the private cache and remote
  index as a pending local item; do not use a zero-byte placeholder as its
  authoritative content.
- Keep new and modified bytes in the private cache while the file is open.
  Queue the remote upload after `flush`, `fsync`, or the final close, while
  retaining dirty state until Graph confirms the upload.
- When an existing remote-only file is opened for writing, materialize its
  current remote contents first unless the operation explicitly truncates it.
  The resulting cache file then becomes the writable local version.
- Update the remote item ID, eTag, size, local signature, and cache state only
  after a successful Graph response. Failed uploads must remain visibly pending
  and retryable instead of being mistaken for synchronized content.
- Persist dirty files and pending operations so a service or desktop restart
  cannot lose an unsent creation or modification.
- Support directory creation, rename, move, truncate, and deletion with explicit
  conflict rules. Local deletion must never reach Graph merely because a cache
  entry was evicted.
- Distinguish these states in service, CLI, tray, and Dolphin: remote only,
  materializing, cached, locally modified, uploading, synchronized, and error.
- Define concurrent-edit conflict behaviour using the stored remote eTag. Never
  overwrite a newer remote version silently; preserve both versions or require
  an explicit conflict decision.
- Keep the mounted path authoritative for normal user I/O while the private
  cache remains an implementation detail and Graph remains the durable remote
  copy after upload confirmation.

Completion criterion: creating or editing a file through the FUSE mount makes
it immediately readable from that mount, persists its bytes safely in the
cache, uploads it to the correct remote path, and survives restart or network
failure without data loss or unintended remote deletion.

### 7. Remove blocking from the FUSE data path

- Move materialization waits away from the single FUSE callback thread.
- Allow unrelated metadata and file operations to continue during a large
  download.
- Distinguish an empty remote directory from a D-Bus/service failure.
- Validate every cache path independently before opening local content.

Completion criterion: downloading one large file does not freeze the mount, and
filesystem errors are reported accurately without exposing paths outside the
profile cache.

### 8. Harden transfers and local scanning

- Move expensive hashing away from the service event loop.
- Persist enough transfer state to resume large uploads and downloads after a
  service restart where Graph permits it.
- Keep small-file work responsive while large transfers are active.

Completion criterion: D-Bus and tray status remain responsive during hashing
and large transfers, and interrupted transfers resume rather than restart when
possible.

### 9. Optimize and complete Dolphin integration

- Cache the parsed profile and local-state indexes used by overlay requests.
- Invalidate that cache when DriveBeacon state changes instead of rereading and
  scanning the complete configuration for every displayed item.
- Batch multi-selection actions instead of starting one CLI process per path.
- Make actions available for every supported file type and directory.
- Keep overlays lightweight and non-blocking.

Completion criterion: opening a directory containing thousands of DriveBeacon
items does not noticeably delay Dolphin, and all items expose consistent
actions and state.

### 10. Remove or isolate obsolete code

- Review and either remove or explicitly retain the unreachable OAuth Device
  Code flow.
- Review unused credential removal, error-clearing, login-cancellation,
  service-client eviction, and legacy D-Bus methods.
- Share duplicated profile/path parsing between the Dolphin plugins.
- Update AppStream and desktop metadata to describe Graph, multi-account,
  on-demand, FUSE, and the optional legacy backend accurately.

Completion criterion: retained compatibility APIs are documented and tested;
unreachable implementation and stale product descriptions are gone.

Initial cleanup completed: the unreachable Device Code implementation and its
unused controller properties were removed; the unconsumed legacy service D-Bus
methods were removed; and the AppStream/README descriptions now identify Graph
and FUSE as the primary architecture while documenting abraunegg as optional
compatibility. The unused `TokenStore::remove()` helper was also removed because
profile deletion is not implemented yet; credential cleanup will be introduced
with that complete local-only deletion workflow. The remaining API and parsing
review is still pending.

## Pending points from the previous plan

### Intelligent cache management

- Add automatic cache release based on configurable free-space thresholds.
- Add optional release based on time since last access.
- Never evict paths marked Keep Local.
- Expose planned and completed automatic evictions in the journal and tray.

### Finish the transition away from legacy storage modes

- Migrate remaining profiles safely from historical local directories to the
  private cache plus user-selected FUSE mount.
- Deprecate the global Remote Only/Keep Local model after migration support is
  complete.
- Deprecate the abraunegg backend only after Graph covers the required workflows
  and existing users have a documented migration path.

### Product Graph application identity

- Replace the development Microsoft application ID with the final DriveBeacon
  application registration before the first production package.
- Provide that value as the default while retaining documented support for a
  user-supplied application ID.

### Packaging and first release of the new architecture

- Complete the release review, version bump, annotated `vMAJOR.MINOR.PATCH` tag,
  and GitHub release.
- Update and validate the separate AUR repository from the release tarball and
  verified checksum, leaving the AUR push to the user.

### Remote change notifications

- Continue using delta queries and configurable polling for the desktop client.
- Reconsider Graph change notifications only if DriveBeacon later gains a
  suitable public HTTPS callback service; they are not implementable solely by
  the current local desktop process.
