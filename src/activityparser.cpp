#include "activityparser.h"

#include <QRegularExpression>

std::optional<ActivityEvent> ActivityParser::parse(const QString &message, const QDateTime &timestamp)
{
    QString normalized = message.trimmed();
    bool completed = false;

    const QString completedSuffix = QStringLiteral(" ... done");
    if (normalized.endsWith(completedSuffix)) {
        normalized.chop(completedSuffix.size());
        completed = true;
    }

    static const QRegularExpression transferExpression(
        QStringLiteral("^(Downloading|Uploading)(?: (?:new|modified))? file: (.+)$"));
    const auto transferMatch = transferExpression.match(normalized);
    if (!transferMatch.hasMatch()) {
        return std::nullopt;
    }

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
