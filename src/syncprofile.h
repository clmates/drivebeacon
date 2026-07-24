// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>

/** Backend selected to synchronize one profile. */
enum class SyncBackend {
    AbrauneggJournal,
    MicrosoftGraph,
};

/** Policy for the local representation of a synchronized item. */
enum class LocalAvailability {
    KeepLocal,
    RemoteOnly,
    OnDemand,
};

/** Persisted synchronization profile shared by all backends. */
struct SyncProfile {
    /** User-facing profile key; also namespaces persisted credentials. */
    QString name = QStringLiteral("default");
    /** Backend implementation selected for this profile. */
    SyncBackend backend = SyncBackend::AbrauneggJournal;
    /** Legacy account identifier retained for journal backend compatibility. */
    QString accountId;
    /** Public Microsoft application ID used by the OAuth flow. */
    QString graphClientId;
    /** Stable Graph drive ID selected for synchronization. */
    QString remoteDriveId;
    /** Absolute local synchronization root. */
    QString localDirectory;
    /** Whether Graph content is materialized locally. */
    LocalAvailability availability = LocalAvailability::KeepLocal;
    /** Polling period for Graph delta requests, clamped to 10..3600 seconds. */
    int remoteCheckIntervalSeconds = 30;
    /** Opaque Graph cursor resumed after restart; empty forces a baseline sync. */
    QString graphDeltaLink;
    /** Persisted `relativePath<TAB>sha256` local change baseline. */
    QStringList graphLocalSignatures;
    /** Persisted `itemId<TAB>relativePath<TAB>eTag` remote identity baseline. */
    QStringList graphRemotePaths;
    /** First-level remote folders explicitly selected for synchronization. */
    QStringList includedFolders;
    /** First-level remote folders explicitly excluded from synchronization. */
    QStringList excludedFolders;
};

/** Converts enum values to stable configuration strings. */
[[nodiscard]] QString syncBackendName(SyncBackend backend);
/** Converts a configuration string to a backend, defaulting to journal mode. */
[[nodiscard]] SyncBackend syncBackendFromName(const QString &name);
/** Converts availability values to stable configuration strings. */
[[nodiscard]] QString localAvailabilityName(LocalAvailability availability);
/** Converts a configuration string to an availability policy. */
[[nodiscard]] LocalAvailability localAvailabilityFromName(const QString &name);
