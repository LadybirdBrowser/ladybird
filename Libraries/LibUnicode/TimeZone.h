/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>

namespace Unicode {

struct TimeZoneOffset {
    enum class InDST {
        No,
        Yes,
    };

    AK::Duration offset;
    InDST in_dst { InDST::No };
};

struct TimeZoneTransition {
    struct Options {
        enum class Direction {
            Previous,
            Next,
        };

        enum class IncludeGivenTime {
            No,
            Yes,
        };

        enum class TransitionRule {
            AnyTransition,
            TransitionWhereUTCOffsetChanges,
        };

        Direction direction { Direction::Previous };
        IncludeGivenTime include_given_time { IncludeGivenTime::No };
        TransitionRule transition_rule { TransitionRule::AnyTransition };
    };

    AK::Duration transition;
};

Utf16String current_time_zone();
ErrorOr<void> set_current_time_zone(Utf16View);
void clear_system_time_zone_cache();
Vector<Utf16String> const& available_time_zones();
Vector<Utf16String> available_time_zones_in_region(Utf16View region);
Optional<Utf16String> resolve_primary_time_zone(Utf16View time_zone);
Optional<TimeZoneOffset> time_zone_offset(Utf16View time_zone, UnixDateTime time);
Vector<TimeZoneOffset> disambiguated_time_zone_offsets(Utf16View time_zone, UnixDateTime time);
Optional<TimeZoneTransition> get_time_zone_transition(Utf16View time_zone, UnixDateTime time, TimeZoneTransition::Options options);

}
