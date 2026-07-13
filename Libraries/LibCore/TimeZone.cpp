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
    auto time_zone_utf16 = Utf16String::from_utf8(time_zone);
    return set_current_time_zone(time_zone_utf16.utf16_view());
}

ErrorOr<void> set_current_time_zone(Utf16View time_zone)
{
    TRY(Unicode::set_current_time_zone(time_zone));
    auto time_zone_utf8 = TRY(time_zone.to_utf8());
    TRY(Core::Environment::set("TZ"sv, time_zone_utf8, Core::Environment::Overwrite::Yes));
    tzset();
    return {};
}

Utf16String current_time_zone()
{
    return Unicode::current_time_zone();
}

}
