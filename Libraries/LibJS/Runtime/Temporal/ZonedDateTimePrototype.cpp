/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/Temporal/ZonedDateTimePrototype.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(ZonedDateTimePrototype);

// 6.3 Properties of the Temporal.ZonedDateTime Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-zoneddatetime-prototype-object
ZonedDateTimePrototype::ZonedDateTimePrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void ZonedDateTimePrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 6.3.2 Temporal.ZonedDateTime.prototype[ @@toStringTag ], https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.ZonedDateTime"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.calendar, calendar_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.calendarId, calendar_id_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.timeZone, time_zone_getter, {}, Attribute::Configurable);
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
    define_native_accessor(realm, vm.names.epochSeconds, epoch_seconds_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.epochMilliseconds, epoch_milliseconds_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.epochMicroseconds, epoch_microseconds_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.epochNanoseconds, epoch_nanoseconds_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.dayOfWeek, day_of_week_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.dayOfYear, day_of_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.weekOfYear, week_of_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.yearOfWeek, year_of_week_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.hoursInDay, hours_in_day_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.daysInWeek, days_in_week_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.daysInMonth, days_in_month_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.daysInYear, days_in_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.monthsInYear, months_in_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.inLeapYear, in_leap_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.offsetNanoseconds, offset_nanoseconds_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.offset, offset_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.era, era_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.eraYear, era_year_getter, {}, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.with, with, 1, attr);
    define_native_function(realm, vm.names.withPlainTime, with_plain_time, 0, attr);
    define_native_function(realm, vm.names.withPlainDate, with_plain_date, 1, attr);
    define_native_function(realm, vm.names.withTimeZone, with_time_zone, 1, attr);
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
    define_native_function(realm, vm.names.startOfDay, start_of_day, 0, attr);
    define_native_function(realm, vm.names.toInstant, to_instant, 0, attr);
    define_native_function(realm, vm.names.toPlainDate, to_plain_date, 0, attr);
    define_native_function(realm, vm.names.toPlainTime, to_plain_time, 0, attr);
    define_native_function(realm, vm.names.toPlainDateTime, to_plain_date_time, 0, attr);
    define_native_function(realm, vm.names.toPlainYearMonth, to_plain_year_month, 0, attr);
    define_native_function(realm, vm.names.toPlainMonthDay, to_plain_month_day, 0, attr);
    define_native_function(realm, vm.names.getISOFields, get_iso_fields, 0, attr);
}

// 6.3.3 get Temporal.ZonedDateTime.prototype.calendar, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.calendar
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::calendar_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return zonedDateTime.[[Calendar]].
    return Value(&zoned_date_time->calendar());
}

// 6.3.4 get Temporal.ZonedDateTime.prototype.timeZone, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.timezone
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::time_zone_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return zonedDateTime.[[TimeZone]].
    return Value(&zoned_date_time->time_zone());
}

// 6.3.5 get Temporal.ZonedDateTime.prototype.year, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.year
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::year_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(? CalendarYear(calendar, temporalDateTime)).
    return TRY(calendar_year(vm, calendar, *temporal_date_time));
}

// 6.3.6 get Temporal.ZonedDateTime.prototype.month, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.month
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::month_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(? CalendarMonth(calendar, temporalDateTime)).
    return TRY(calendar_month(vm, calendar, *temporal_date_time));
}

// 6.3.7 get Temporal.ZonedDateTime.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.monthcode
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::month_code_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return ? CalendarMonthCode(calendar, temporalDateTime).
    return PrimitiveString::create(vm, TRY(calendar_month_code(vm, calendar, *temporal_date_time)));
}

// 6.3.8 get Temporal.ZonedDateTime.prototype.day, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.day
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::day_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(? CalendarDay(calendar, temporalDateTime)).
    return TRY(calendar_day(vm, calendar, *temporal_date_time));
}

// 6.3.9 get Temporal.ZonedDateTime.prototype.hour, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.hour
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::hour_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(temporalDateTime.[[ISOHour]]).
    return Value(temporal_date_time->iso_hour());
}

// 6.3.10 get Temporal.ZonedDateTime.prototype.minute, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.minute
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::minute_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(temporalDateTime.[[ISOMinute]]).
    return Value(temporal_date_time->iso_minute());
}

// 6.3.11 get Temporal.ZonedDateTime.prototype.second, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.second
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::second_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(temporalDateTime.[[ISOSecond]]).
    return Value(temporal_date_time->iso_second());
}

// 6.3.12 get Temporal.ZonedDateTime.prototype.millisecond, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.millisecond
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::millisecond_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(temporalDateTime.[[ISOMillisecond]]).
    return Value(temporal_date_time->iso_millisecond());
}

// 6.3.13 get Temporal.ZonedDateTime.prototype.microsecond, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.microsecond
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::microsecond_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(temporalDateTime.[[ISOMicrosecond]]).
    return Value(temporal_date_time->iso_microsecond());
}

// 6.3.14 get Temporal.ZonedDateTime.prototype.nanosecond, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.nanosecond
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::nanosecond_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(temporalDateTime.[[ISONanosecond]]).
    return Value(temporal_date_time->iso_nanosecond());
}

// 6.3.15 get Temporal.ZonedDateTime.prototype.epochSeconds, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.epochseconds
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::epoch_seconds_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let ns be zonedDateTime.[[Nanoseconds]].
    auto& ns = zoned_date_time->nanoseconds();

    // 4. Let s be truncate(ℝ(ns) / 10^9).
    auto s = ns.big_integer().divided_by(Crypto::UnsignedBigInteger { 1'000'000'000 }).quotient;

    // 5. Return 𝔽(s).
    return Value((double)s.to_base_deprecated(10).to_number<i64>().value());
}

// 6.3.16 get Temporal.ZonedDateTime.prototype.epochMilliseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.epochmilliseconds
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::epoch_milliseconds_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let ns be zonedDateTime.[[Nanoseconds]].
    auto& ns = zoned_date_time->nanoseconds();

    // 4. Let ms be truncate(ℝ(ns) / 10^6).
    auto ms = ns.big_integer().divided_by(Crypto::UnsignedBigInteger { 1'000'000 }).quotient;

    // 5. Return 𝔽(ms).
    return Value((double)ms.to_base_deprecated(10).to_number<i64>().value());
}

// 6.3.17 get Temporal.ZonedDateTime.prototype.epochMicroseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.epochmicroseconds
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::epoch_microseconds_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let ns be zonedDateTime.[[Nanoseconds]].
    auto& ns = zoned_date_time->nanoseconds();

    // 4. Let µs be truncate(ℝ(ns) / 10^3).
    auto us = ns.big_integer().divided_by(Crypto::UnsignedBigInteger { 1'000 }).quotient;

    // 5. Return ℤ(µs).
    return BigInt::create(vm, move(us));
}

// 6.3.18 get Temporal.ZonedDateTime.prototype.epochNanoseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.epochnanoseconds
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::epoch_nanoseconds_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return zonedDateTime.[[Nanoseconds]].
    return &zoned_date_time->nanoseconds();
}

// 6.3.19 get Temporal.ZonedDateTime.prototype.dayOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.dayofweek
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::day_of_week_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(? CalendarDayOfWeek(calendar, temporalDateTime)).
    return TRY(calendar_day_of_week(vm, calendar, *temporal_date_time));
}

// 6.3.20 get Temporal.ZonedDateTime.prototype.dayOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.dayofyear
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::day_of_year_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(? CalendarDayOfYear(calendar, temporalDateTime)).
    return TRY(calendar_day_of_year(vm, calendar, *temporal_date_time));
}

// 6.3.21 get Temporal.ZonedDateTime.prototype.weekOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.weekofyear
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::week_of_year_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(? CalendarWeekOfYear(calendar, temporalDateTime)).
    return TRY(calendar_week_of_year(vm, calendar, *temporal_date_time));
}

// 6.3.22 get Temporal.ZonedDateTime.prototype.yearOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.yearofweek
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::year_of_week_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(? CalendarYearOfWeek(calendar, temporalDateTime)).
    return TRY(calendar_year_of_week(vm, calendar, *temporal_date_time));
}

// 6.3.23 get Temporal.ZonedDateTime.prototype.hoursInDay, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.hoursinday
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::hours_in_day_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let isoCalendar be ! GetISO8601Calendar().
    auto* iso_calendar = get_iso8601_calendar(vm);

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, isoCalendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, *iso_calendar));

    // 7. Let year be temporalDateTime.[[ISOYear]].
    auto year = temporal_date_time->iso_year();

    // 8. Let month be temporalDateTime.[[ISOMonth]].
    auto month = temporal_date_time->iso_month();

    // 9. Let day be temporalDateTime.[[ISODay]].
    auto day = temporal_date_time->iso_day();

    // 10. Let today be ? CreateTemporalDateTime(year, month, day, 0, 0, 0, 0, 0, 0, isoCalendar).
    auto* today = TRY(create_temporal_date_time(vm, year, month, day, 0, 0, 0, 0, 0, 0, *iso_calendar));

    // 11. Let tomorrowFields be BalanceISODate(year, month, day + 1).
    auto tomorrow_fields = balance_iso_date(year, month, day + 1);

    // 12. Let tomorrow be ? CreateTemporalDateTime(tomorrowFields.[[Year]], tomorrowFields.[[Month]], tomorrowFields.[[Day]], 0, 0, 0, 0, 0, 0, isoCalendar).
    auto* tomorrow = TRY(create_temporal_date_time(vm, tomorrow_fields.year, tomorrow_fields.month, tomorrow_fields.day, 0, 0, 0, 0, 0, 0, *iso_calendar));

    // 13. Let todayInstant be ? BuiltinTimeZoneGetInstantFor(timeZone, today, "compatible").
    auto today_instant = TRY(builtin_time_zone_get_instant_for(vm, &time_zone, *today, "compatible"sv));

    // 14. Let tomorrowInstant be ? BuiltinTimeZoneGetInstantFor(timeZone, tomorrow, "compatible").
    auto tomorrow_instant = TRY(builtin_time_zone_get_instant_for(vm, &time_zone, *tomorrow, "compatible"sv));

    // 15. Let diffNs be tomorrowInstant.[[Nanoseconds]] - todayInstant.[[Nanoseconds]].
    auto diff_ns = tomorrow_instant->nanoseconds().big_integer().minus(today_instant->nanoseconds().big_integer());

    // 16. Return 𝔽(diffNs / (3.6 × 10^12)).
    auto hours_diff_ns = diff_ns.divided_by("3600000000000"_bigint).quotient;
    return Value(hours_diff_ns.to_double());
}

// 6.3.24 get Temporal.ZonedDateTime.prototype.daysInWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.daysinweek
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::days_in_week_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(? CalendarDaysInWeek(calendar, temporalDateTime)).
    return TRY(calendar_days_in_week(vm, calendar, *temporal_date_time));
}

// 6.3.25 get Temporal.ZonedDateTime.prototype.daysInMonth, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.daysinmonth
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::days_in_month_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(? CalendarDaysInMonth(calendar, temporalDateTime)).
    return TRY(calendar_days_in_month(vm, calendar, *temporal_date_time));
}

// 6.3.26 get Temporal.ZonedDateTime.prototype.daysInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.daysinyear
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::days_in_year_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(? CalendarDaysInYear(calendar, temporalDateTime)).
    return TRY(calendar_days_in_year(vm, calendar, *temporal_date_time));
}

// 6.3.27 get Temporal.ZonedDateTime.prototype.monthsInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.monthsinyear
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::months_in_year_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return 𝔽(? CalendarMonthsInYear(calendar, temporalDateTime)).
    return TRY(calendar_months_in_year(vm, calendar, *temporal_date_time));
}

// 6.3.28 get Temporal.ZonedDateTime.prototype.inLeapYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.inleapyear
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::in_leap_year_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return ? CalendarInLeapYear(calendar, temporalDateTime).
    return TRY(calendar_in_leap_year(vm, calendar, *temporal_date_time));
}

// 6.3.29 get Temporal.ZonedDateTime.prototype.offsetNanoseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.offsetnanoseconds
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::offset_nanoseconds_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    // 3. Let timeZoneRec be ? CreateTimeZoneMethodsRecord(zonedDateTime.[[TimeZone]], « GET-OFFSET-NANOSECONDS-FOR »).
    auto time_zone_record = TRY(create_time_zone_methods_record(vm, NonnullGCPtr<Object> { zoned_date_time->time_zone() }, { { TimeZoneMethod::GetOffsetNanosecondsFor } }));

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Return 𝔽(? GetOffsetNanosecondsFor(timeZoneRec, instant)).
    return Value(TRY(get_offset_nanoseconds_for(vm, time_zone_record, *instant)));
}

// 6.3.30 get Temporal.ZonedDateTime.prototype.offset, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.offset
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::offset_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 4. Return ? BuiltinTimeZoneGetOffsetStringFor(zonedDateTime.[[TimeZone]], instant).
    auto offset_string = TRY(builtin_time_zone_get_offset_string_for(vm, &zoned_date_time->time_zone(), *instant));
    return PrimitiveString::create(vm, move(offset_string));
}

// 15.6.10.2 get Temporal.ZonedDateTime.prototype.era, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.era
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::era_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let plainDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* plain_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return ? CalendarEra(calendar, plainDateTime).
    return TRY(calendar_era(vm, calendar, *plain_date_time));
}

// 15.6.10.3 get Temporal.ZonedDateTime.prototype.eraYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.erayear
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::era_year_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let plainDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* plain_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return ? CalendarEraYear(calendar, plainDateTime).
    return TRY(calendar_era_year(vm, calendar, *plain_date_time));
}

// 6.3.31 Temporal.ZonedDateTime.prototype.with ( temporalZonedDateTimeLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.with
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::with)
{
    auto temporal_zoned_date_time_like = vm.argument(0);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. If Type(temporalZonedDateTimeLike) is not Object, then
    if (!temporal_zoned_date_time_like.is_object()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, temporal_zoned_date_time_like.to_string_without_side_effects());
    }

    // 4. Perform ? RejectObjectWithCalendarOrTimeZone(temporalZonedDateTimeLike).
    TRY(reject_object_with_calendar_or_time_zone(vm, temporal_zoned_date_time_like.as_object()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let fieldNames be ? CalendarFields(calendar, « "day", "hour", "microsecond", "millisecond", "minute", "month", "monthCode", "nanosecond", "second", "year" »).
    auto field_names = TRY(calendar_fields(vm, calendar, { "day"sv, "hour"sv, "microsecond"sv, "millisecond"sv, "minute"sv, "month"sv, "monthCode"sv, "nanosecond"sv, "second"sv, "year"sv }));

    // 7. Append "offset" to fieldNames.
    field_names.append("offset"_string);

    // 8. Let partialZonedDateTime be ? PrepareTemporalFields(temporalZonedDateTimeLike, fieldNames, partial).
    auto* partial_zoned_date_time = TRY(prepare_temporal_fields(vm, temporal_zoned_date_time_like.as_object(), field_names, PrepareTemporalFieldsPartial {}));

    // 9. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(1)));

    // 10. Let disambiguation be ? ToTemporalDisambiguation(options).
    auto disambiguation = TRY(to_temporal_disambiguation(vm, options));

    // 11. Let offset be ? ToTemporalOffset(options, "prefer").
    auto offset = TRY(to_temporal_offset(vm, options, "prefer"sv));

    // 12. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 13. Append "timeZone" to fieldNames.
    field_names.append("timeZone"_string);

    // 14. Let fields be ? PrepareTemporalFields(zonedDateTime, fieldNames, « "timeZone", "offset" »).
    auto* fields = TRY(prepare_temporal_fields(vm, zoned_date_time, field_names, Vector<StringView> { "timeZone"sv, "offset"sv }));

    // 15. Set fields to ? CalendarMergeFields(calendar, fields, partialZonedDateTime).
    fields = TRY(calendar_merge_fields(vm, calendar, *fields, *partial_zoned_date_time));

    // 16. Set fields to ? PrepareTemporalFields(fields, fieldNames, « "timeZone", "offset" »).
    fields = TRY(prepare_temporal_fields(vm, *fields, field_names, Vector<StringView> { "timeZone"sv, "offset"sv }));

    // 17. Let offsetString be ! Get(fields, "offset").
    auto offset_string_value = MUST(fields->get(vm.names.offset));

    // 18. Assert: Type(offsetString) is String.
    VERIFY(offset_string_value.is_string());
    auto offset_string = offset_string_value.as_string().utf8_string();

    // 19. Let dateTimeResult be ? InterpretTemporalDateTimeFields(calendar, fields, options).
    auto date_time_result = TRY(interpret_temporal_date_time_fields(vm, calendar, *fields, options));

    // 20. If IsTimeZoneOffsetString(offsetString) is false, throw a RangeError exception.
    if (!is_time_zone_offset_string(offset_string))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeZoneName, offset_string);

    // 21. Let offsetNanoseconds be ParseTimeZoneOffsetString(offsetString).
    auto offset_nanoseconds = parse_time_zone_offset_string(offset_string);

    // 22. Let epochNanoseconds be ? InterpretISODateTimeOffset(dateTimeResult.[[Year]], dateTimeResult.[[Month]], dateTimeResult.[[Day]], dateTimeResult.[[Hour]], dateTimeResult.[[Minute]], dateTimeResult.[[Second]], dateTimeResult.[[Millisecond]], dateTimeResult.[[Microsecond]], dateTimeResult.[[Nanosecond]], option, offsetNanoseconds, timeZone, disambiguation, offset, match exactly).
    auto* epoch_nanoseconds = TRY(interpret_iso_date_time_offset(vm, date_time_result.year, date_time_result.month, date_time_result.day, date_time_result.hour, date_time_result.minute, date_time_result.second, date_time_result.millisecond, date_time_result.microsecond, date_time_result.nanosecond, OffsetBehavior::Option, offset_nanoseconds, &time_zone, disambiguation, offset, MatchBehavior::MatchExactly));

    // 23. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, *epoch_nanoseconds, time_zone, calendar));
}

// 6.3.32 Temporal.ZonedDateTime.prototype.withPlainTime ( [ plainTimeLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.withplaintime
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::with_plain_time)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    PlainTime* plain_time = nullptr;

    // 3. If plainTimeLike is undefined, then
    if (vm.argument(0).is_undefined()) {
        // a. Let plainTime be ! CreateTemporalTime(0, 0, 0, 0, 0, 0).
        plain_time = MUST(create_temporal_time(vm, 0, 0, 0, 0, 0, 0));
    }
    // 4. Else,
    else {
        // a. Let plainTime be ? ToTemporalTime(plainTimeLike).
        plain_time = TRY(to_temporal_time(vm, vm.argument(0)));
    }

    // 5. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 6. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 7. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 8. Let plainDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* plain_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 9. Let resultPlainDateTime be ? CreateTemporalDateTime(plainDateTime.[[ISOYear]], plainDateTime.[[ISOMonth]], plainDateTime.[[ISODay]], plainTime.[[ISOHour]], plainTime.[[ISOMinute]], plainTime.[[ISOSecond]], plainTime.[[ISOMillisecond]], plainTime.[[ISOMicrosecond]], plainTime.[[ISONanosecond]], calendar).
    auto* result_plain_date_time = TRY(create_temporal_date_time(vm, plain_date_time->iso_year(), plain_date_time->iso_month(), plain_date_time->iso_day(), plain_time->iso_hour(), plain_time->iso_minute(), plain_time->iso_second(), plain_time->iso_millisecond(), plain_time->iso_microsecond(), plain_time->iso_nanosecond(), calendar));

    // 10. Set instant to ? BuiltinTimeZoneGetInstantFor(timeZone, resultPlainDateTime, "compatible").
    instant = TRY(builtin_time_zone_get_instant_for(vm, &time_zone, *result_plain_date_time, "compatible"sv));

    // 11. Return ! CreateTemporalZonedDateTime(instant.[[Nanoseconds]], timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, instant->nanoseconds(), time_zone, calendar));
}

// 6.3.33 Temporal.ZonedDateTime.prototype.withPlainDate ( plainDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.withplaindate
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::with_plain_date)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let plainDate be ? ToTemporalDate(plainDateLike).
    auto* plain_date = TRY(to_temporal_date(vm, vm.argument(0)));

    // 4. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 5. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 6. Let plainDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, zonedDateTime.[[Calendar]]).
    auto* plain_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, zoned_date_time->calendar()));

    // 7. Let calendar be ? ConsolidateCalendars(zonedDateTime.[[Calendar]], plainDate.[[Calendar]]).
    auto* calendar = TRY(consolidate_calendars(vm, zoned_date_time->calendar(), plain_date->calendar()));

    // 8. Let resultPlainDateTime be ? CreateTemporalDateTime(plainDate.[[ISOYear]], plainDate.[[ISOMonth]], plainDate.[[ISODay]], plainDateTime.[[ISOHour]], plainDateTime.[[ISOMinute]], plainDateTime.[[ISOSecond]], plainDateTime.[[ISOMillisecond]], plainDateTime.[[ISOMicrosecond]], plainDateTime.[[ISONanosecond]], calendar).
    auto* result_plain_date_time = TRY(create_temporal_date_time(vm, plain_date->iso_year(), plain_date->iso_month(), plain_date->iso_day(), plain_date_time->iso_hour(), plain_date_time->iso_minute(), plain_date_time->iso_second(), plain_date_time->iso_millisecond(), plain_date_time->iso_microsecond(), plain_date_time->iso_nanosecond(), *calendar));

    // 9. Set instant to ? BuiltinTimeZoneGetInstantFor(timeZone, resultPlainDateTime, "compatible").
    instant = TRY(builtin_time_zone_get_instant_for(vm, &time_zone, *result_plain_date_time, "compatible"sv));

    // 10. Return ! CreateTemporalZonedDateTime(instant.[[Nanoseconds]], timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, instant->nanoseconds(), time_zone, *calendar));
}

// 6.3.34 Temporal.ZonedDateTime.prototype.withTimeZone ( timeZoneLike ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.withtimezone
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::with_time_zone)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be ? ToTemporalTimeZone(timeZoneLike).
    auto* time_zone = TRY(to_temporal_time_zone(vm, vm.argument(0)));

    // 4. Return ! CreateTemporalZonedDateTime(zonedDateTime.[[Nanoseconds]], timeZone, zonedDateTime.[[Calendar]]).
    return MUST(create_temporal_zoned_date_time(vm, zoned_date_time->nanoseconds(), *time_zone, zoned_date_time->calendar()));
}

// 6.3.35 Temporal.ZonedDateTime.prototype.withCalendar ( calendarLike ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.withcalendar
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::with_calendar)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be ? ToTemporalCalendar(calendarLike).
    auto* calendar = TRY(to_temporal_calendar(vm, vm.argument(0)));

    // 4. Return ! CreateTemporalZonedDateTime(zonedDateTime.[[Nanoseconds]], zonedDateTime.[[TimeZone]], calendar).
    return MUST(create_temporal_zoned_date_time(vm, zoned_date_time->nanoseconds(), zoned_date_time->time_zone(), *calendar));
}

// 6.3.36 Temporal.ZonedDateTime.prototype.add ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.add
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::add)
{
    auto temporal_duration_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToOrSubtractDurationFromZonedDateTime(add, zonedDateTime, temporalDurationLike, options).
    return TRY(add_duration_to_or_subtract_duration_from_zoned_date_time(vm, ArithmeticOperation::Add, zoned_date_time, temporal_duration_like, options));
}

// 6.3.37 Temporal.ZonedDateTime.prototype.subtract ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.subtract
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::subtract)
{
    auto temporal_duration_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToOrSubtractDurationFromZonedDateTime(subtract, zonedDateTime, temporalDurationLike, options).
    return TRY(add_duration_to_or_subtract_duration_from_zoned_date_time(vm, ArithmeticOperation::Subtract, zoned_date_time, temporal_duration_like, options));
}

// 6.3.38 Temporal.ZonedDateTime.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.until
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::until)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalZonedDateTime(until, zonedDateTime, other, options).
    return TRY(difference_temporal_zoned_date_time(vm, DifferenceOperation::Until, zoned_date_time, other, options));
}

// 6.3.39 Temporal.ZonedDateTime.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.since
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::since)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalZonedDateTime(since, zonedDateTime, other, options).
    return TRY(difference_temporal_zoned_date_time(vm, DifferenceOperation::Since, zoned_date_time, other, options));
}

// 6.3.40 Temporal.ZonedDateTime.prototype.round ( roundTo ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.round
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::round)
{
    auto& realm = *vm.current_realm();

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. If roundTo is undefined, then
    if (vm.argument(0).is_undefined()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::TemporalMissingOptionsObject);
    }

    Object* round_to;

    // 4. If Type(roundTo) is String, then
    if (vm.argument(0).is_string()) {
        // a. Let paramString be roundTo.

        // b. Set roundTo to OrdinaryObjectCreate(null).
        round_to = Object::create(realm, nullptr);

        // c. Perform ! CreateDataPropertyOrThrow(roundTo, "smallestUnit", paramString).
        MUST(round_to->create_data_property_or_throw(vm.names.smallestUnit, vm.argument(0)));
    }
    // 5. Else,
    else {
        // a. Set roundTo to ? GetOptionsObject(roundTo).
        round_to = TRY(get_options_object(vm, vm.argument(0)));
    }

    // 6. Let smallestUnit be ? GetTemporalUnit(roundTo, "smallestUnit", time, required, « "day" »).
    auto smallest_unit = TRY(get_temporal_unit(vm, *round_to, vm.names.smallestUnit, UnitGroup::Time, TemporalUnitRequired {}, { "day"sv }));

    // 7. Let roundingMode be ? ToTemporalRoundingMode(roundTo, "halfExpand").
    auto rounding_mode = TRY(to_temporal_rounding_mode(vm, *round_to, "halfExpand"sv));

    // 8. If smallestUnit is "day", then
    Optional<u16> maximum;
    if (smallest_unit == "day"sv) {
        // a. Let maximum be 1.
        maximum = 1;
    }
    // 9. Else
    else {
        // a. Let maximum be ! MaximumTemporalDurationRoundingIncrement(smallestUnit)
        maximum = maximum_temporal_duration_rounding_increment(*smallest_unit);

        // b. Assert: maximum is not undefined
        VERIFY(maximum.has_value());
    }

    // 10. Let roundingIncrement be ? ToTemporalDateTimeRoundingIncrement(roundTo).
    auto rounding_increment = TRY(to_temporal_rounding_increment(vm, *round_to));

    // 11. Perform ? ValidateTemporalDateTimeRoundingIncrement(roundingIncrement, maximum, false).
    TRY(validate_temporal_rounding_increment(vm, rounding_increment, *maximum, false));

    // 12. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    auto time_zone_record = TRY(create_time_zone_methods_record(vm, NonnullGCPtr<Object> { time_zone }, { { TimeZoneMethod::GetOffsetNanosecondsFor } }));

    // 13. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 14. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 15. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 16. Let isoCalendar be ! GetISO8601Calendar().
    auto* iso_calendar = get_iso8601_calendar(vm);

    // 17. Let dtStart be ? CreateTemporalDateTime(temporalDateTime.[[ISOYear]], temporalDateTime.[[ISOMonth]], temporalDateTime.[[ISODay]], 0, 0, 0, 0, 0, 0, isoCalendar).
    auto* dt_start = TRY(create_temporal_date_time(vm, temporal_date_time->iso_year(), temporal_date_time->iso_month(), temporal_date_time->iso_day(), 0, 0, 0, 0, 0, 0, *iso_calendar));

    // 18. Let instantStart be ? BuiltinTimeZoneGetInstantFor(timeZone, dtStart, "compatible").
    auto instant_start = TRY(builtin_time_zone_get_instant_for(vm, &time_zone, *dt_start, "compatible"sv));

    // 19. Let startNs be instantStart.[[Nanoseconds]].
    auto& start_ns = instant_start->nanoseconds();

    // 20. Let endNs be ? AddZonedDateTime(startNs, timeZone, calendar, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0).
    auto* end_ns = TRY(add_zoned_date_time(vm, start_ns, &time_zone, calendar, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0));

    // 21. Let dayLengthNs be ℝ(endNs - startNs).
    auto day_length_ns = end_ns->big_integer().minus(start_ns.big_integer()).to_double();

    // 22. If dayLengthNs ≤ 0, then
    if (day_length_ns <= 0) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalZonedDateTimeRoundZeroOrNegativeLengthDay);
    }

    // 23. Let roundResult be ! RoundISODateTime(temporalDateTime.[[ISOYear]], temporalDateTime.[[ISOMonth]], temporalDateTime.[[ISODay]], temporalDateTime.[[ISOHour]], temporalDateTime.[[ISOMinute]], temporalDateTime.[[ISOSecond]], temporalDateTime.[[ISOMillisecond]], temporalDateTime.[[ISOMicrosecond]], temporalDateTime.[[ISONanosecond]], roundingIncrement, smallestUnit, roundingMode, dayLengthNs).
    auto round_result = round_iso_date_time(temporal_date_time->iso_year(), temporal_date_time->iso_month(), temporal_date_time->iso_day(), temporal_date_time->iso_hour(), temporal_date_time->iso_minute(), temporal_date_time->iso_second(), temporal_date_time->iso_millisecond(), temporal_date_time->iso_microsecond(), temporal_date_time->iso_nanosecond(), rounding_increment, *smallest_unit, rounding_mode, day_length_ns);

    // 24. Let offsetNanoseconds be ? GetOffsetNanosecondsFor(timeZone, instant).
    auto offset_nanoseconds = TRY(get_offset_nanoseconds_for(vm, time_zone_record, *instant));

    // 25. Let epochNanoseconds be ? InterpretISODateTimeOffset(roundResult.[[Year]], roundResult.[[Month]], roundResult.[[Day]], roundResult.[[Hour]], roundResult.[[Minute]], roundResult.[[Second]], roundResult.[[Millisecond]], roundResult.[[Microsecond]], roundResult.[[Nanosecond]], option, offsetNanoseconds, timeZone, "compatible", "prefer", match exactly).
    auto* epoch_nanoseconds = TRY(interpret_iso_date_time_offset(vm, round_result.year, round_result.month, round_result.day, round_result.hour, round_result.minute, round_result.second, round_result.millisecond, round_result.microsecond, round_result.nanosecond, OffsetBehavior::Option, offset_nanoseconds, &time_zone, "compatible"sv, "prefer"sv, MatchBehavior::MatchExactly));

    // 26. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, *epoch_nanoseconds, time_zone, calendar));
}

// 6.3.41 Temporal.ZonedDateTime.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.equals
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::equals)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Set other to ? ToTemporalZonedDateTime(other).
    auto* other = TRY(to_temporal_zoned_date_time(vm, vm.argument(0)));

    // 4. If zonedDateTime.[[Nanoseconds]] ≠ other.[[Nanoseconds]], return false.
    if (zoned_date_time->nanoseconds().big_integer() != other->nanoseconds().big_integer())
        return Value(false);

    // 5. If ? TimeZoneEquals(zonedDateTime.[[TimeZone]], other.[[TimeZone]]) is false, return false.
    if (!TRY(time_zone_equals(vm, zoned_date_time->time_zone(), other->time_zone())))
        return Value(false);

    // 6. Return ? CalendarEquals(zonedDateTime.[[Calendar]], other.[[Calendar]]).
    return Value(TRY(calendar_equals(vm, zoned_date_time->calendar(), other->calendar())));
}

// 6.3.42 Temporal.ZonedDateTime.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_string)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(0)));

    // 4. Let precision be ? ToSecondsStringPrecisionRecord(options).
    auto precision = TRY(to_seconds_string_precision_record(vm, *options));

    // 5. Let roundingMode be ? ToTemporalRoundingMode(options, "trunc").
    auto rounding_mode = TRY(to_temporal_rounding_mode(vm, *options, "trunc"sv));

    // 6. Let showCalendar be ? ToCalendarNameOption(options).
    auto show_calendar = TRY(to_calendar_name_option(vm, *options));

    // 7. Let showTimeZone be ? ToTimeZoneNameOption(options).
    auto show_time_zone = TRY(to_time_zone_name_option(vm, *options));

    // 8. Let showOffset be ? ToShowOffsetOption(options).
    auto show_offset = TRY(to_show_offset_option(vm, *options));

    // 9. Return ? TemporalZonedDateTimeToString(zonedDateTime, precision.[[Precision]], showCalendar, showTimeZone, showOffset, precision.[[Increment]], precision.[[Unit]], roundingMode).
    return PrimitiveString::create(vm, TRY(temporal_zoned_date_time_to_string(vm, zoned_date_time, precision.precision, show_calendar, show_time_zone, show_offset, precision.increment, precision.unit, rounding_mode)));
}

// 6.3.43 Temporal.ZonedDateTime.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tolocalestring
// NOTE: This is the minimum toLocaleString implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_locale_string)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return ? TemporalZonedDateTimeToString(zonedDateTime, "auto", "auto", "auto", "auto").
    return PrimitiveString::create(vm, TRY(temporal_zoned_date_time_to_string(vm, zoned_date_time, "auto"sv, "auto"sv, "auto"sv, "auto"sv)));
}

// 6.3.44 Temporal.ZonedDateTime.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_json)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return ? TemporalZonedDateTimeToString(zonedDateTime, "auto", "auto", "auto", "auto").
    return PrimitiveString::create(vm, TRY(temporal_zoned_date_time_to_string(vm, zoned_date_time, "auto"sv, "auto"sv, "auto"sv, "auto"sv)));
}

// 6.3.45 Temporal.ZonedDateTime.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.ZonedDateTime", "a primitive value");
}

// 6.3.46 Temporal.ZonedDateTime.prototype.startOfDay ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.startofday
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::start_of_day)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 5. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Let startDateTime be ? CreateTemporalDateTime(temporalDateTime.[[ISOYear]], temporalDateTime.[[ISOMonth]], temporalDateTime.[[ISODay]], 0, 0, 0, 0, 0, 0, calendar).
    auto* start_date_time = TRY(create_temporal_date_time(vm, temporal_date_time->iso_year(), temporal_date_time->iso_month(), temporal_date_time->iso_day(), 0, 0, 0, 0, 0, 0, calendar));

    // 8. Let startInstant be ? BuiltinTimeZoneGetInstantFor(timeZone, startDateTime, "compatible").
    auto start_instant = TRY(builtin_time_zone_get_instant_for(vm, &time_zone, *start_date_time, "compatible"sv));

    // 9. Return ! CreateTemporalZonedDateTime(startInstant.[[Nanoseconds]], timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, start_instant->nanoseconds(), time_zone, calendar));
}

// 6.3.47 Temporal.ZonedDateTime.prototype.toInstant ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toinstant
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_instant)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    return MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));
}

// 6.3.48 Temporal.ZonedDateTime.prototype.toPlainDate ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplaindate
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_plain_date)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return ! CreateTemporalDate(temporalDateTime.[[ISOYear]], temporalDateTime.[[ISOMonth]], temporalDateTime.[[ISODay]], calendar).
    return MUST(create_temporal_date(vm, temporal_date_time->iso_year(), temporal_date_time->iso_month(), temporal_date_time->iso_day(), calendar));
}

// 6.3.49 Temporal.ZonedDateTime.prototype.toPlainTime ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplaintime
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_plain_time)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, zonedDateTime.[[Calendar]]).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Return ! CreateTemporalTime(temporalDateTime.[[ISOHour]], temporalDateTime.[[ISOMinute]], temporalDateTime.[[ISOSecond]], temporalDateTime.[[ISOMillisecond]], temporalDateTime.[[ISOMicrosecond]], temporalDateTime.[[ISONanosecond]]).
    return MUST(create_temporal_time(vm, temporal_date_time->iso_hour(), temporal_date_time->iso_minute(), temporal_date_time->iso_second(), temporal_date_time->iso_millisecond(), temporal_date_time->iso_microsecond(), temporal_date_time->iso_nanosecond()));
}

// 6.3.50 Temporal.ZonedDateTime.prototype.toPlainDateTime ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplaindatetime
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_plain_date_time)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Return ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, zonedDateTime.[[Calendar]]).
    return TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, zoned_date_time->calendar()));
}

// 6.3.51 Temporal.ZonedDateTime.prototype.toPlainYearMonth ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplainyearmonth
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_plain_year_month)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Let fieldNames be ? CalendarFields(calendar, « "monthCode", "year" »).
    auto field_names = TRY(calendar_fields(vm, calendar, { "monthCode"sv, "year"sv }));

    // 8. Let fields be ? PrepareTemporalFields(temporalDateTime, fieldNames, «»).
    auto* fields = TRY(prepare_temporal_fields(vm, *temporal_date_time, field_names, Vector<StringView> {}));

    // 9. Return ? CalendarYearMonthFromFields(calendar, fields).
    return TRY(calendar_year_month_from_fields(vm, calendar, *fields));
}

// 6.3.52 Temporal.ZonedDateTime.prototype.toPlainMonthDay ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplainmonthday
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_plain_month_day)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 4. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 6. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 7. Let fieldNames be ? CalendarFields(calendar, « "day", "monthCode" »).
    auto field_names = TRY(calendar_fields(vm, calendar, { "day"sv, "monthCode"sv }));

    // 8. Let fields be ? PrepareTemporalFields(temporalDateTime, fieldNames, «»).
    auto* fields = TRY(prepare_temporal_fields(vm, *temporal_date_time, field_names, Vector<StringView> {}));

    // 9. Return ? CalendarMonthDayFromFields(calendar, fields).
    return TRY(calendar_month_day_from_fields(vm, calendar, *fields));
}

// 6.3.53 Temporal.ZonedDateTime.prototype.getISOFields ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.getisofields
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::get_iso_fields)
{
    auto& realm = *vm.current_realm();

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let fields be OrdinaryObjectCreate(%Object.prototype%).
    auto fields = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time->time_zone();

    // 5. Let instant be ! CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, zoned_date_time->nanoseconds()));

    // 6. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time->calendar();

    // 7. Let dateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, calendar));

    // 8. Let offset be ? BuiltinTimeZoneGetOffsetStringFor(timeZone, instant).
    auto offset = TRY(builtin_time_zone_get_offset_string_for(vm, &time_zone, *instant));

    // 9. Perform ! CreateDataPropertyOrThrow(fields, "calendar", calendar).
    MUST(fields->create_data_property_or_throw(vm.names.calendar, Value(&calendar)));

    // 10. Perform ! CreateDataPropertyOrThrow(fields, "isoDay", 𝔽(dateTime.[[ISODay]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoDay, Value(date_time->iso_day())));

    // 11. Perform ! CreateDataPropertyOrThrow(fields, "isoHour", 𝔽(dateTime.[[ISOHour]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoHour, Value(date_time->iso_hour())));

    // 12. Perform ! CreateDataPropertyOrThrow(fields, "isoMicrosecond", 𝔽(dateTime.[[ISOMicrosecond]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoMicrosecond, Value(date_time->iso_microsecond())));

    // 13. Perform ! CreateDataPropertyOrThrow(fields, "isoMillisecond", 𝔽(dateTime.[[ISOMillisecond]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoMillisecond, Value(date_time->iso_millisecond())));

    // 14. Perform ! CreateDataPropertyOrThrow(fields, "isoMinute", 𝔽(dateTime.[[ISOMinute]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoMinute, Value(date_time->iso_minute())));

    // 15. Perform ! CreateDataPropertyOrThrow(fields, "isoMonth", 𝔽(dateTime.[[ISOMonth]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoMonth, Value(date_time->iso_month())));

    // 16. Perform ! CreateDataPropertyOrThrow(fields, "isoNanosecond", 𝔽(dateTime.[[ISONanosecond]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoNanosecond, Value(date_time->iso_nanosecond())));

    // 17. Perform ! CreateDataPropertyOrThrow(fields, "isoSecond", 𝔽(dateTime.[[ISOSecond]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoSecond, Value(date_time->iso_second())));

    // 18. Perform ! CreateDataPropertyOrThrow(fields, "isoYear", 𝔽(dateTime.[[ISOYear]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoYear, Value(date_time->iso_year())));

    // 19. Perform ! CreateDataPropertyOrThrow(fields, "offset", offset).
    MUST(fields->create_data_property_or_throw(vm.names.offset, PrimitiveString::create(vm, move(offset))));

    // 20. Perform ! CreateDataPropertyOrThrow(fields, "timeZone", timeZone).
    MUST(fields->create_data_property_or_throw(vm.names.timeZone, Value(&time_zone)));

    // 21. Return fields.
    return fields;
}

// 6.3.3 get Temporal.ZonedDateTime.prototype.calendarId
// https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.calendarid
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::calendar_id_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return zonedDateTime.[[Calendar]].identifier
    auto& calendar = static_cast<Calendar&>(zoned_date_time->calendar());
    return PrimitiveString::create(vm, calendar.identifier());
}

}
