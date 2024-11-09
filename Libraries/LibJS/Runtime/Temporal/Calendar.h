/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>
#include <LibJS/Runtime/Value.h>

namespace JS::Temporal {

class Calendar final : public Object {
    JS_OBJECT(Calendar, Object);
    JS_DECLARE_ALLOCATOR(Calendar);

public:
    virtual ~Calendar() override = default;

    [[nodiscard]] String const& identifier() const { return m_identifier; }

private:
    Calendar(String identifier, Object& prototype);

    // 12.5 Properties of Temporal.Calendar Instances, https://tc39.es/proposal-temporal/#sec-properties-of-temporal-calendar-instances
    String m_identifier; // [[Identifier]]
};

// 14.2 The Year-Week Record Specification Type, https://tc39.es/proposal-temporal/#sec-year-week-record-specification-type
struct YearWeekRecord {
    u8 week { 0 };
    i32 year { 0 };
};

bool is_builtin_calendar(StringView identifier);
ReadonlySpan<StringView> available_calendars();
ThrowCompletionOr<Calendar*> create_temporal_calendar(VM&, String const& identifier, FunctionObject const* new_target = nullptr);
ThrowCompletionOr<Calendar*> get_builtin_calendar(VM&, String const& identifier);
Calendar* get_iso8601_calendar(VM&);
ThrowCompletionOr<Vector<String>> calendar_fields(VM&, Object& calendar, Vector<StringView> const& field_names);
ThrowCompletionOr<Object*> calendar_merge_fields(VM&, Object& calendar, Object& fields, Object& additional_fields);
ThrowCompletionOr<PlainDate*> calendar_date_add(VM&, Object& calendar, Value date, Duration&, Object* options = nullptr, FunctionObject* date_add = nullptr);
ThrowCompletionOr<NonnullGCPtr<Duration>> calendar_date_until(VM&, CalendarMethods const&, Value one, Value two, Object const& options);
ThrowCompletionOr<double> calendar_year(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<double> calendar_month(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<String> calendar_month_code(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<double> calendar_day(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<double> calendar_day_of_week(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<double> calendar_day_of_year(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<double> calendar_week_of_year(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<double> calendar_year_of_week(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<double> calendar_days_in_week(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<double> calendar_days_in_month(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<double> calendar_days_in_year(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<double> calendar_months_in_year(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<Value> calendar_in_leap_year(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<Value> calendar_era(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<Value> calendar_era_year(VM&, Object& calendar, Object& date_like);
ThrowCompletionOr<Object*> to_temporal_calendar(VM&, Value);
ThrowCompletionOr<Object*> to_temporal_calendar_with_iso_default(VM&, Value);
ThrowCompletionOr<Object*> get_temporal_calendar_with_iso_default(VM&, Object&);
ThrowCompletionOr<PlainDate*> calendar_date_from_fields(VM&, Object& calendar, Object const& fields, Object const* options = nullptr);
ThrowCompletionOr<PlainYearMonth*> calendar_year_month_from_fields(VM&, Object& calendar, Object const& fields, Object const* options = nullptr);
ThrowCompletionOr<PlainMonthDay*> calendar_month_day_from_fields(VM&, Object& calendar, Object const& fields, Object const* options = nullptr);
ThrowCompletionOr<String> maybe_format_calendar_annotation(VM&, Object const* calendar_object, StringView show_calendar);
ThrowCompletionOr<String> format_calendar_annotation(VM&, StringView id, StringView show_calendar);
ThrowCompletionOr<bool> calendar_equals(VM&, Object& one, Object& two);
ThrowCompletionOr<Object*> consolidate_calendars(VM&, Object& one, Object& two);
u8 iso_days_in_month(i32 year, u8 month);
YearWeekRecord to_iso_week_of_year(i32 year, u8 month, u8 day);
ThrowCompletionOr<String> iso_month_code(VM&, u8 month);
ThrowCompletionOr<double> resolve_iso_month(VM&, Object const& fields);
ThrowCompletionOr<ISODateRecord> iso_date_from_fields(VM&, Object const& fields, Object const& options);
ThrowCompletionOr<ISOYearMonth> iso_year_month_from_fields(VM&, Object const& fields, Object const& options);
ThrowCompletionOr<ISOMonthDay> iso_month_day_from_fields(VM&, Object const& fields, Object const& options);
ThrowCompletionOr<Object*> default_merge_calendar_fields(VM&, Object const& fields, Object const& additional_fields);
u16 to_iso_day_of_year(i32 year, u8 month, u8 day);
u8 to_iso_day_of_week(i32 year, u8 month, u8 day);

// https://tc39.es/proposal-temporal/#table-temporal-calendar-methods-record-fields
struct CalendarMethods {
    // The calendar object, or a string indicating a built-in time zone.
    Variant<String, NonnullGCPtr<Object>> receiver; // [[Reciever]]

    // The calendar's dateAdd method. For a built-in calendar this is always %Temporal.Calendar.prototype.dateAdd%.
    GCPtr<FunctionObject> date_add; // [[DateAdd]]

    // The calendar's dateFromFields method. For a built-in calendar this is always %Temporal.Calendar.prototype.dateFromFields%.
    GCPtr<FunctionObject> date_from_fields; // [[DateFromFields]]

    // The calendar's dateUntil method. For a built-in calendar this is always %Temporal.Calendar.prototype.dateUntil%.
    GCPtr<FunctionObject> date_until; // [[DateUntil]]

    // The calendar's day method. For a built-in calendar this is always %Temporal.Calendar.prototype.day%.
    GCPtr<FunctionObject> day; // [[Day]]

    // The calendar's fields method. For a built-in calendar this is always %Temporal.Calendar.prototype.fields%.
    GCPtr<FunctionObject> fields; // [[Fields]]

    // The calendar's mergeFields method. For a built-in calendar this is always %Temporal.Calendar.prototype.mergeFields%.
    GCPtr<FunctionObject> merge_fields; // [[MergeFields]]

    // The calendar's monthDayFromFields method. For a built-in calendar this is always %Temporal.Calendar.prototype.monthDayFromFields%.
    GCPtr<FunctionObject> month_day_from_fields; // [[MonthDayFromFields]]

    // The calendar's yearMonthFromFields method. For a built-in calendar this is always %Temporal.Calendar.prototype.yearMonthFromFields%.
    GCPtr<FunctionObject> year_month_from_fields; // [[YearMonthFromFields]]
};

#define JS_ENUMERATE_CALENDAR_METHODS                                             \
    __JS_ENUMERATE(DateAdd, dateAdd, date_add)                                    \
    __JS_ENUMERATE(DateFromFields, dateFromFields, date_from_fields)              \
    __JS_ENUMERATE(DateUntil, dateUntil, date_until)                              \
    __JS_ENUMERATE(Day, day, day)                                                 \
    __JS_ENUMERATE(Fields, fields, fields)                                        \
    __JS_ENUMERATE(MergeFields, mergeFields, merge_fields)                        \
    __JS_ENUMERATE(MonthDayFromFields, monthDayFromFields, month_day_from_fields) \
    __JS_ENUMERATE(YearMonthFromFields, yearMonthFromFields, year_month_from_fields)

enum class CalendarMethod {
#define __JS_ENUMERATE(PascalName, camelName, snake_name) \
    PascalName,
    JS_ENUMERATE_CALENDAR_METHODS
#undef __JS_ENUMERATE
};

ThrowCompletionOr<void> calendar_methods_record_lookup(VM&, CalendarMethods&, CalendarMethod);
ThrowCompletionOr<CalendarMethods> create_calendar_methods_record(VM&, Variant<String, NonnullGCPtr<Object>> calendar, ReadonlySpan<CalendarMethod>);
ThrowCompletionOr<Optional<CalendarMethods>> create_calendar_methods_record_from_relative_to(VM&, GCPtr<PlainDate>, GCPtr<ZonedDateTime>, ReadonlySpan<CalendarMethod>);
bool calendar_methods_record_has_looked_up(CalendarMethods const&, CalendarMethod);
bool calendar_methods_record_is_builtin(CalendarMethods const&);
ThrowCompletionOr<Value> calendar_methods_record_call(VM&, CalendarMethods const&, CalendarMethod, ReadonlySpan<Value> arguments);

}
