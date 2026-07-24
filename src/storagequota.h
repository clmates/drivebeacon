// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

/** Describes the capacity reported by a remote drive. */
struct StorageQuota {
    /** Total capacity in bytes, or -1 when the provider did not report it. */
    qint64 total = -1;
    /** Bytes currently used by the account or drive. */
    qint64 used = -1;
    /** Bytes available for new content. */
    qint64 remaining = -1;
    /** Time at which this snapshot was obtained. */
    QDateTime lastUpdated;
    /** Whether the snapshot could not be refreshed successfully. */
    bool stale = true;

    /** Parses the Graph drive.quota object. */
    [[nodiscard]] static StorageQuota fromGraphObject(const QJsonObject &object,
                                                       const QDateTime &updatedAt);
};
