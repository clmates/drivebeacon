# Repository Guidelines

## Project Structure & Module Organization

Production C++ code lives in `src/`; the tray status and activity interface is a native Qt menu assembled in `src/main.cpp`. Automated Qt Test cases live in `tests/`, and installable desktop metadata belongs in `data/`. Keep generated output in `build/`; never add generated MOC, QML cache, or binary files to the source tree. Mirror backend components in tests where practical, such as `src/activityparser.*` and `tests/activityparser_test.cpp`.

Update this guide when the project adopts a framework or a different layout.

## Documentation Policy

The source code is the primary source of truth. Keep Markdown documents at a high level: describe architecture, workflows, decisions, and user-facing behavior without duplicating implementation details. Prefer clear code, tests, types, and focused comments for low-level behavior. If documentation and code diverge, verify the code and update the document accordingly.

Document code as part of every code change. Add concise comments or Doxygen documentation for new or modified classes, structs, enums, properties, signals, and functions, including private helpers when their purpose is not immediately obvious. Comments should explain responsibilities, data flow, invariants, security constraints, lifecycle decisions, and non-obvious tradeoffs; do not restate syntax or implementation line by line. Keep comments next to the declarations or logic they describe, update stale comments when behavior changes, and add structural comments in QML when they clarify the relationship between UI sections and backend properties.

Maintain `CHANGELOG.md` as the user-facing summary of released behavior. Before
creating a release tag, review the commits since the previous tag, move the
relevant entries from `Unreleased` into a dated version section, and describe
changes by user-visible behavior rather than commit titles. The changelog must be
included in the release commit and remain consistent with the About and AppStream
metadata.

## Destructive Action Safety

Never perform a destructive action without the user's explicit prior authorization. Before requesting authorization, identify the exact targets and explain the concrete risks, affected data or services, expected impact, and whether recovery is possible. Approval must apply to the specific action described; do not treat general or earlier consent as authorization for a different destructive operation. Prefer reversible alternatives whenever available.

## Dependency Age Policy

Never install a third-party package or dependency version published less than 30 days ago. Verify the release date of the exact version before installation; if its age cannot be confirmed, do not install it until reliable release information is available. Prefer the newest compatible version that satisfies this minimum age, and record intentional version pins in the project's dependency configuration.

Locally built DriveBeacon packages are exempt when they are produced directly from the current repository and installed only for development or packaging validation. This exception does not extend to bundled or transitive third-party dependencies, downloaded release artifacts, or packages built from unreviewed external changes.

## Build, Test, and Development Commands

Run all commands from the repository root:

- `cmake -S . -B build -G Ninja -DBUILD_TESTING=ON` — configure development and test targets.
- `cmake --build build` — compile the application.
- `ctest --test-dir build --output-on-failure` — run all tests with useful failure output.
- `qmllint -I /usr/lib/qt6/qml src/qml/Main.qml` — statically check the QML interface.

Launch a development build with `cmake --build build --target run`. This target exposes the build-tree desktop metadata to the XDG portal without installing it system-wide.

## Coding Style & Naming Conventions

Use C++20, four-space indentation in C++, four spaces in QML, UTF-8 text, and final newlines. Format types as `PascalCase`, methods and local variables as `camelCase`, and private data members with an `m_` prefix. Keep filenames lowercase, for example `systemdmanager.cpp`. Prefer Qt types and parent-owned `QObject` lifetimes. Keep QML declarative; systemd, journal, and process logic belongs in the C++ backend.

## Testing Guidelines

Every behavior change should include a Qt Test where practical. Name test executables `<component>_test` and test files `<component>_test.cpp`. Keep unit tests deterministic and independent of the live OneDrive service; use representative journal messages as fixtures. Manual integration checks may use the current user's service but must never stop, restart, or alter it without explicit authorization. Bug fixes require a regression test.

## Commit & Pull Request Guidelines

Use short, imperative commit subjects consistent with the existing history (for example, `Add storage client retries`). Keep commits focused and explain non-obvious tradeoffs in the body. Pull requests should summarize the change, explain how it was tested, and link relevant issues. Include screenshots for visible UI changes and call out configuration changes, migrations, or follow-up work.

Release tags use the `vMAJOR.MINOR.PATCH` format (for example, `v0.1.1`) and must be annotated tags pointing at the release commit. Keep a matching bare version tag only when compatibility with an already-published release requires it; new releases should use the `v`-prefixed form consistently.

For every versioned correction or release, keep the user-visible About information
and release metadata synchronized with the tagged version. The About dialog version
must come from `PROJECT_VERSION`/`DRIVEBEACON_VERSION`, the Dolphin plugin metadata
must use the same version, and `data/io.github.clmates.drivebeacon.metainfo.xml`
must contain a matching AppStream release entry. Verify that no stale version
remains in these files before creating the annotated tag.

Never commit directly to the `prod` branch, even if the user explicitly authorizes or requests it. Before committing, verify the current branch. Commits are permitted only on `test` or on a dedicated branch for a specific feature, preferably named `feature/<short-description>`. Move changes to production exclusively through the repository's review and merge process.

## AUR Packaging

Prepare Arch User Repository metadata in the sibling repository
`/home/clmates/programación/drivebeacon-aur`, not in this source repository. Commit
`PKGBUILD` and `.SRCINFO` from that repository, but leave the push to its AUR
remote to the user. Codex must not push the AUR repository. Keep packaging-only
commits and files out of DriveBeacon's source history.

For every released AUR update, follow this order:

1. Bump the project version in `CMakeLists.txt`.
2. Create an annotated release tag with the `vMAJOR.MINOR.PATCH` form on the release commit.
3. Push the release commit and the `v`-prefixed tag to GitHub before preparing AUR metadata.
4. Point `PKGBUILD` at the GitHub release tarball for that tag and replace `SKIP` with its verified SHA-256 checksum.
5. Regenerate and validate `.SRCINFO` with `makepkg --printsrcinfo`.
6. Commit the resulting `PKGBUILD` and `.SRCINFO` in `drivebeacon-aur`; the user performs the AUR push.

Do not upload a Git-commit snapshot or leave `sha256sums=('SKIP')` for a normal release package; those are only temporary development fallbacks.
