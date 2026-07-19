// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "activityevent.h"

#include <QAbstractListModel>
#include <QList>

class ActivityModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        TimestampRole = Qt::UserRole + 1,
        OperationRole,
        PathRole,
        MessageRole,
        CompletedRole,
    };

    explicit ActivityModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void prepend(ActivityEvent event);
    Q_INVOKABLE void clear();

private:
    static constexpr int maximumEntries = 200;
    QList<ActivityEvent> m_events;
};
