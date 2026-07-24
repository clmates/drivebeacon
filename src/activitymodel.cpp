// SPDX-License-Identifier: GPL-3.0-only

#include "activitymodel.h"

ActivityModel::ActivityModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

// QML only asks for top-level rows; this model does not expose a tree.
int ActivityModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_events.size());
}

QVariant ActivityModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_events.size()) {
        return {};
    }

    const auto &event = m_events.at(index.row());
    switch (role) {
    case TimestampRole:
        return event.timestamp;
    case OperationRole:
        return event.operation;
    case PathRole:
        return event.path;
    case DestinationPathRole:
        return event.destinationPath;
    case MessageRole:
        return event.message;
    case CompletedRole:
        return event.completed;
    default:
        return {};
    }
}

QHash<int, QByteArray> ActivityModel::roleNames() const
{
    return {
        {TimestampRole, "timestamp"},
        {OperationRole, "operation"},
        {PathRole, "path"},
        {DestinationPathRole, "destinationPath"},
        {MessageRole, "message"},
        {CompletedRole, "completed"},
    };
}

void ActivityModel::prepend(ActivityEvent event)
{
    beginInsertRows({}, 0, 0);
    m_events.prepend(std::move(event));
    endInsertRows();

    if (m_events.size() > maximumEntries) {
        beginRemoveRows({}, maximumEntries, m_events.size() - 1);
        m_events.erase(m_events.begin() + maximumEntries, m_events.end());
        endRemoveRows();
    }
}

void ActivityModel::updateGraphProgress(const QString &path, const QString &message,
                                        bool completed)
{
    // Identify progress by operation and path so repeated updates replace one
    // tray row instead of creating a new activity for every percentage.
    const QDateTime timestamp = QDateTime::currentDateTimeUtc();
    for (int row = 0; row < m_events.size(); ++row) {
        ActivityEvent &event = m_events[row];
        if (event.operation != QLatin1String("graph-progress") || event.path != path) {
            continue;
        }
        event.timestamp = timestamp;
        event.message = message;
        event.completed = completed;
        const QModelIndex index = this->index(row, 0);
        Q_EMIT dataChanged(index, index);
        if (row > 0) {
            beginMoveRows({}, row, row, {}, 0);
            m_events.move(row, 0);
            endMoveRows();
        }
        return;
    }

    prepend({timestamp, QStringLiteral("graph-progress"), path, {}, message, completed});
}

void ActivityModel::clear()
{
    if (m_events.isEmpty()) {
        return;
    }

    beginResetModel();
    m_events.clear();
    endResetModel();
}
