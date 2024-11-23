/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainDateTimePrototype.h>

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
    define_native_function(realm, vm.names.add, add, 1, attr);
    define_native_function(realm, vm.names.subtract, subtract, 1, attr);
    define_native_function(realm, vm.names.equals, equals, 1, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);
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
// NOTE: This is the minimum toLocaleString implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_locale_string)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ISODateTimeToString(dateTime.[[ISODateTime]], dateTime.[[Calendar]], AUTO, AUTO).
    return PrimitiveString::create(vm, iso_date_time_to_string(date_time->iso_date_time(), date_time->calendar(), Auto {}, ShowCalendar::Auto));
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

}
