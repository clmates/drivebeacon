// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDateTime>

/** Parses Graph Retry-After seconds or an HTTP date into a bounded delay. */
inline int graphRetryAfterSeconds(const QByteArray &header,
                                  const QDateTime &now = QDateTime::currentDateTimeUtc())
{
    const QByteArray value = header.trimmed();
    bool numeric = false;
    const int seconds = value.toInt(&numeric);
    if (numeric) {
        return qBound(1, seconds, 3600);
    }
    if (!value.isEmpty()) {
        QString dateValue = QString::fromLatin1(value);
        // Qt's RFC2822 parser accepts numeric offsets more consistently than
        // the literal GMT suffix emitted by some Graph-compatible gateways.
        if (dateValue.endsWith(QStringLiteral(" GMT"))) {
            dateValue.chop(4);
            dateValue += QStringLiteral(" +0000");
        }
        const QDateTime retryAt = QDateTime::fromString(dateValue, Qt::RFC2822Date);
        if (retryAt.isValid()) {
            return qBound(1, now.secsTo(retryAt), 3600);
        }
    }
    // A malformed or missing header must still produce a bounded backoff.
    return 5;
}

/** Combines Microsoft's delay with a bounded local backoff between retries. */
inline int graphRetryDelaySeconds(int retryAfterSeconds, int retryAttempt)
{
    const int providerDelay = qBound(1, retryAfterSeconds, 3600);
    const int exponent = qBound(0, retryAttempt, 6);
    const int localDelay = 5 * (1 << exponent);
    return qMax(providerDelay, localDelay);
}
