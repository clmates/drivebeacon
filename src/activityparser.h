// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "activityevent.h"

#include <optional>

/** Converts recognized OneDrive journal messages into normalized events. */
class ActivityParser
{
public:
    /** Parses one journal message, or returns no event for unrelated messages. */
    [[nodiscard]] static std::optional<ActivityEvent> parse(
        const QString &message,
        const QDateTime &timestamp = QDateTime::currentDateTime());
};
