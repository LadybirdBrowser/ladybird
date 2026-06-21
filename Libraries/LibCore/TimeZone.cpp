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
    TRY(Unicode::set_current_time_zone(time_zone_utf16));
    TRY(Core::Environment::set("TZ"sv, time_zone, Core::Environment::Overwrite::Yes));
    tzset();
    return {};
}

String current_time_zone()
{
    auto time_zone = Unicode::current_time_zone();
    return time_zone.utf16_view().to_utf8_but_should_be_ported_to_utf16();
}

}
