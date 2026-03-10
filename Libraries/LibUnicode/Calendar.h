/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>

namespace Unicode {

// 3.5.1 ISO Date Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-records
struct ISODate {
    i32 year { 0 };
    u8 month { 0 };
    u8 day { 0 };
};

// 14.3 The Year-Week Record Specification Type, https://tc39.es/proposal-temporal/#sec-year-week-record-specification-type
struct YearWeek {
    Optional<u8> week;
    Optional<i32> year;
};

// 12.3.1 Calendar Date Records, https://tc39.es/proposal-temporal/#sec-temporal-calendar-date-records
struct CalendarDate {
    Optional<String> era;
    Optional<i32> era_year;
    i32 year { 0 };
    u8 month { 0 };
    String month_code;
    u8 day { 0 };
    u8 day_of_week { 0 };
    u16 day_of_year { 0 };
    YearWeek week_of_year;
    u8 days_in_week { 0 };
    u8 days_in_month { 0 };
    u16 days_in_year { 0 };
    u8 months_in_year { 0 };
    bool in_leap_year { false };
};

CalendarDate iso_date_to_calendar_date(String const& calendar, ISODate);
Optional<ISODate> calendar_date_to_iso_date(String const& calendar, i32 year, u8 month, u8 day);
Optional<ISODate> calendar_month_code_to_iso_date(String const& calendar, i32 year, StringView month_code, u8 day);

u8 calendar_months_in_year(String const& calendar, i32 arithmetic_year);
u8 calendar_days_in_month(String const& calendar, i32 arithmetic_year, u8 ordinal_month);
u8 calendar_max_days_in_month_code(String const& calendar, StringView month_code);
bool calendar_year_contains_month_code(String const& calendar, i32 arithmetic_year, StringView month_code);

}
