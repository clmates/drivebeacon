// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "syncprofile.h"

#include <QObject>
#include <QStringList>

/** Stores the active profile in the user's application settings. */
class ProfileStore final : public QObject
{
    Q_OBJECT

public:
    /** Creates a store using the standard DriveBeacon settings location. */
    explicit ProfileStore(QObject *parent = nullptr);

    /** Returns profile names stored in the user's configuration. */
    [[nodiscard]] QStringList profileNames() const;
    /** Returns the configured active profile name. */
    [[nodiscard]] QString activeProfileName() const;
    /** Loads a named profile, preserving the existing default directory. */
    [[nodiscard]] SyncProfile load(const QString &name = {}) const;
    /** Persists the active profile for the next application start. */
    void save(const SyncProfile &profile);
    /** Returns whether synchronization is globally enabled for all profiles. */
    [[nodiscard]] bool globalSyncEnabled() const;
    /** Persists the global pause state without changing individual profiles. */
    void setGlobalSyncEnabled(bool enabled);
    /** Marks a profile as the default when no command-line override is supplied. */
    void setActiveProfileName(const QString &name);

private:
    /** Maps user-facing names to safe QSettings group components. */
    [[nodiscard]] static QString normalizedName(const QString &name);
};
