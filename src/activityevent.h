// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDateTime>
#include <QString>

/** A normalized file-system activity entry shown by the user interface. */
struct ActivityEvent {
    /** Time at which the journal reported the activity. */
    QDateTime timestamp;
    /** Stable operation name used by the model and QML (for example, download). */
    QString operation;
    /** Source path involved in the operation. */
    QString path;
    /** Destination path for moves; empty for other operations. */
    QString destinationPath;
    /** Original journal message, retained for display or future diagnostics. */
    QString message;
    /** Whether the journal indicates that the operation has finished. */
    bool completed = false;
};
