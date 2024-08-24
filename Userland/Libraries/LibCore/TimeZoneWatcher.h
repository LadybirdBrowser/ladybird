/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>

namespace Core {

class TimeZoneWatcher {
    AK_MAKE_NONCOPYABLE(TimeZoneWatcher);

public:
    static ErrorOr<NonnullOwnPtr<TimeZoneWatcher>> create();
    virtual ~TimeZoneWatcher() = default;

    Function<void()> on_time_zone_changed;

protected:
    TimeZoneWatcher() = default;
};

}
