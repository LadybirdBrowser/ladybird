/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
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

namespace JS::Temporal {

// 12.2.1 Calendar Date Records, https://tc39.es/proposal-temporal/#sec-temporal-calendar-date-records
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

ThrowCompletionOr<String> canonicalize_calendar(VM&, StringView id);
Vector<String> const& available_calendars();
ThrowCompletionOr<CalendarFields> prepare_calendar_fields(VM&, StringView calendar, Object const& fields, CalendarFieldList calendar_field_names, CalendarFieldList non_calendar_field_names, CalendarFieldListOrPartial required_field_names);
ThrowCompletionOr<ISODate> calendar_date_from_fields(VM&, StringView calendar, CalendarFields&, Overflow);
ThrowCompletionOr<ISODate> calendar_year_month_from_fields(VM&, StringView calendar, CalendarFields&, Overflow);
ThrowCompletionOr<ISODate> calendar_month_day_from_fields(VM&, StringView calendar, CalendarFields&, Overflow);
String format_calendar_annotation(StringView id, ShowCalendar);
bool calendar_equals(StringView one, StringView two);
u8 iso_days_in_month(double year, double month);
YearWeek iso_week_of_year(ISODate);
u16 iso_day_of_year(ISODate);
u8 iso_day_of_week(ISODate);
Vector<CalendarField> calendar_field_keys_present(CalendarFields const&);
CalendarFields calendar_merge_fields(StringView calendar, CalendarFields const& fields, CalendarFields const& additional_fields);
ThrowCompletionOr<ISODate> calendar_date_add(VM&, StringView calendar, ISODate, DateDuration const&, Overflow);
DateDuration calendar_date_until(VM&, StringView calendar, ISODate, ISODate, Unit largest_unit);
ThrowCompletionOr<String> to_temporal_calendar_identifier(VM&, Value temporal_calendar_like);
ThrowCompletionOr<String> get_temporal_calendar_identifier_with_iso_default(VM&, Object const& item);
ThrowCompletionOr<ISODate> calendar_date_to_iso(VM&, StringView calendar, CalendarFields const&, Overflow);
ThrowCompletionOr<ISODate> calendar_month_day_to_iso_reference_date(VM&, StringView calendar, CalendarFields const&, Overflow);
CalendarDate calendar_iso_to_date(StringView calendar, ISODate);
Vector<CalendarField> calendar_extra_fields(StringView calendar, CalendarFieldList);
Vector<CalendarField> calendar_field_keys_to_ignore(StringView calendar, ReadonlySpan<CalendarField>);
ThrowCompletionOr<void> calendar_resolve_fields(VM&, StringView calendar, CalendarFields&, DateType);

}
