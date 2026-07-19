# Repository Guidelines

## Project Structure & Module Organization

Production C++ code lives in `src/`, with the Kirigami interface in `src/qml/`. Automated Qt Test cases live in `tests/`, and installable desktop metadata belongs in `data/`. Keep generated output in `build/`; never add generated MOC, QML cache, or binary files to the source tree. Mirror backend components in tests where practical, such as `src/activityparser.*` and `tests/activityparser_test.cpp`.

Update this guide when the project adopts a framework or a different layout.

## Documentation Policy

The source code is the primary source of truth. Keep Markdown documents at a high level: describe architecture, workflows, decisions, and user-facing behavior without duplicating implementation details. Prefer clear code, tests, types, and focused comments for low-level behavior. If documentation and code diverge, verify the code and update the document accordingly.

## Destructive Action Safety

Never perform a destructive action without the user's explicit prior authorization. Before requesting authorization, identify the exact targets and explain the concrete risks, affected data or services, expected impact, and whether recovery is possible. Approval must apply to the specific action described; do not treat general or earlier consent as authorization for a different destructive operation. Prefer reversible alternatives whenever available.

## Dependency Age Policy

Never install a package or dependency version published less than 30 days ago. Verify the release date of the exact version before installation; if its age cannot be confirmed, do not install it until reliable release information is available. Prefer the newest compatible version that satisfies this minimum age, and record intentional version pins in the project's dependency configuration.

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

No readable Git history is available in the current scaffold, so use short, imperative commit subjects (for example, `Add storage client retries`). Keep commits focused and explain non-obvious tradeoffs in the body. Pull requests should summarize the change, explain how it was tested, and link relevant issues. Include screenshots for visible UI changes and call out configuration changes, migrations, or follow-up work.

Never commit directly to the `prod` branch, even if the user explicitly authorizes or requests it. Before committing, verify the current branch. Commits are permitted only on `test` or on a dedicated branch for a specific feature, preferably named `feature/<short-description>`. Move changes to production exclusively through the repository's review and merge process.
