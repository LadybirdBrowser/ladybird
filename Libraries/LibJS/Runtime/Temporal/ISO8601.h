/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>

namespace JS::Temporal {

struct ParseResult {
    Optional<char> sign;
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
};

enum class Production {
    TemporalDurationString,
};

Optional<ParseResult> parse_iso8601(Production, StringView);

}
