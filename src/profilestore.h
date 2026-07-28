// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "syncprofile.h"

#include <QObject>
#include <QStringList>
#include <QSettings>

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
    /** Returns the global minimum free cache space in bytes; zero disables it. */
    [[nodiscard]] qint64 cacheMinimumFreeBytes() const;
    /** Persists the global pause state without changing individual profiles. */
    void setGlobalSyncEnabled(bool enabled);
    /** Persists the global minimum free cache space in bytes. */
    void setCacheMinimumFreeBytes(qint64 bytes);
    /** Marks a profile as the default when no command-line override is supplied. */
    void setActiveProfileName(const QString &name);

private:
    /** Opens the shared INI file used identically by tray and service. */
    [[nodiscard]] static QSettings settings();
    /** Maps user-facing names to safe QSettings group components. */
    [[nodiscard]] static QString normalizedName(const QString &name);
};
