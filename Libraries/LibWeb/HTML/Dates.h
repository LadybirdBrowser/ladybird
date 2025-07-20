/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Utf16String.h>
#include <LibJS/Runtime/Date.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

WEB_API u32 week_number_of_the_last_day(u64 year);
bool is_valid_week_string(Utf16View const& value);
bool is_valid_month_string(Utf16View const& value);
bool is_valid_date_string(Utf16View const& value);
bool is_valid_local_date_and_time_string(Utf16View const& value);
Utf16String normalize_local_date_and_time_string(Utf16String const& value);
bool is_valid_time_string(Utf16View const& value);
WebIDL::ExceptionOr<GC::Ref<JS::Date>> parse_time_string(JS::Realm& realm, StringView value);

struct YearAndMonth {
    u32 year;
    u32 month;
};
Optional<YearAndMonth> parse_a_month_string(StringView);

struct WeekYearAndWeek {
    u32 week_year;
    u32 week;
};
Optional<WeekYearAndWeek> parse_a_week_string(StringView);

struct YearMonthDay {
    u32 year;
    u32 month;
    u32 day;
};

struct HourMinuteSecond {
    i32 hour;
    i32 minute;
    f32 second;
};

struct DateAndTime {
    YearMonthDay date;
    HourMinuteSecond time;
};

Optional<YearMonthDay> parse_a_date_string(StringView);

Optional<DateAndTime> parse_a_local_date_and_time_string(StringView);

i32 number_of_months_since_unix_epoch(YearAndMonth);

}
