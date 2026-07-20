// SPDX-License-Identifier: GPL-3.0-only

#include "activityparser.h"

#include <QRegularExpression>

std::optional<ActivityEvent> ActivityParser::parse(const QString &message, const QDateTime &timestamp)
{
    // OneDrive writes a completion suffix as a separate part of several messages.
    // Remove it before matching while retaining the original message in the event.
    QString normalized = message.trimmed();
    bool completed = false;

    const QString completedSuffix = QStringLiteral(" ... done");
    if (normalized.endsWith(completedSuffix)) {
        normalized.chop(completedSuffix.size());
        completed = true;
    }

    // Transfers can be reported as new, modified, or unspecified files.
    static const QRegularExpression transferExpression(
        QStringLiteral("^(Downloading|Uploading)(?: (?:new|modified))? file: (.+)$"));
    const auto transferMatch = transferExpression.match(normalized);
    if (transferMatch.hasMatch()) {
        ActivityEvent event;
        event.timestamp = timestamp;
        event.operation = transferMatch.captured(1) == QLatin1String("Downloading")
            ? QStringLiteral("download")
            : QStringLiteral("upload");
        event.path = transferMatch.captured(2);
        event.message = message;
        event.completed = completed;
        return event;
    }

    // Moves are complete operations and therefore have no intermediate state here.
    static const QRegularExpression moveExpression(QStringLiteral("^Moving (.+) to (.+)$"));
    const auto moveMatch = moveExpression.match(normalized);
    if (moveMatch.hasMatch()) {
        return ActivityEvent{
            timestamp,
            QStringLiteral("move"),
            moveMatch.captured(1),
            moveMatch.captured(2),
            message,
            true,
        };
    }

    // Local deletions are emitted after OneDrive has decided to remove the item.
    static const QRegularExpression localDeletionExpression(
        QStringLiteral("^Deleting local (?:file|directory): (.+)$"));
    const auto localDeletionMatch = localDeletionExpression.match(normalized);
    if (localDeletionMatch.hasMatch()) {
        return ActivityEvent{
            timestamp,
            QStringLiteral("delete"),
            localDeletionMatch.captured(1),
            {},
            message,
            true,
        };
    }

    // The operating-system notification precedes the actual deletion, so it remains pending.
    static const QRegularExpression requestedDeletionExpression(QStringLiteral(
        "^The operating system sent a deletion notification\\. "
        "Trying to delete this item as requested: (.+)$"));
    const auto requestedDeletionMatch = requestedDeletionExpression.match(normalized);
    if (requestedDeletionMatch.hasMatch()) {
        return ActivityEvent{
            timestamp,
            QStringLiteral("delete"),
            requestedDeletionMatch.captured(1),
            {},
            message,
            false,
        };
    }

    return std::nullopt;
}
