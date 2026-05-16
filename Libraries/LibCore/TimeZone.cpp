/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Environment.h>
#include <LibCore/TimeZone.h>
#include <LibUnicode/TimeZone.h>
#include <time.h>

namespace Core::TimeZone {

ErrorOr<void> set_current_time_zone(StringView time_zone)
{
    TRY(Unicode::set_current_time_zone(time_zone));
    TRY(Core::Environment::set("TZ"sv, time_zone, Core::Environment::Overwrite::Yes));
    tzset();
    return {};
}

String current_time_zone()
{
    return Unicode::current_time_zone();
}

}
