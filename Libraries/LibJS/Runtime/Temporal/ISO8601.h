/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>

namespace JS::Temporal {

struct Annotation {
    bool critical { false };
    Utf16View key;
    Utf16View value;
};

struct TimeZoneOffset {
    Optional<char> sign;
    Optional<Utf16View> hours;
    Optional<Utf16View> minutes;
    Optional<Utf16View> seconds;
    Optional<Utf16View> fraction;
    Utf16View source_text;
};

struct ParseResult {
    Optional<char> sign;

    Optional<Utf16View> date_year;
    Optional<Utf16View> date_month;
    Optional<Utf16View> date_day;
    Optional<Utf16View> time_hour;
    Optional<Utf16View> time_minute;
    Optional<Utf16View> time_second;
    Optional<Utf16View> time_fraction;
    Optional<TimeZoneOffset> date_time_offset;

    Optional<Utf16View> utc_designator;
    Optional<Utf16View> time_zone_identifier;
    Optional<Utf16View> time_zone_iana_name;
    Optional<TimeZoneOffset> time_zone_offset;

    Optional<Utf16View> duration_years;
    Optional<Utf16View> duration_months;
    Optional<Utf16View> duration_weeks;
    Optional<Utf16View> duration_days;
    Optional<Utf16View> duration_hours;
    Optional<Utf16View> duration_hours_fraction;
    Optional<Utf16View> duration_minutes;
    Optional<Utf16View> duration_minutes_fraction;
    Optional<Utf16View> duration_seconds;
    Optional<Utf16View> duration_seconds_fraction;

    Vector<Annotation> annotations;
};

enum class Production {
    AnnotationValue,
    DateMonth,
    TemporalDateTimeString,
    TemporalDurationString,
    TemporalInstantString,
    TemporalMonthDayString,
    TemporalTimeString,
    TemporalYearMonthString,
    TemporalZonedDateTimeString,
    TimeZoneIdentifier,
};

Optional<ParseResult> parse_iso8601(Production, Utf16View);

enum class SubMinutePrecision {
    No,
    Yes,
};

Optional<TimeZoneOffset> parse_utc_offset(Utf16View, SubMinutePrecision);

}
