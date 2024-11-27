/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Intl/DateTimeFormat.h>
#include <LibJS/Runtime/Intl/DateTimeFormatConstructor.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTimePrototype.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainDateTimePrototype);

// 5.3 Properties of the Temporal.PlainDateTime Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plaindatetime-prototype-object
PlainDateTimePrototype::PlainDateTimePrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void PlainDateTimePrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 5.3.2 Temporal.PlainDateTime.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.PlainDateTime"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.calendarId, calendar_id_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.era, era_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.eraYear, era_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.year, year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.month, month_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.monthCode, month_code_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.day, day_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.hour, hour_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.minute, minute_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.second, second_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.millisecond, millisecond_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.microsecond, microsecond_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.nanosecond, nanosecond_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.dayOfWeek, day_of_week_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.dayOfYear, day_of_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.weekOfYear, week_of_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.yearOfWeek, year_of_week_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.daysInWeek, days_in_week_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.daysInMonth, days_in_month_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.daysInYear, days_in_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.monthsInYear, months_in_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.inLeapYear, in_leap_year_getter, {}, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.with, with, 1, attr);
    define_native_function(realm, vm.names.withPlainTime, with_plain_time, 0, attr);
    define_native_function(realm, vm.names.withCalendar, with_calendar, 1, attr);
    define_native_function(realm, vm.names.add, add, 1, attr);
    define_native_function(realm, vm.names.subtract, subtract, 1, attr);
    define_native_function(realm, vm.names.until, until, 1, attr);
    define_native_function(realm, vm.names.since, since, 1, attr);
    define_native_function(realm, vm.names.round, round, 1, attr);
    define_native_function(realm, vm.names.equals, equals, 1, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);
    define_native_function(realm, vm.names.valueOf, value_of, 0, attr);
    define_native_function(realm, vm.names.toZonedDateTime, to_zoned_date_time, 1, attr);
    define_native_function(realm, vm.names.toPlainDate, to_plain_date, 0, attr);
    define_native_function(realm, vm.names.toPlainTime, to_plain_time, 0, attr);
}

// 5.3.3 get Temporal.PlainDateTime.prototype.calendarId, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.calendarid
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::calendar_id_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return dateTime.[[Calendar]].
    return PrimitiveString::create(vm, date_time->calendar());
}

// 5.3.4 get Temporal.PlainDateTime.prototype.era, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.era
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::era_getter)
{
    // 1. Let plainDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[Era]].
    auto result = calendar_iso_to_date(date_time->calendar(), date_time->iso_date_time().iso_date).era;

    if (!result.has_value())
        return js_undefined();

    return PrimitiveString::create(vm, result.release_value());
}

// 5.3.5 get Temporal.PlainDateTime.prototype.eraYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.erayear
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::era_year_getter)
{
    // 1. Let plainDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let result be CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[EraYear]].
    auto result = calendar_iso_to_date(date_time->calendar(), date_time->iso_date_time().iso_date).era_year;

    // 4. If result is undefined, return undefined.
    if (!result.has_value())
        return js_undefined();

    // 5. Return 𝔽(result).
    return *result;
}

// 5.3.6 get Temporal.PlainDateTime.prototype.year, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.year
// 5.3.7 get Temporal.PlainDateTime.prototype.month, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.month
// 5.3.9 get Temporal.PlainDateTime.prototype.day, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.monthcode
// 5.3.16 get Temporal.PlainDateTime.prototype.dayOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.dayofweek
// 5.3.17 get Temporal.PlainDateTime.prototype.dayOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.dayofyear
// 5.3.20 get Temporal.PlainDateTime.prototype.daysInWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.daysinweek
// 5.3.21 get Temporal.PlainDateTime.prototype.daysInMonth, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.daysinmonth
// 5.3.22 get Temporal.PlainDateTime.prototype.daysInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.daysinyear
// 5.3.23 get Temporal.PlainDateTime.prototype.monthsInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.monthsinyear
// 5.3.24 get Temporal.PlainDateTime.prototype.inLeapYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.inleapyear
#define JS_ENUMERATE_PLAIN_DATE_TIME_SIMPLE_DATE_FIELDS \
    __JS_ENUMERATE(year)                                \
    __JS_ENUMERATE(month)                               \
    __JS_ENUMERATE(day)                                 \
    __JS_ENUMERATE(day_of_week)                         \
    __JS_ENUMERATE(day_of_year)                         \
    __JS_ENUMERATE(days_in_week)                        \
    __JS_ENUMERATE(days_in_month)                       \
    __JS_ENUMERATE(days_in_year)                        \
    __JS_ENUMERATE(months_in_year)                      \
    __JS_ENUMERATE(in_leap_year)

#define __JS_ENUMERATE(field)                                                                                             \
    JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::field##_getter)                                                     \
    {                                                                                                                     \
        /* 1. Let dateTime be the this value. */                                                                          \
        /* 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]). */                                \
        auto date_time = TRY(typed_this_object(vm));                                                                      \
                                                                                                                          \
        /* 3. Return 𝔽(CalendarISOToDate(dateTime.[[Calendar]], dateTime.[[ISODateTime]].[[ISODate]]).[[<field>]]). */ \
        return calendar_iso_to_date(date_time->calendar(), date_time->iso_date_time().iso_date).field;                    \
    }
JS_ENUMERATE_PLAIN_DATE_TIME_SIMPLE_DATE_FIELDS
#undef __JS_ENUMERATE

// 5.3.8 get Temporal.PlainDateTime.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.monthcode
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::month_code_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return CalendarISOToDate(dateTime.[[Calendar]], dateTime.[[ISODateTime]].[[ISODate]]).[[MonthCode]].
    auto month_code = calendar_iso_to_date(date_time->calendar(), date_time->iso_date_time().iso_date).month_code;
    return PrimitiveString::create(vm, move(month_code));
}

// 5.3.10 get Temporal.PlainDateTime.prototype.hour, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.hour
// 5.3.11 get Temporal.PlainDateTime.prototype.minute, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.minute
// 5.3.12 get Temporal.PlainDateTime.prototype.second, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.second
// 5.3.13 get Temporal.PlainDateTime.prototype.millisecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.millisecond
// 5.3.14 get Temporal.PlainDateTime.prototype.microsecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.microsecond
// 5.3.15 get Temporal.PlainDateTime.prototype.nanosecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.nanosecond
#define JS_ENUMERATE_PLAIN_DATE_TIME_TIME_FIELDS \
    __JS_ENUMERATE(hour)                         \
    __JS_ENUMERATE(minute)                       \
    __JS_ENUMERATE(second)                       \
    __JS_ENUMERATE(millisecond)                  \
    __JS_ENUMERATE(microsecond)                  \
    __JS_ENUMERATE(nanosecond)

#define __JS_ENUMERATE(field)                                                              \
    JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::field##_getter)                      \
    {                                                                                      \
        /* 1. Let dateTime be the this value. */                                           \
        /* 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]). */ \
        auto date_time = TRY(typed_this_object(vm));                                       \
                                                                                           \
        /* 3. Return 𝔽(dateTime.[[ISODateTime]].[[Time]].[[<field>]]). */               \
        return date_time->iso_date_time().time.field;                                      \
    }
JS_ENUMERATE_PLAIN_DATE_TIME_TIME_FIELDS
#undef __JS_ENUMERATE

// 5.3.18 get Temporal.PlainDateTime.prototype.weekOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.weekofyear
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::week_of_year_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let result be CalendarISOToDate(dateTime.[[Calendar]], dateTime.[[ISODateTime]].[[ISODate]]).[[WeekOfYear]].[[Week]].
    auto result = calendar_iso_to_date(date_time->calendar(), date_time->iso_date_time().iso_date).week_of_year.week;

    // 4. If result is undefined, return undefined.
    if (!result.has_value())
        return js_undefined();

    // 5. Return 𝔽(result).
    return *result;
}

// 5.3.19 get Temporal.PlainDateTime.prototype.yearOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.yearofweek
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::year_of_week_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let result be CalendarISOToDate(dateTime.[[Calendar]], dateTime.[[ISODateTime]].[[ISODate]]).[[WeekOfYear]].[[Year]].
    auto result = calendar_iso_to_date(date_time->calendar(), date_time->iso_date_time().iso_date).week_of_year.year;

    // 4. If result is undefined, return undefined.
    if (!result.has_value())
        return js_undefined();

    // 5. Return 𝔽(result).
    return *result;
}

// 5.3.25 Temporal.PlainDateTime.prototype.with ( temporalDateTimeLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.with
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::with)
{
    auto temporal_date_time_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. If ? IsPartialTemporalObject(temporalDateTimeLike) is false, throw a TypeError exception.
    if (!TRY(is_partial_temporal_object(vm, temporal_date_time_like)))
        return vm.throw_completion<TypeError>(ErrorType::TemporalObjectMustBePartialTemporalObject);

    // 4. Let calendar be dateTime.[[Calendar]].
    auto const& calendar = date_time->calendar();

    // 5. Let fields be ISODateToFields(calendar, dateTime.[[ISODateTime]].[[ISODate]], DATE).
    auto fields = iso_date_to_fields(calendar, date_time->iso_date_time().iso_date, DateType::Date);

    // 6. Set fields.[[Hour]] to dateTime.[[ISODateTime]].[[Time]].[[Hour]].
    fields.hour = date_time->iso_date_time().time.hour;

    // 7. Set fields.[[Minute]] to dateTime.[[ISODateTime]].[[Time]].[[Minute]].
    fields.minute = date_time->iso_date_time().time.minute;

    // 8. Set fields.[[Second]] to dateTime.[[ISODateTime]].[[Time]].[[Second]].
    fields.second = date_time->iso_date_time().time.second;

    // 9. Set fields.[[Millisecond]] to dateTime.[[ISODateTime]].[[Time]].[[Millisecond]].
    fields.millisecond = date_time->iso_date_time().time.millisecond;

    // 10. Set fields.[[Microsecond]] to dateTime.[[ISODateTime]].[[Time]].[[Microsecond]].
    fields.microsecond = date_time->iso_date_time().time.microsecond;

    // 11. Set fields.[[Nanosecond]] to dateTime.[[ISODateTime]].[[Time]].[[Nanosecond]].
    fields.nanosecond = date_time->iso_date_time().time.nanosecond;

    // 12. Let partialDateTime be ? PrepareCalendarFields(calendar, temporalDateTimeLike, « YEAR, MONTH, MONTH-CODE, DAY », « HOUR, MINUTE, SECOND, MILLISECOND, MICROSECOND, NANOSECOND », PARTIAL).
    static constexpr auto calendar_field_names = to_array({ CalendarField::Year, CalendarField::Month, CalendarField::MonthCode, CalendarField::Day });
    static constexpr auto non_calendar_field_names = to_array({ CalendarField::Hour, CalendarField::Minute, CalendarField::Second, CalendarField::Millisecond, CalendarField::Microsecond, CalendarField::Nanosecond });
    auto partial_date_time = TRY(prepare_calendar_fields(vm, calendar, temporal_date_time_like.as_object(), calendar_field_names, non_calendar_field_names, Partial {}));

    // 13. Set fields to CalendarMergeFields(calendar, fields, partialDateTime).
    fields = calendar_merge_fields(calendar, fields, partial_date_time);

    // 14. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 15. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
    auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

    // 16. Let result be ? InterpretTemporalDateTimeFields(calendar, fields, overflow).
    auto result = TRY(interpret_temporal_date_time_fields(vm, calendar, fields, overflow));

    // 17. Return ? CreateTemporalDateTime(result, calendar).
    return MUST(create_temporal_date_time(vm, result, calendar));
}

// 5.3.26 Temporal.PlainDateTime.prototype.withPlainTime ( [ plainTimeLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.withplaintime
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::with_plain_time)
{
    auto plain_time_like = vm.argument(0);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let time be ? ToTimeRecordOrMidnight(plainTimeLike).
    auto time = TRY(to_time_record_or_midnight(vm, plain_time_like));

    // 4. Let isoDateTime be CombineISODateAndTimeRecord(dateTime.[[ISODateTime]].[[ISODate]], time).
    auto iso_date_time = combine_iso_date_and_time_record(date_time->iso_date_time().iso_date, time);

    // 5. Return ? CreateTemporalDateTime(isoDateTime, dateTime.[[Calendar]]).
    return TRY(create_temporal_date_time(vm, iso_date_time, date_time->calendar()));
}

// 5.3.27 Temporal.PlainDateTime.prototype.withCalendar ( calendarLike ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.withcalendar
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::with_calendar)
{
    auto calendar_like = vm.argument(0);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be ? ToTemporalCalendarIdentifier(calendarLike).
    auto calendar = TRY(to_temporal_calendar_identifier(vm, calendar_like));

    // 4. Return ! CreateTemporalDateTime(dateTime.[[ISODateTime]], calendar).
    return TRY(create_temporal_date_time(vm, date_time->iso_date_time(), calendar));
}

// 5.3.28 Temporal.PlainDateTime.prototype.add ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.add
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::add)
{
    auto temporal_duration_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToDateTime(ADD, dateTime, temporalDurationLike, options).
    return TRY(add_duration_to_date_time(vm, ArithmeticOperation::Add, date_time, temporal_duration_like, options));
}

// 5.3.29 Temporal.PlainDateTime.prototype.subtract ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.add
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::subtract)
{
    auto temporal_duration_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToDateTime(SUBTRACT, dateTime, temporalDurationLike, options).
    return TRY(add_duration_to_date_time(vm, ArithmeticOperation::Subtract, date_time, temporal_duration_like, options));
}

// 5.3.30 Temporal.PlainDateTime.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.until
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::until)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalPlainDateTime(UNTIL, dateTime, other, options).
    return TRY(difference_temporal_plain_date_time(vm, DurationOperation::Until, date_time, other, options));
}

// 5.3.31 Temporal.PlainDateTime.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.since
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::since)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalPlainDateTime(SINCE, dateTime, other, options).
    return TRY(difference_temporal_plain_date_time(vm, DurationOperation::Since, date_time, other, options));
}

// 5.3.32 Temporal.PlainDateTime.prototype.round ( roundTo ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.round
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::round)
{
    auto& realm = *vm.current_realm();

    auto round_to_value = vm.argument(0);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. If roundTo is undefined, then
    if (round_to_value.is_undefined()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::TemporalMissingOptionsObject);
    }

    GC::Ptr<Object> round_to;

    // 4. If roundTo is a String, then
    if (round_to_value.is_string()) {
        // a. Let paramString be roundTo.
        auto param_string = round_to_value;

        // b. Set roundTo to OrdinaryObjectCreate(null).
        round_to = Object::create(realm, nullptr);

        // c. Perform ! CreateDataPropertyOrThrow(roundTo, "smallestUnit", paramString).
        MUST(round_to->create_data_property_or_throw(vm.names.smallestUnit, param_string));
    }
    // 5. Else,
    else {
        // a. Set roundTo to ? GetOptionsObject(roundTo).
        round_to = TRY(get_options_object(vm, round_to_value));
    }

    // 6. NOTE: The following steps read options and perform independent validation in alphabetical order
    //    (GetRoundingIncrementOption reads "roundingIncrement" and GetRoundingModeOption reads "roundingMode").

    // 7. Let roundingIncrement be ? GetRoundingIncrementOption(roundTo).
    auto rounding_increment = TRY(get_rounding_increment_option(vm, *round_to));

    // 8. Let roundingMode be ? GetRoundingModeOption(roundTo, HALF-EXPAND).
    auto rounding_mode = TRY(get_rounding_mode_option(vm, *round_to, RoundingMode::HalfExpand));

    // 9. Let smallestUnit be ? GetTemporalUnitValuedOption(roundTo, "smallestUnit", TIME, REQUIRED, « DAY »).
    auto smallest_unit = TRY(get_temporal_unit_valued_option(vm, *round_to, vm.names.smallestUnit, UnitGroup::Time, Required {}, { { Unit::Day } }));
    auto smallest_unit_value = smallest_unit.get<Unit>();

    RoundingIncrement maximum { 0 };
    auto inclusive = false;

    // 10. If smallestUnit is DAY, then
    if (smallest_unit_value == Unit::Day) {
        // a. Let maximum be 1.
        maximum = 1;

        // b. Let inclusive be true.
        inclusive = true;
    }
    // 11. Else,
    else {
        // a. Let maximum be MaximumTemporalDurationRoundingIncrement(smallestUnit).
        maximum = maximum_temporal_duration_rounding_increment(smallest_unit_value);

        // b. Assert: maximum is not UNSET.
        VERIFY(!maximum.has<Unset>());

        // c. Let inclusive be false.
        inclusive = false;
    }

    // 12. Perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, inclusive).
    TRY(validate_temporal_rounding_increment(vm, rounding_increment, maximum.get<u64>(), inclusive));

    // 13. If smallestUnit is NANOSECOND and roundingIncrement = 1, then
    if (smallest_unit_value == Unit::Nanosecond && rounding_increment == 1) {
        // a. Return ! CreateTemporalDateTime(dateTime.[[ISODateTime]], dateTime.[[Calendar]]).
        return MUST(create_temporal_date_time(vm, date_time->iso_date_time(), date_time->calendar()));
    }

    // 14. Let result be RoundISODateTime(dateTime.[[ISODateTime]], roundingIncrement, smallestUnit, roundingMode).
    auto result = round_iso_date_time(date_time->iso_date_time(), rounding_increment, smallest_unit_value, rounding_mode);

    // 15. Return ? CreateTemporalDateTime(result, dateTime.[[Calendar]]).
    return TRY(create_temporal_date_time(vm, result, date_time->calendar()));
}

// 5.3.33 Temporal.PlainDateTime.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.equals
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::equals)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Set other to ? ToTemporalDateTime(other).
    auto other = TRY(to_temporal_date_time(vm, vm.argument(0)));

    // 4. If CompareISODateTime(dateTime.[[ISODateTime]], other.[[ISODateTime]]) ≠ 0, return false.
    if (compare_iso_date_time(date_time->iso_date_time(), other->iso_date_time()) != 0)
        return false;

    // 5. Return CalendarEquals(dateTime.[[Calendar]], other.[[Calendar]]).
    return calendar_equals(date_time->calendar(), other->calendar());
}

// 5.3.34 Temporal.PlainDateTime.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_string)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, vm.argument(0)));

    // 4. NOTE: The following steps read options and perform independent validation in alphabetical order
    //    (GetTemporalShowCalendarNameOption reads "calendarName", GetTemporalFractionalSecondDigitsOption reads
    //    "fractionalSecondDigits", and GetRoundingModeOption reads "roundingMode").

    // 5. Let showCalendar be ? GetTemporalShowCalendarNameOption(resolvedOptions).
    auto show_calendar = TRY(get_temporal_show_calendar_name_option(vm, resolved_options));

    // 6. Let digits be ? GetTemporalFractionalSecondDigitsOption(resolvedOptions).
    auto digits = TRY(get_temporal_fractional_second_digits_option(vm, resolved_options));

    // 7. Let roundingMode be ? GetRoundingModeOption(resolvedOptions, TRUNC).
    auto rounding_mode = TRY(get_rounding_mode_option(vm, resolved_options, RoundingMode::Trunc));

    // 8. Let smallestUnit be ? GetTemporalUnitValuedOption(resolvedOptions, "smallestUnit", TIME, UNSET).
    auto smallest_unit = TRY(get_temporal_unit_valued_option(vm, resolved_options, vm.names.smallestUnit, UnitGroup::Time, Unset {}));

    // 9. If smallestUnit is HOUR, throw a RangeError exception.
    if (auto const* unit = smallest_unit.get_pointer<Unit>(); unit && *unit == Unit::Hour)
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, temporal_unit_to_string(*unit), vm.names.smallestUnit);

    // 10. Let precision be ToSecondsStringPrecisionRecord(smallestUnit, digits).
    auto precision = to_seconds_string_precision_record(smallest_unit, digits);

    // 11. Let result be RoundISODateTime(dateTime.[[ISODateTime]], precision.[[Increment]], precision.[[Unit]], roundingMode).
    auto result = round_iso_date_time(date_time->iso_date_time(), precision.increment, precision.unit, rounding_mode);

    // 12. If ISODateTimeWithinLimits(result) is false, throw a RangeError exception.
    if (!iso_date_time_within_limits(result))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDateTime);

    // 13. Return ISODateTimeToString(result, dateTime.[[Calendar]], precision.[[Precision]], showCalendar).
    return PrimitiveString::create(vm, iso_date_time_to_string(result, date_time->calendar(), precision.precision, show_calendar));
}

// 5.3.35 Temporal.PlainDateTime.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tolocalestring
// 15.12.4.1 Temporal.PlainDateTime.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sup-properties-of-the-temporal-plaindatetime-prototype-object
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_locale_string)
{
    auto& realm = *vm.current_realm();

    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let dateFormat be ? CreateDateTimeFormat(%Intl.DateTimeFormat%, locales, options, ANY, ALL).
    auto date_format = TRY(Intl::create_date_time_format(vm, realm.intrinsics().intl_date_time_format_constructor(), locales, options, Intl::OptionRequired::Any, Intl::OptionDefaults::All));

    // 4. Return ? FormatDateTime(dateFormat, dateTime).
    return PrimitiveString::create(vm, TRY(Intl::format_date_time(vm, date_format, date_time)));
}

// 5.3.36 Temporal.PlainDateTime.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_json)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ISODateTimeToString(dateTime.[[ISODateTime]], dateTime.[[Calendar]], AUTO, AUTO).
    return PrimitiveString::create(vm, iso_date_time_to_string(date_time->iso_date_time(), date_time->calendar(), Auto {}, ShowCalendar::Auto));
}

// 5.3.37 Temporal.PlainDateTime.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.PlainDateTime", "a primitive value");
}

// 5.3.38 Temporal.PlainDateTime.prototype.toZonedDateTime ( temporalTimeZoneLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tozoneddatetime
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_zoned_date_time)
{
    auto temporal_time_zone_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be ? ToTemporalTimeZoneIdentifier(temporalTimeZoneLike).
    auto time_zone = TRY(to_temporal_time_zone_identifier(vm, temporal_time_zone_like));

    // 4. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 5. Let disambiguation be ? GetTemporalDisambiguationOption(resolvedOptions).
    auto disambiguation = TRY(get_temporal_disambiguation_option(vm, resolved_options));

    // 6. Let epochNs be ? GetEpochNanosecondsFor(timeZone, dateTime.[[ISODateTime]], disambiguation).
    auto epoch_nanoseconds = TRY(get_epoch_nanoseconds_for(vm, time_zone, date_time->iso_date_time(), disambiguation));

    // 7. Return ! CreateTemporalZonedDateTime(epochNs, timeZone, dateTime.[[Calendar]]).
    return MUST(create_temporal_zoned_date_time(vm, BigInt::create(vm, move(epoch_nanoseconds)), move(time_zone), date_time->calendar()));
}

// 5.3.39 Temporal.PlainDateTime.prototype.toPlainDate ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.toplaindate
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_plain_date)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ! CreateTemporalDate(dateTime.[[ISODateTime]].[[ISODate]], dateTime.[[Calendar]]).
    return MUST(create_temporal_date(vm, date_time->iso_date_time().iso_date, date_time->calendar()));
}

// 5.3.40 Temporal.PlainDateTime.prototype.toPlainTime ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.toplaintime
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_plain_time)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ! CreateTemporalTime(dateTime.[[ISODateTime]].[[Time]]).
    return MUST(create_temporal_time(vm, date_time->iso_date_time().time));
}

}
