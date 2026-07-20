// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "activityevent.h"

#include <QAbstractListModel>
#include <QList>

/** List-model adapter that exposes recent ActivityEvent values to QML. */
class ActivityModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        /** Event timestamp. */
        TimestampRole = Qt::UserRole + 1,
        /** Normalized operation name. */
        OperationRole,
        /** Source path. */
        PathRole,
        /** Move destination path. */
        DestinationPathRole,
        /** Original journal message. */
        MessageRole,
        /** Whether the operation is complete. */
        CompletedRole,
    };

    /** Creates an empty activity model owned by the given parent. */
    explicit ActivityModel(QObject *parent = nullptr);

    /** Returns the number of top-level activity rows. */
    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    /** Returns the value for one activity row and model role. */
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    /** Maps the C++ roles to the names consumed by QML delegates. */
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /** Inserts an event at the front and removes entries beyond the history limit. */
    void prepend(ActivityEvent event);
    /** Removes all activity rows. */
    Q_INVOKABLE void clear();

private:
    /** Keeps the activity history bounded while the journal is followed. */
    static constexpr int maximumEntries = 200;
    QList<ActivityEvent> m_events;
};
