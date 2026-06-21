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
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibUnicode/Calendar.h>

namespace JS::Temporal {

extern Utf16String ISO8601_CALENDAR;

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

    Optional<Utf16String> era;
    Optional<i32> era_year;
    Optional<i32> year;
    Optional<u32> month;
    Optional<Utf16String> month_code;
    Optional<u32> day;
    Optional<u8> hour { 0 };
    Optional<u8> minute { 0 };
    Optional<u8> second { 0 };
    Optional<u16> millisecond { 0 };
    Optional<u16> microsecond { 0 };
    Optional<u16> nanosecond { 0 };
    Optional<Utf16String> offset_string;
    Optional<Utf16String> time_zone;
};

struct Partial { };
using CalendarFieldList = ReadonlySpan<CalendarField>;
using CalendarFieldListOrPartial = Variant<Partial, CalendarFieldList>;

struct BalancedDate {
    i32 year { 0 };
    u8 month { 0 };
    u8 day { 0 };
};

ThrowCompletionOr<Utf16String> canonicalize_calendar(VM&, Utf16View id);
Vector<Utf16String> const& available_calendars();

ThrowCompletionOr<MonthCode> parse_month_code(VM&, Value argument);
ThrowCompletionOr<MonthCode> parse_month_code(VM&, Utf16View month_code);

ThrowCompletionOr<CalendarFields> prepare_calendar_fields(VM&, Utf16View calendar, Object const& fields, CalendarFieldList calendar_field_names, CalendarFieldList non_calendar_field_names, CalendarFieldListOrPartial required_field_names);
ThrowCompletionOr<ISODate> calendar_date_from_fields(VM&, Utf16View calendar, CalendarFields&, Overflow);
ThrowCompletionOr<ISODate> calendar_year_month_from_fields(VM&, Utf16View calendar, CalendarFields&, Overflow);
ThrowCompletionOr<ISODate> calendar_month_day_from_fields(VM&, Utf16View calendar, CalendarFields&, Overflow);
Utf16String format_calendar_annotation(Utf16View id, ShowCalendar);
bool calendar_equals(Utf16View one, Utf16View two);
u8 iso_days_in_month(double year, double month);
YearWeek iso_week_of_year(ISODate);
u16 iso_day_of_year(ISODate);
u8 iso_day_of_week(ISODate);
Vector<CalendarField> calendar_field_keys_present(CalendarFields const&);
CalendarFields calendar_merge_fields(Utf16View calendar, CalendarFields const& fields, CalendarFields const& additional_fields);
ThrowCompletionOr<ISODate> non_iso_date_add(VM&, Utf16View calendar, ISODate, DateDuration const&, Overflow);
ThrowCompletionOr<ISODate> calendar_date_add(VM&, Utf16View calendar, ISODate, DateDuration const&, Overflow);
DateDuration non_iso_date_until(VM&, Utf16View calendar, ISODate, ISODate, Unit largest_unit);
DateDuration calendar_date_until(VM&, Utf16View calendar, ISODate, ISODate, Unit largest_unit);
ThrowCompletionOr<Utf16String> to_temporal_calendar_identifier(VM&, Value temporal_calendar_like);
ThrowCompletionOr<Utf16String> get_temporal_calendar_identifier_with_iso_default(VM&, Object const& item);
ThrowCompletionOr<ISODate> non_iso_calendar_date_to_iso(VM&, Utf16View calendar, CalendarFields const&, Overflow);
ThrowCompletionOr<ISODate> calendar_date_to_iso(VM&, Utf16View calendar, CalendarFields const&, Overflow);
ThrowCompletionOr<ISODate> non_iso_month_day_to_iso_reference_date(VM&, Utf16View calendar, CalendarFields const&, Overflow);
ThrowCompletionOr<ISODate> calendar_month_day_to_iso_reference_date(VM&, Utf16View calendar, CalendarFields const&, Overflow);
CalendarDate non_iso_calendar_iso_to_date(Utf16View calendar, ISODate);
CalendarDate calendar_iso_to_date(Utf16View calendar, ISODate);
Vector<CalendarField> calendar_extra_fields(Utf16View calendar, CalendarFieldList);
Vector<CalendarField> non_iso_field_keys_to_ignore(Utf16View calendar, ReadonlySpan<CalendarField>);
Vector<CalendarField> calendar_field_keys_to_ignore(Utf16View calendar, ReadonlySpan<CalendarField>);
ThrowCompletionOr<void> non_iso_resolve_fields(VM&, Utf16View calendar, CalendarFields&, DateType);
ThrowCompletionOr<void> calendar_resolve_fields(VM&, Utf16View calendar, CalendarFields&, DateType);

bool calendar_supports_era(Utf16View calendar);
Optional<Utf16View> canonicalize_era_in_calendar(Utf16View calendar, Utf16View era);
bool calendar_has_mid_year_eras(Utf16View calendar);
bool is_valid_month_code_for_calendar(Utf16View calendar, Utf16View month_code);
bool year_contains_month_code(Utf16View calendar, i32 arithmetic_year, Utf16View month_code);
ThrowCompletionOr<Utf16String> constrain_month_code(VM&, Utf16View calendar, i32 arithmetic_year, Utf16String const& month_code, Overflow overflow);
u8 month_code_to_ordinal(Utf16View calendar, i32 arithmetic_year, Utf16View month_code);
u8 calendar_days_in_month(Utf16View calendar, i32 arithmetic_year, u8 ordinal_month);
i32 calendar_date_arithmetic_year_for_era_year(Utf16View calendar, Utf16View era, i32 era_year);
ThrowCompletionOr<ISODate> calendar_integers_to_iso(VM&, Utf16View calendar, i32 arithmetic_year, u8 ordinal_month, u8 day);
u8 calendar_months_in_year(Utf16View calendar, i32 arithmetic_year);
BalancedDate balance_non_iso_date(Utf16View calendar, i32 arithmetic_year, i32 ordinal_month, i32 day);
bool non_iso_date_surpasses(VM&, Utf16View calendar, i8 sign, CalendarDate const& from_calendar_date, CalendarDate const& to_calendar_date, double years, double months, double weeks, double days);

}
