/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace JS::Temporal {

struct Annotation {
    bool critical { false };
    StringView key;
    StringView value;
};

struct TimeZoneOffset {
    Optional<char> sign;
    Optional<StringView> hours;
    Optional<StringView> minutes;
    Optional<StringView> seconds;
    Optional<StringView> fraction;
    StringView source_text;
};

struct ParseResult {
    Optional<char> sign;

    Optional<StringView> date_year;
    Optional<StringView> date_month;
    Optional<StringView> date_day;
    Optional<StringView> time_hour;
    Optional<StringView> time_minute;
    Optional<StringView> time_second;
    Optional<StringView> time_fraction;
    Optional<TimeZoneOffset> date_time_offset;

    Optional<StringView> utc_designator;
    Optional<StringView> time_zone_identifier;
    Optional<StringView> time_zone_iana_name;
    Optional<TimeZoneOffset> time_zone_offset;

    Optional<StringView> duration_years;
    Optional<StringView> duration_months;
    Optional<StringView> duration_weeks;
    Optional<StringView> duration_days;
    Optional<StringView> duration_hours;
    Optional<StringView> duration_hours_fraction;
    Optional<StringView> duration_minutes;
    Optional<StringView> duration_minutes_fraction;
    Optional<StringView> duration_seconds;
    Optional<StringView> duration_seconds_fraction;

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

Optional<ParseResult> parse_iso8601(Production, StringView);

enum class SubMinutePrecision {
    No,
    Yes,
};

Optional<TimeZoneOffset> parse_utc_offset(StringView, SubMinutePrecision);

}
