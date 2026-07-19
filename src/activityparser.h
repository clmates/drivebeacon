// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "activityevent.h"

#include <optional>

class ActivityParser
{
public:
    [[nodiscard]] static std::optional<ActivityEvent> parse(
        const QString &message,
        const QDateTime &timestamp = QDateTime::currentDateTime());
};
