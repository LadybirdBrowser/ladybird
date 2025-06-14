/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/StringView.h>

namespace AK {

static constexpr Array<StringView, 7> long_day_names = {
    "Sunday"_sv, "Monday"_sv, "Tuesday"_sv, "Wednesday"_sv, "Thursday"_sv, "Friday"_sv, "Saturday"_sv
};

static constexpr Array<StringView, 7> short_day_names = {
    "Sun"_sv, "Mon"_sv, "Tue"_sv, "Wed"_sv, "Thu"_sv, "Fri"_sv, "Sat"_sv
};

static constexpr Array<StringView, 7> mini_day_names = {
    "Su"_sv, "Mo"_sv, "Tu"_sv, "We"_sv, "Th"_sv, "Fr"_sv, "Sa"_sv
};

static constexpr Array<StringView, 7> micro_day_names = {
    "S"_sv, "M"_sv, "T"_sv, "W"_sv, "T"_sv, "F"_sv, "S"_sv
};

static constexpr Array<StringView, 12> long_month_names = {
    "January"_sv, "February"_sv, "March"_sv, "April"_sv, "May"_sv, "June"_sv,
    "July"_sv, "August"_sv, "September"_sv, "October"_sv, "November"_sv, "December"_sv
};

static constexpr Array<StringView, 12> short_month_names = {
    "Jan"_sv, "Feb"_sv, "Mar"_sv, "Apr"_sv, "May"_sv, "Jun"_sv,
    "Jul"_sv, "Aug"_sv, "Sep"_sv, "Oct"_sv, "Nov"_sv, "Dec"_sv
};

}

#if USING_AK_GLOBALLY
using AK::long_day_names;
using AK::long_month_names;
using AK::micro_day_names;
using AK::mini_day_names;
using AK::short_day_names;
using AK::short_month_names;
#endif
