// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDateTime>
#include <QString>

struct ActivityEvent {
    QDateTime timestamp;
    QString operation;
    QString path;
    QString message;
    bool completed = false;
};
