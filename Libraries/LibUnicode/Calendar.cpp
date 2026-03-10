/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/Calendar.h>

struct FfiISODate {
    i32 year;
    u8 month;
    u8 day;
};

struct FfiOptionalISODate {
    FfiISODate iso_date;
    bool has_value;
};

struct FfiCalendarDate {
    i32 year;
    u8 month;
    u8 month_code[5];
    u8 month_code_length;
    u8 day;
    u8 day_of_week;
    u16 day_of_year;
    u8 days_in_week;
    u8 days_in_month;
    u16 days_in_year;
    u8 months_in_year;
    bool in_leap_year;
};

extern "C" {

FfiCalendarDate icu_iso_date_to_calendar_date(u8 const* calendar, size_t calendar_length, i32 iso_year, u8 iso_month, u8 iso_day);
FfiOptionalISODate icu_calendar_date_to_iso_date(u8 const* calendar, size_t calendar_length, i32 arithmetic_year, u8 ordinal_month, u8 day);
FfiOptionalISODate icu_calendar_month_code_to_iso_date(u8 const* calendar, size_t calendar_length, i32 iso_year, u8 const* month_code, size_t month_code_length, u8 day);

u8 icu_calendar_months_in_year(u8 const* calendar, size_t calendar_length, i32 arithmetic_year);
u8 icu_calendar_days_in_month(u8 const* calendar, size_t calendar_length, i32 arithmetic_year, u8 ordinal_month);
u8 icu_calendar_max_days_in_month_code(u8 const* calendar, size_t calendar_length, u8 const* month_code, size_t month_code_length);
bool icu_year_contains_month_code(u8 const* calendar, size_t calendar_length, i32 arithmetic_year, u8 const* month_code, size_t month_code_length);

} // extern "C"

namespace Unicode {

CalendarDate iso_date_to_calendar_date(String const& calendar, ISODate iso_date)
{
    auto result = icu_iso_date_to_calendar_date(calendar.bytes().data(), calendar.bytes().size(), iso_date.year, iso_date.month, iso_date.day);

    return CalendarDate {
        .era = {},
        .era_year = {},
        .year = result.year,
        .month = result.month,
        .month_code = String::from_utf8_without_validation({ result.month_code, result.month_code_length }),
        .day = result.day,
        .day_of_week = result.day_of_week,
        .day_of_year = result.day_of_year,
        .week_of_year = {},
        .days_in_week = result.days_in_week,
        .days_in_month = result.days_in_month,
        .days_in_year = result.days_in_year,
        .months_in_year = result.months_in_year,
        .in_leap_year = result.in_leap_year,
    };
}

Optional<ISODate> calendar_date_to_iso_date(String const& calendar, i32 year, u8 month, u8 day)
{
    auto result = icu_calendar_date_to_iso_date(calendar.bytes().data(), calendar.bytes().size(), year, month, day);
    if (!result.has_value)
        return {};

    return ISODate { result.iso_date.year, result.iso_date.month, result.iso_date.day };
}

Optional<ISODate> calendar_month_code_to_iso_date(String const& calendar, i32 year, StringView month_code, u8 day)
{
    auto result = icu_calendar_month_code_to_iso_date(calendar.bytes().data(), calendar.bytes().size(), year, month_code.bytes().data(), month_code.length(), day);
    if (!result.has_value)
        return {};

    return ISODate { result.iso_date.year, result.iso_date.month, result.iso_date.day };
}

u8 calendar_months_in_year(String const& calendar, i32 arithmetic_year)
{
    return icu_calendar_months_in_year(calendar.bytes().data(), calendar.bytes().size(), arithmetic_year);
}

u8 calendar_days_in_month(String const& calendar, i32 arithmetic_year, u8 ordinal_month)
{
    return icu_calendar_days_in_month(calendar.bytes().data(), calendar.bytes().size(), arithmetic_year, ordinal_month);
}

u8 calendar_max_days_in_month_code(String const& calendar, StringView month_code)
{
    return icu_calendar_max_days_in_month_code(calendar.bytes().data(), calendar.bytes().size(), month_code.bytes().data(), month_code.length());
}

bool calendar_year_contains_month_code(String const& calendar, i32 arithmetic_year, StringView month_code)
{
    return icu_year_contains_month_code(calendar.bytes().data(), calendar.bytes().size(), arithmetic_year, month_code.bytes().data(), month_code.length());
}

}
