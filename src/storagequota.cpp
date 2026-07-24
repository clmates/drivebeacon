// SPDX-License-Identifier: GPL-3.0-only

#include "storagequota.h"

StorageQuota StorageQuota::fromGraphObject(const QJsonObject &object,
                                           const QDateTime &updatedAt)
{
    StorageQuota quota;
    const auto numberOrUnknown = [&object](const QString &key) {
        return object.contains(key) ? object.value(key).toVariant().toLongLong() : qint64(-1);
    };
    quota.total = numberOrUnknown(QStringLiteral("total"));
    quota.used = numberOrUnknown(QStringLiteral("used"));
    quota.remaining = numberOrUnknown(QStringLiteral("remaining"));
    quota.lastUpdated = updatedAt;
    quota.stale = false;
    return quota;
}
