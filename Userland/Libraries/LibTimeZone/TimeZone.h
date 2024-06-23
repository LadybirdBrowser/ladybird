/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibTimeZone/Forward.h>

namespace TimeZone {

enum class IsLink {
    No,
    Yes,
};

struct TimeZoneIdentifier {
    StringView name;
    IsLink is_link { IsLink::No };
};

enum class InDST {
    No,
    Yes,
};

struct Offset {
    i64 seconds { 0 };
    InDST in_dst { InDST::No };
};

StringView current_time_zone();
ReadonlySpan<TimeZoneIdentifier> all_time_zones();

Optional<TimeZone> time_zone_from_string(StringView time_zone);
StringView time_zone_to_string(TimeZone time_zone);
Optional<StringView> canonicalize_time_zone(StringView time_zone);

Optional<Offset> get_time_zone_offset(TimeZone time_zone, AK::UnixDateTime time);
Optional<Offset> get_time_zone_offset(StringView time_zone, AK::UnixDateTime time);

Optional<Region> region_from_string(StringView region);
StringView region_to_string(Region region);
Vector<StringView> time_zones_in_region(StringView region);

}

template<>
struct AK::Formatter<TimeZone::TimeZone> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, TimeZone::TimeZone const& time_zone)
    {
        return Formatter<FormatString>::format(builder, TimeZone::time_zone_to_string(time_zone));
    }
};
