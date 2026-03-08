/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibUnicode/Calendar.h>

namespace JS::Temporal {

extern String ISO8601_CALENDAR;

// 12.2 Month Codes, https://tc39.es/proposal-temporal/#sec-temporal-month-codes
using MonthCode = Unicode::MonthCode;

// 12.3.1 Calendar Date Records, https://tc39.es/proposal-temporal/#sec-temporal-calendar-date-records
using CalendarDate = Unicode::CalendarDate;

// 14.3 The Year-Week Record Specification Type, https://tc39.es/proposal-temporal/#sec-year-week-record-specification-type
using YearWeek = Unicode::YearWeek;

// https://tc39.es/proposal-temporal/#table-temporal-calendar-fields-record-fields
enum class CalendarField {
    Era,
    EraYear,
    Year,
    Month,
    MonthCode,
    Day,
    Hour,
    Minute,
    Second,
    Millisecond,
    Microsecond,
    Nanosecond,
    Offset,
    TimeZone,
};

// https://tc39.es/proposal-temporal/#table-temporal-calendar-fields-record-fields
struct CalendarFields {
    static CalendarFields unset()
    {
        return {
            .era = {},
            .era_year = {},
            .year = {},
            .month = {},
            .month_code = {},
            .day = {},
            .hour = {},
            .minute = {},
            .second = {},
            .millisecond = {},
            .microsecond = {},
            .nanosecond = {},
            .offset_string = {},
            .time_zone = {},
        };
    }

    Optional<String> era;
    Optional<i32> era_year;
    Optional<i32> year;
    Optional<u32> month;
    Optional<String> month_code;
    Optional<u32> day;
    Optional<u8> hour { 0 };
    Optional<u8> minute { 0 };
    Optional<u8> second { 0 };
    Optional<u16> millisecond { 0 };
    Optional<u16> microsecond { 0 };
    Optional<u16> nanosecond { 0 };
    Optional<String> offset_string;
    Optional<String> time_zone;
};

struct Partial { };
using CalendarFieldList = ReadonlySpan<CalendarField>;
using CalendarFieldListOrPartial = Variant<Partial, CalendarFieldList>;

struct BalancedDate {
    i32 year { 0 };
    u8 month { 0 };
    u8 day { 0 };
};

ThrowCompletionOr<String> canonicalize_calendar(VM&, StringView id);
Vector<String> const& available_calendars();

ThrowCompletionOr<MonthCode> parse_month_code(VM&, Value argument);
ThrowCompletionOr<MonthCode> parse_month_code(VM&, StringView month_code);
MonthCode parse_month_code(StringView month_code);
String create_month_code(u8 month_number, bool is_leap_month);

ThrowCompletionOr<CalendarFields> prepare_calendar_fields(VM&, String const& calendar, Object const& fields, CalendarFieldList calendar_field_names, CalendarFieldList non_calendar_field_names, CalendarFieldListOrPartial required_field_names);
ThrowCompletionOr<ISODate> calendar_date_from_fields(VM&, String const& calendar, CalendarFields&, Overflow);
ThrowCompletionOr<ISODate> calendar_year_month_from_fields(VM&, String const& calendar, CalendarFields&, Overflow);
ThrowCompletionOr<ISODate> calendar_month_day_from_fields(VM&, String const& calendar, CalendarFields&, Overflow);
String format_calendar_annotation(StringView id, ShowCalendar);
bool calendar_equals(StringView one, StringView two);
u8 iso_days_in_month(double year, double month);
YearWeek iso_week_of_year(ISODate);
u16 iso_day_of_year(ISODate);
u8 iso_day_of_week(ISODate);
Vector<CalendarField> calendar_field_keys_present(CalendarFields const&);
CalendarFields calendar_merge_fields(String const& calendar, CalendarFields const& fields, CalendarFields const& additional_fields);
ThrowCompletionOr<ISODate> non_iso_date_add(VM&, String const& calendar, ISODate, DateDuration const&, Overflow);
ThrowCompletionOr<ISODate> calendar_date_add(VM&, String const& calendar, ISODate, DateDuration const&, Overflow);
DateDuration non_iso_date_until(VM&, String const& calendar, ISODate, ISODate, Unit largest_unit);
DateDuration calendar_date_until(VM&, String const& calendar, ISODate, ISODate, Unit largest_unit);
ThrowCompletionOr<String> to_temporal_calendar_identifier(VM&, Value temporal_calendar_like);
ThrowCompletionOr<String> get_temporal_calendar_identifier_with_iso_default(VM&, Object const& item);
ThrowCompletionOr<ISODate> non_iso_calendar_date_to_iso(VM&, String const& calendar, CalendarFields const&, Overflow);
ThrowCompletionOr<ISODate> calendar_date_to_iso(VM&, String const& calendar, CalendarFields const&, Overflow);
ThrowCompletionOr<ISODate> non_iso_month_day_to_iso_reference_date(VM&, String const& calendar, CalendarFields const&, Overflow);
ThrowCompletionOr<ISODate> calendar_month_day_to_iso_reference_date(VM&, String const& calendar, CalendarFields const&, Overflow);
CalendarDate non_iso_calendar_iso_to_date(String const& calendar, ISODate);
CalendarDate calendar_iso_to_date(String const& calendar, ISODate);
Vector<CalendarField> calendar_extra_fields(String const& calendar, CalendarFieldList);
Vector<CalendarField> non_iso_field_keys_to_ignore(String const& calendar, ReadonlySpan<CalendarField>);
Vector<CalendarField> calendar_field_keys_to_ignore(String const& calendar, ReadonlySpan<CalendarField>);
ThrowCompletionOr<void> non_iso_resolve_fields(VM&, String const& calendar, CalendarFields&, DateType);
ThrowCompletionOr<void> calendar_resolve_fields(VM&, String const& calendar, CalendarFields&, DateType);

bool calendar_supports_era(String const& calendar);
Optional<StringView> canonicalize_era_in_calendar(String const& calendar, StringView era);
bool calendar_has_mid_year_eras(String const& calendar);
bool is_valid_month_code_for_calendar(String const& calendar, StringView month_code);
bool year_contains_month_code(String const& calendar, i32 arithmetic_year, StringView month_code);
ThrowCompletionOr<String> constrain_month_code(VM&, String const& calendar, i32 arithmetic_year, String const& month_code, Overflow overflow);
u8 month_code_to_ordinal(String const& calendar, i32 arithmetic_year, StringView month_code);
u8 calendar_days_in_month(String const& calendar, i32 arithmetic_year, u8 ordinal_month);
i32 calendar_date_arithmetic_year_for_era_year(String const& calendar, StringView era, i32 era_year);
ThrowCompletionOr<ISODate> calendar_integers_to_iso(VM&, String const& calendar, i32 arithmetic_year, u8 ordinal_month, u8 day);
u8 calendar_months_in_year(String const& calendar, i32 arithmetic_year);
BalancedDate balance_non_iso_date(String const& calendar, i32 arithmetic_year, i32 ordinal_month, i32 day);
bool non_iso_date_surpasses(VM&, String const& calendar, i8 sign, CalendarDate const& from_calendar_date, CalendarDate const& to_calendar_date, double years, double months, double weeks, double days);

}
