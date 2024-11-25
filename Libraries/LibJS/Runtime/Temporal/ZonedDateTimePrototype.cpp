/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTimePrototype.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(ZonedDateTimePrototype);

// 6.3 Properties of the Temporal.ZonedDateTime Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-zoneddatetime-prototype-object
ZonedDateTimePrototype::ZonedDateTimePrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void ZonedDateTimePrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 6.3.2 Temporal.ZonedDateTime.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.ZonedDateTime"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.calendarId, calendar_id_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.timeZoneId, time_zone_id_getter, {}, Attribute::Configurable);
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
    define_native_accessor(realm, vm.names.epochMilliseconds, epoch_milliseconds_getter, {}, Attribute::Configurable);
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

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.with, with, 1, attr);
    define_native_function(realm, vm.names.withPlainTime, with_plain_time, 0, attr);
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
    define_native_function(realm, vm.names.getTimeZoneTransition, get_time_zone_transition, 1, attr);
    define_native_function(realm, vm.names.toInstant, to_instant, 0, attr);
    define_native_function(realm, vm.names.toPlainDate, to_plain_date, 0, attr);
    define_native_function(realm, vm.names.toPlainTime, to_plain_time, 0, attr);
    define_native_function(realm, vm.names.toPlainDateTime, to_plain_date_time, 0, attr);
}

// 6.3.3 get Temporal.ZonedDateTime.prototype.calendarId, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.calendarid
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::calendar_id_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return zonedDateTime.[[Calendar]].
    return PrimitiveString::create(vm, zoned_date_time->calendar());
}

// 6.3.4 get Temporal.ZonedDateTime.prototype.timeZoneId, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.timezoneid
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::time_zone_id_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return zonedDateTime.[[TimeZone]].
    return PrimitiveString::create(vm, zoned_date_time->time_zone());
}

// 6.3.5 get Temporal.ZonedDateTime.prototype.era, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.era
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::era_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
    auto iso_date_time = get_iso_date_time_for(zoned_date_time->time_zone(), zoned_date_time->epoch_nanoseconds()->big_integer());

    // 4. Return CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[Era]].
    auto result = calendar_iso_to_date(zoned_date_time->calendar(), iso_date_time.iso_date).era;

    // 5. If result is undefined, return undefined.
    if (!result.has_value())
        return js_undefined();

    // 6. Return 𝔽(result).
    return PrimitiveString::create(vm, result.release_value());
}

// 6.3.6 get Temporal.ZonedDateTime.prototype.eraYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.erayear
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::era_year_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
    auto iso_date_time = get_iso_date_time_for(zoned_date_time->time_zone(), zoned_date_time->epoch_nanoseconds()->big_integer());

    // 4. Let result be CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[EraYear]].
    auto result = calendar_iso_to_date(zoned_date_time->calendar(), iso_date_time.iso_date).era_year;

    // 5. If result is undefined, return undefined.
    if (!result.has_value())
        return js_undefined();

    // 6. Return 𝔽(result).
    return *result;
}

// 6.3.7 get Temporal.ZonedDateTime.prototype.year, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.year
// 6.3.8 get Temporal.ZonedDateTime.prototype.month, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.month
// 6.3.10 get Temporal.ZonedDateTime.prototype.day, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.monthcode
// 6.3.19 get Temporal.ZonedDateTime.prototype.dayOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.dayofweek
// 6.3.20 get Temporal.ZonedDateTime.prototype.dayOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.dayofyear
// 6.3.24 get Temporal.ZonedDateTime.prototype.daysInWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.daysinweek
// 6.3.25 get Temporal.ZonedDateTime.prototype.daysInMonth, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.daysinmonth
// 6.3.26 get Temporal.ZonedDateTime.prototype.daysInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.daysinyear
// 6.3.27 get Temporal.ZonedDateTime.prototype.monthsInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.monthsinyear
// 6.3.28 get Temporal.ZonedDateTime.prototype.inLeapYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.inleapyear
#define JS_ENUMERATE_ZONED_DATE_TIME_SIMPLE_DATE_FIELDS \
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

#define __JS_ENUMERATE(field)                                                                                                          \
    JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::field##_getter)                                                                  \
    {                                                                                                                                  \
        /* 1. Let zonedDateTime be the this value. */                                                                                  \
        /* 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]). */                                   \
        auto zoned_date_time = TRY(typed_this_object(vm));                                                                             \
                                                                                                                                       \
        /* Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]). */                    \
        auto iso_date_time = get_iso_date_time_for(zoned_date_time->time_zone(), zoned_date_time->epoch_nanoseconds()->big_integer()); \
                                                                                                                                       \
        /* 3. Return 𝔽(CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[<field>]]). */                      \
        return calendar_iso_to_date(zoned_date_time->calendar(), iso_date_time.iso_date).field;                                        \
    }
JS_ENUMERATE_ZONED_DATE_TIME_SIMPLE_DATE_FIELDS
#undef __JS_ENUMERATE

// 6.3.9 get Temporal.ZonedDateTime.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.monthcode
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::month_code_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
    auto iso_date_time = get_iso_date_time_for(zoned_date_time->time_zone(), zoned_date_time->epoch_nanoseconds()->big_integer());

    // 4. Return CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[MonthCode]].
    auto month_code = calendar_iso_to_date(zoned_date_time->calendar(), iso_date_time.iso_date).month_code;
    return PrimitiveString::create(vm, move(month_code));
}

// 6.3.11 get Temporal.ZonedDateTime.prototype.hour, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.hour
// 6.3.12 get Temporal.ZonedDateTime.prototype.minute, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.minute
// 6.3.13 get Temporal.ZonedDateTime.prototype.second, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.second
// 6.3.14 get Temporal.ZonedDateTime.prototype.millisecond, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.millisecond
// 6.3.15 get Temporal.ZonedDateTime.prototype.microsecond, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.microsecond
// 6.3.16 get Temporal.ZonedDateTime.prototype.nanosecond, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.nanosecond
#define JS_ENUMERATE_PLAIN_DATE_TIME_TIME_FIELDS \
    __JS_ENUMERATE(hour)                         \
    __JS_ENUMERATE(minute)                       \
    __JS_ENUMERATE(second)                       \
    __JS_ENUMERATE(millisecond)                  \
    __JS_ENUMERATE(microsecond)                  \
    __JS_ENUMERATE(nanosecond)

#define __JS_ENUMERATE(field)                                                                                                          \
    JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::field##_getter)                                                                  \
    {                                                                                                                                  \
        /* 1. Let zonedDateTime be the this value. */                                                                                  \
        /* 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]). */                                   \
        auto zoned_date_time = TRY(typed_this_object(vm));                                                                             \
                                                                                                                                       \
        /* Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]). */                    \
        auto iso_date_time = get_iso_date_time_for(zoned_date_time->time_zone(), zoned_date_time->epoch_nanoseconds()->big_integer()); \
                                                                                                                                       \
        /* 3. Return 𝔽(isoDateTime.[[Time]].[[<field>]]). */                                                                        \
        return iso_date_time.time.field;                                                                                               \
    }
JS_ENUMERATE_PLAIN_DATE_TIME_TIME_FIELDS
#undef __JS_ENUMERATE

// 6.3.17 get Temporal.ZonedDateTime.prototype.epochMilliseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.epochmilliseconds
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::epoch_milliseconds_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let ns be zonedDateTime.[[EpochNanoseconds]].
    auto const& nanoseconds = zoned_date_time->epoch_nanoseconds()->big_integer();

    // 4. Let ms be floor(ℝ(ns) / 10**6).
    auto milliseconds = big_floor(nanoseconds, NANOSECONDS_PER_MILLISECOND);

    // 5. Return 𝔽(ms).
    return milliseconds.to_double();
}

// 6.3.18 get Temporal.ZonedDateTime.prototype.epochNanoseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.epochnanoseconds
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::epoch_nanoseconds_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return zonedDateTime.[[EpochNanoseconds]].
    return zoned_date_time->epoch_nanoseconds();
}

// 6.3.21 get Temporal.ZonedDateTime.prototype.weekOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.weekofyear
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::week_of_year_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
    auto iso_date_time = get_iso_date_time_for(zoned_date_time->time_zone(), zoned_date_time->epoch_nanoseconds()->big_integer());

    // 4. Let result be CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[WeekOfYear]].[[Week]].
    auto result = calendar_iso_to_date(zoned_date_time->calendar(), iso_date_time.iso_date).week_of_year.week;

    // 5. If result is undefined, return undefined.
    if (!result.has_value())
        return js_undefined();

    // 6. Return 𝔽(result).
    return *result;
}

// 6.3.22 get Temporal.ZonedDateTime.prototype.yearOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.yearofweek
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::year_of_week_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
    auto iso_date_time = get_iso_date_time_for(zoned_date_time->time_zone(), zoned_date_time->epoch_nanoseconds()->big_integer());

    // 4. Let result be CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[WeekOfYear]].[[Year]].
    auto result = calendar_iso_to_date(zoned_date_time->calendar(), iso_date_time.iso_date).week_of_year.year;

    // 5. If result is undefined, return undefined.
    if (!result.has_value())
        return js_undefined();

    // 6. Return 𝔽(result).
    return *result;
}

// 6.3.23 get Temporal.ZonedDateTime.prototype.hoursInDay, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.hoursinday
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::hours_in_day_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto const& time_zone = zoned_date_time->time_zone();

    // 4. Let isoDateTime be GetISODateTimeFor(timeZone, zonedDateTime.[[EpochNanoseconds]]).
    auto iso_date_time = get_iso_date_time_for(time_zone, zoned_date_time->epoch_nanoseconds()->big_integer());

    // 5. Let today be isoDateTime.[[ISODate]].
    auto today = iso_date_time.iso_date;

    // 6. Let tomorrow be BalanceISODate(today.[[Year]], today.[[Month]], today.[[Day]] + 1).
    auto tomorrow = balance_iso_date(today.year, today.month, today.day + 1);

    // 7. Let todayNs be ? GetStartOfDay(timeZone, today).
    auto today_nanoseconds = TRY(get_start_of_day(vm, time_zone, today));

    // 8. Let tomorrowNs be ? GetStartOfDay(timeZone, tomorrow).
    auto tomorrow_nanoseconds = TRY(get_start_of_day(vm, time_zone, tomorrow));

    // 9. Let diff be TimeDurationFromEpochNanosecondsDifference(tomorrowNs, todayNs).
    auto diff = time_duration_from_epoch_nanoseconds_difference(tomorrow_nanoseconds, today_nanoseconds);

    // 10. Return 𝔽(TotalTimeDuration(diff, HOUR)).
    return total_time_duration(diff, Unit::Hour).to_double();
}

// 6.3.29 get Temporal.ZonedDateTime.prototype.offsetNanoseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.offsetnanoseconds
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::offset_nanoseconds_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return 𝔽(GetOffsetNanosecondsFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]])).
    return static_cast<double>(get_offset_nanoseconds_for(zoned_date_time->time_zone(), zoned_date_time->epoch_nanoseconds()->big_integer()));
}

// 6.3.30 get Temporal.ZonedDateTime.prototype.offset, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.offset
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::offset_getter)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let offsetNanoseconds be GetOffsetNanosecondsFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
    auto offset_nanoseconds = get_offset_nanoseconds_for(zoned_date_time->time_zone(), zoned_date_time->epoch_nanoseconds()->big_integer());

    // 4. Return FormatUTCOffsetNanoseconds(offsetNanoseconds).
    return PrimitiveString::create(vm, format_utc_offset_nanoseconds(offset_nanoseconds));
}

// 6.3.31 Temporal.ZonedDateTime.prototype.with ( temporalZonedDateTimeLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.with
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::with)
{
    auto temporal_zoned_date_time_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. If ? IsPartialTemporalObject(temporalZonedDateTimeLike) is false, throw a TypeError exception.
    if (!TRY(is_partial_temporal_object(vm, temporal_zoned_date_time_like)))
        return vm.throw_completion<TypeError>(ErrorType::TemporalObjectMustBePartialTemporalObject);

    // 4. Let epochNs be zonedDateTime.[[EpochNanoseconds]].
    auto const& epoch_nanoseconds = zoned_date_time->epoch_nanoseconds()->big_integer();

    // 5. Let timeZone be zonedDateTime.[[TimeZone]].
    auto const& time_zone = zoned_date_time->time_zone();

    // 6. Let calendar be zonedDateTime.[[Calendar]].
    auto const& calendar = zoned_date_time->calendar();

    // 7. Let offsetNanoseconds be GetOffsetNanosecondsFor(timeZone, epochNs).
    auto offset_nanoseconds = get_offset_nanoseconds_for(time_zone, epoch_nanoseconds);

    // 8. Let isoDateTime be GetISODateTimeFor(timeZone, epochNs).
    auto iso_date_time = get_iso_date_time_for(time_zone, epoch_nanoseconds);

    // 9. Let fields be ISODateToFields(calendar, isoDateTime.[[ISODate]], DATE).
    auto fields = iso_date_to_fields(calendar, iso_date_time.iso_date, DateType::Date);

    // 10. Set fields.[[Hour]] to isoDateTime.[[Time]].[[Hour]].
    fields.hour = iso_date_time.time.hour;

    // 11. Set fields.[[Minute]] to isoDateTime.[[Time]].[[Minute]].
    fields.minute = iso_date_time.time.minute;

    // 12. Set fields.[[Second]] to isoDateTime.[[Time]].[[Second]].
    fields.second = iso_date_time.time.second;

    // 13. Set fields.[[Millisecond]] to isoDateTime.[[Time]].[[Millisecond]].
    fields.millisecond = iso_date_time.time.millisecond;

    // 14. Set fields.[[Microsecond]] to isoDateTime.[[Time]].[[Microsecond]].
    fields.microsecond = iso_date_time.time.microsecond;

    // 15. Set fields.[[Nanosecond]] to isoDateTime.[[Time]].[[Nanosecond]].
    fields.nanosecond = iso_date_time.time.nanosecond;

    // 16. Set fields.[[OffsetString]] to FormatUTCOffsetNanoseconds(offsetNanoseconds).
    fields.offset = format_utc_offset_nanoseconds(offset_nanoseconds);

    // 17. Let partialZonedDateTime be ? PrepareCalendarFields(calendar, temporalZonedDateTimeLike, « YEAR, MONTH, MONTH-CODE, DAY », « HOUR, MINUTE, SECOND, MILLISECOND, MICROSECOND, NANOSECOND, OFFSET », PARTIAL).
    static constexpr auto calendar_field_names = to_array({ CalendarField::Year, CalendarField::Month, CalendarField::MonthCode, CalendarField::Day });
    static constexpr auto non_calendar_field_names = to_array({ CalendarField::Hour, CalendarField::Minute, CalendarField::Second, CalendarField::Millisecond, CalendarField::Microsecond, CalendarField::Nanosecond, CalendarField::Offset });
    auto partial_zoned_date_time = TRY(prepare_calendar_fields(vm, calendar, temporal_zoned_date_time_like.as_object(), calendar_field_names, non_calendar_field_names, Partial {}));

    // 18. Set fields to CalendarMergeFields(calendar, fields, partialZonedDateTime).
    fields = calendar_merge_fields(calendar, fields, partial_zoned_date_time);

    // 19. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 20. Let disambiguation be ? GetTemporalDisambiguationOption(resolvedOptions).
    auto disambiguation = TRY(get_temporal_disambiguation_option(vm, resolved_options));

    // 21. Let offset be ? GetTemporalOffsetOption(resolvedOptions, PREFER).
    auto offset = TRY(get_temporal_offset_option(vm, resolved_options, OffsetOption::Prefer));

    // 22. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
    auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

    // 23. Let dateTimeResult be ? InterpretTemporalDateTimeFields(calendar, fields, overflow).
    auto date_time_result = TRY(interpret_temporal_date_time_fields(vm, calendar, fields, overflow));

    // 24. Let newOffsetNanoseconds be ! ParseDateTimeUTCOffset(fields.[[OffsetString]]).
    auto new_offset_nanoseconds = parse_date_time_utc_offset(*fields.offset);

    // 25. Let epochNanoseconds be ? InterpretISODateTimeOffset(dateTimeResult.[[ISODate]], dateTimeResult.[[Time]], OPTION, newOffsetNanoseconds, timeZone, disambiguation, offset, MATCH-EXACTLY).
    auto new_epoch_nanoseconds = TRY(interpret_iso_date_time_offset(vm, date_time_result.iso_date, date_time_result.time, OffsetBehavior::Option, new_offset_nanoseconds, time_zone, disambiguation, offset, MatchBehavior::MatchExactly));

    // 26. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, BigInt::create(vm, move(new_epoch_nanoseconds)), time_zone, calendar));
}

// 6.3.32 Temporal.ZonedDateTime.prototype.withPlainTime ( [ plainTimeLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.withplaintime
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::with_plain_time)
{
    auto plain_time_like = vm.argument(0);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto const& time_zone = zoned_date_time->time_zone();

    // 4. Let calendar be zonedDateTime.[[Calendar]].
    auto const& calendar = zoned_date_time->calendar();

    // 5. Let isoDateTime be GetISODateTimeFor(timeZone, zonedDateTime.[[EpochNanoseconds]]).
    auto iso_date_time = get_iso_date_time_for(time_zone, zoned_date_time->epoch_nanoseconds()->big_integer());

    Crypto::SignedBigInteger epoch_nanoseconds;

    // 6. If plainTimeLike is undefined, then
    if (plain_time_like.is_undefined()) {
        // a. Let epochNs be ? GetStartOfDay(timeZone, isoDateTime.[[ISODate]]).
        epoch_nanoseconds = TRY(get_start_of_day(vm, time_zone, iso_date_time.iso_date));
    }
    // 7. Else,
    else {
        // a. Let plainTime be ? ToTemporalTime(plainTimeLike).
        auto plain_time = TRY(to_temporal_time(vm, plain_time_like));

        // b. Let resultISODateTime be CombineISODateAndTimeRecord(isoDateTime.[[ISODate]], plainTime.[[Time]]).
        auto result_iso_date_time = combine_iso_date_and_time_record(iso_date_time.iso_date, plain_time->time());

        // c. Let epochNs be ? GetEpochNanosecondsFor(timeZone, resultISODateTime, COMPATIBLE).
        epoch_nanoseconds = TRY(get_epoch_nanoseconds_for(vm, time_zone, result_iso_date_time, Disambiguation::Compatible));
    }

    // 8. Return ! CreateTemporalZonedDateTime(epochNs, timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, BigInt::create(vm, move(epoch_nanoseconds)), time_zone, calendar));
}

// 6.3.33 Temporal.ZonedDateTime.prototype.withTimeZone ( timeZoneLike ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.withtimezone
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::with_time_zone)
{
    auto time_zone_like = vm.argument(0);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be ? ToTemporalTimeZoneIdentifier(timeZoneLike).
    auto time_zone = TRY(to_temporal_time_zone_identifier(vm, time_zone_like));

    // 4. Return ! CreateTemporalZonedDateTime(zonedDateTime.[[EpochNanoseconds]], timeZone, zonedDateTime.[[Calendar]]).
    return MUST(create_temporal_zoned_date_time(vm, zoned_date_time->epoch_nanoseconds(), move(time_zone), zoned_date_time->calendar()));
}

// 6.3.34 Temporal.ZonedDateTime.prototype.withCalendar ( calendarLike ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.withcalendar
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::with_calendar)
{
    auto calendar_like = vm.argument(0);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be ? ToTemporalCalendarIdentifier(calendarLike).
    auto calendar = TRY(to_temporal_calendar_identifier(vm, calendar_like));

    // 4. Return ! CreateTemporalZonedDateTime(zonedDateTime.[[EpochNanoseconds]], zonedDateTime.[[TimeZone]], calendar).
    return MUST(create_temporal_zoned_date_time(vm, zoned_date_time->epoch_nanoseconds(), zoned_date_time->time_zone(), move(calendar)));
}

// 6.3.35 Temporal.ZonedDateTime.prototype.add ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.add
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::add)
{
    auto temporal_duration_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToZonedDateTime(ADD, zonedDateTime, temporalDurationLike, options).
    return TRY(add_duration_to_zoned_date_time(vm, ArithmeticOperation::Add, zoned_date_time, temporal_duration_like, options));
}

// 6.3.36 Temporal.ZonedDateTime.prototype.subtract ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.subtract
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::subtract)
{
    auto temporal_duration_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToZonedDateTime(SUBTRACT, zonedDateTime, temporalDurationLike, options).
    return TRY(add_duration_to_zoned_date_time(vm, ArithmeticOperation::Subtract, zoned_date_time, temporal_duration_like, options));
}

// 6.3.37 Temporal.ZonedDateTime.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.until
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::until)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalZonedDateTime(UNTIL, zonedDateTime, other, options).
    return TRY(difference_temporal_zoned_date_time(vm, DurationOperation::Until, zoned_date_time, other, options));
}

// 6.3.38 Temporal.ZonedDateTime.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.since
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::since)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalZonedDateTime(SINCE, zonedDateTime, other, options).
    return TRY(difference_temporal_zoned_date_time(vm, DurationOperation::Since, zoned_date_time, other, options));
}

// 6.3.39 Temporal.ZonedDateTime.prototype.round ( roundTo ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.round
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::round)
{
    auto& realm = *vm.current_realm();

    auto round_to_value = vm.argument(0);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

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
        // a. Return ! CreateTemporalZonedDateTime(zonedDateTime.[[EpochNanoseconds]], zonedDateTime.[[TimeZone]], zonedDateTime.[[Calendar]]).
        return MUST(create_temporal_zoned_date_time(vm, zoned_date_time->epoch_nanoseconds(), zoned_date_time->time_zone(), zoned_date_time->calendar()));
    }

    // 14. Let thisNs be zonedDateTime.[[EpochNanoseconds]].
    auto const& this_nanoseconds = zoned_date_time->epoch_nanoseconds()->big_integer();

    // 15. Let timeZone be zonedDateTime.[[TimeZone]].
    auto const& time_zone = zoned_date_time->time_zone();

    // 16. Let calendar be zonedDateTime.[[Calendar]].
    auto const& calendar = zoned_date_time->calendar();

    // 17. Let isoDateTime be GetISODateTimeFor(timeZone, thisNs).
    auto iso_date_time = get_iso_date_time_for(time_zone, this_nanoseconds);

    Crypto::SignedBigInteger epoch_nanoseconds;

    // 18. If smallestUnit is day, then
    if (smallest_unit_value == Unit::Day) {
        // a. Let dateStart be isoDateTime.[[ISODate]].
        auto date_start = iso_date_time.iso_date;

        // b. Let dateEnd be BalanceISODate(dateStart.[[Year]], dateStart.[[Month]], dateStart.[[Day]] + 1).
        auto date_end = balance_iso_date(date_start.year, date_start.month, static_cast<double>(date_start.day) + 1);

        // c. Let startNs be ? GetStartOfDay(timeZone, dateStart).
        auto start_nanoseconds = TRY(get_start_of_day(vm, time_zone, date_start));

        // d. Assert: thisNs ≥ startNs.
        VERIFY(this_nanoseconds >= start_nanoseconds);

        // e. Let endNs be ? GetStartOfDay(timeZone, dateEnd).
        auto end_nanoseconds = TRY(get_start_of_day(vm, time_zone, date_end));

        // f. Assert: thisNs < endNs.
        VERIFY(this_nanoseconds < end_nanoseconds);

        // g. Let dayLengthNs be ℝ(endNs - startNs).
        auto day_length_nanoseconds = end_nanoseconds.minus(start_nanoseconds);

        // h. Let dayProgressNs be TimeDurationFromEpochNanosecondsDifference(thisNs, startNs).
        auto day_progress_nanoseconds = time_duration_from_epoch_nanoseconds_difference(this_nanoseconds, start_nanoseconds);

        // i. Let roundedDayNs be ! RoundTimeDurationToIncrement(dayProgressNs, dayLengthNs, roundingMode).
        auto rounded_day_nanoseconds = MUST(round_time_duration_to_increment(vm, day_progress_nanoseconds, day_length_nanoseconds.unsigned_value(), rounding_mode));

        // j. Let epochNanoseconds be AddTimeDurationToEpochNanoseconds(startNs, roundedDayNs).
        epoch_nanoseconds = add_time_duration_to_epoch_nanoseconds(start_nanoseconds, rounded_day_nanoseconds);
    }
    // 19. Else,
    else {
        // a. Let roundResult be RoundISODateTime(isoDateTime, roundingIncrement, smallestUnit, roundingMode).
        auto round_result = round_iso_date_time(iso_date_time, rounding_increment, smallest_unit_value, rounding_mode);

        // b. Let offsetNanoseconds be GetOffsetNanosecondsFor(timeZone, thisNs).
        auto offset_nanoseconds = get_offset_nanoseconds_for(time_zone, this_nanoseconds);

        // c. Let epochNanoseconds be ? InterpretISODateTimeOffset(roundResult.[[ISODate]], roundResult.[[Time]], OPTION, offsetNanoseconds, timeZone, COMPATIBLE, PREFER, MATCH-EXACTLY).
        epoch_nanoseconds = TRY(interpret_iso_date_time_offset(vm, round_result.iso_date, round_result.time, OffsetBehavior::Option, offset_nanoseconds, time_zone, Disambiguation::Compatible, OffsetOption::Prefer, MatchBehavior::MatchExactly));
    }

    // 20. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, BigInt::create(vm, move(epoch_nanoseconds)), time_zone, calendar));
}

// 6.3.40 Temporal.ZonedDateTime.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.equals
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::equals)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Set other to ? ToTemporalZonedDateTime(other).
    auto other = TRY(to_temporal_zoned_date_time(vm, vm.argument(0)));

    // 4. If zonedDateTime.[[EpochNanoseconds]] ≠ other.[[EpochNanoseconds]], return false.
    if (zoned_date_time->epoch_nanoseconds()->big_integer() != other->epoch_nanoseconds()->big_integer())
        return false;

    // 5. If TimeZoneEquals(zonedDateTime.[[TimeZone]], other.[[TimeZone]]) is false, return false.
    if (!time_zone_equals(zoned_date_time->time_zone(), other->time_zone()))
        return false;

    // 6. Return CalendarEquals(zonedDateTime.[[Calendar]], other.[[Calendar]]).
    return calendar_equals(zoned_date_time->calendar(), other->calendar());
}

// 6.3.41 Temporal.ZonedDateTime.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_string)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, vm.argument(0)));

    // 4. NOTE: The following steps read options and perform independent validation in alphabetical order
    //    (GetTemporalShowCalendarNameOption reads "calendarName", GetTemporalFractionalSecondDigitsOption reads
    //    "fractionalSecondDigits", GetTemporalShowOffsetOption reads "offset", and GetRoundingModeOption reads "roundingMode").

    // 5. Let showCalendar be ? GetTemporalShowCalendarNameOption(resolvedOptions).
    auto show_calendar = TRY(get_temporal_show_calendar_name_option(vm, resolved_options));

    // 6. Let digits be ? GetTemporalFractionalSecondDigitsOption(resolvedOptions).
    auto digits = TRY(get_temporal_fractional_second_digits_option(vm, resolved_options));

    // 7. Let showOffset be ? GetTemporalShowOffsetOption(resolvedOptions).
    auto show_offset = TRY(get_temporal_show_offset_option(vm, resolved_options));

    // 8. Let roundingMode be ? GetRoundingModeOption(resolvedOptions, TRUNC).
    auto rounding_mode = TRY(get_rounding_mode_option(vm, resolved_options, RoundingMode::Trunc));

    // 9. Let smallestUnit be ? GetTemporalUnitValuedOption(resolvedOptions, "smallestUnit", TIME, UNSET).
    auto smallest_unit = TRY(get_temporal_unit_valued_option(vm, resolved_options, vm.names.smallestUnit, UnitGroup::Time, Unset {}));

    // 10. If smallestUnit is hour, throw a RangeError exception.
    if (auto const* unit = smallest_unit.get_pointer<Unit>(); unit && *unit == Unit::Hour)
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, temporal_unit_to_string(*unit), vm.names.smallestUnit);

    // 11. Let showTimeZone be ? GetTemporalShowTimeZoneNameOption(resolvedOptions).
    auto show_time_zone = TRY(get_temporal_show_time_zone_name_option(vm, resolved_options));

    // 12. Let precision be ToSecondsStringPrecisionRecord(smallestUnit, digits).
    auto precision = to_seconds_string_precision_record(smallest_unit, digits);

    // 13. Return TemporalZonedDateTimeToString(zonedDateTime, precision.[[Precision]], showCalendar, showTimeZone, showOffset, precision.[[Increment]], precision.[[Unit]], roundingMode).
    return PrimitiveString::create(vm, temporal_zoned_date_time_to_string(zoned_date_time, precision.precision, show_calendar, show_time_zone, show_offset, precision.increment, precision.unit, rounding_mode));
}

// 6.3.42 Temporal.ZonedDateTime.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tolocalestring
// NOTE: This is the minimum toLocaleString implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_locale_string)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return TemporalZonedDateTimeToString(zonedDateTime, AUTO, AUTO, AUTO, AUTO).
    return PrimitiveString::create(vm, temporal_zoned_date_time_to_string(zoned_date_time, Auto {}, ShowCalendar::Auto, ShowTimeZoneName::Auto, ShowOffset::Auto));
}

// 6.3.43 Temporal.ZonedDateTime.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_json)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return TemporalZonedDateTimeToString(zonedDateTime, AUTO, AUTO, AUTO, AUTO).
    return PrimitiveString::create(vm, temporal_zoned_date_time_to_string(zoned_date_time, Auto {}, ShowCalendar::Auto, ShowTimeZoneName::Auto, ShowOffset::Auto));
}

// 6.3.44 Temporal.ZonedDateTime.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.ZonedDateTime", "a primitive value");
}

// 6.3.45 Temporal.ZonedDateTime.prototype.startOfDay ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.startofday
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::start_of_day)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto const& time_zone = zoned_date_time->time_zone();

    // 4. Let calendar be zonedDateTime.[[Calendar]].
    auto const& calendar = zoned_date_time->calendar();

    // 5. Let isoDateTime be GetISODateTimeFor(timeZone, zonedDateTime.[[EpochNanoseconds]]).
    auto iso_date_time = get_iso_date_time_for(time_zone, zoned_date_time->epoch_nanoseconds()->big_integer());

    // 6. Let epochNanoseconds be ? GetStartOfDay(timeZone, isoDateTime.[[ISODate]]).
    auto epoch_nanoseconds = TRY(get_start_of_day(vm, time_zone, iso_date_time.iso_date));

    // 7. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, BigInt::create(vm, move(epoch_nanoseconds)), time_zone, calendar));
}

// 6.3.46 Temporal.ZonedDateTime.prototype.getTimeZoneTransition ( directionParam ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.gettimezonetransition
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::get_time_zone_transition)
{
    auto& realm = *vm.current_realm();

    auto direction_param_value = vm.argument(0);

    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be zonedDateTime.[[TimeZone]].
    auto const& time_zone = zoned_date_time->time_zone();

    // 4. If directionParam is undefined, throw a TypeError exception.
    if (direction_param_value.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "Transition direction parameter"sv);

    GC::Ptr<Object> direction_param;

    // 5. If directionParam is a String, then
    if (direction_param_value.is_string()) {
        // a. Let paramString be directionParam.
        auto param_string = direction_param_value;

        // b. Set directionParam to OrdinaryObjectCreate(null).
        direction_param = Object::create(realm, nullptr);

        // c. Perform ! CreateDataPropertyOrThrow(directionParam, "direction", paramString).
        MUST(direction_param->create_data_property_or_throw(vm.names.direction, param_string));
    }
    // 6. Else,
    else {
        // a. Set directionParam to ? GetOptionsObject(directionParam).
        direction_param = TRY(get_options_object(vm, direction_param_value));
    }

    // 7. Let direction be ? GetDirectionOption(directionParam).
    auto direction = TRY(get_direction_option(vm, *direction_param));

    // 8. If IsOffsetTimeZoneIdentifier(timeZone) is true, return null.
    if (is_offset_time_zone_identifier(time_zone))
        return js_null();

    Optional<Crypto::SignedBigInteger> transition;

    switch (direction) {
    // 9. If direction is NEXT, then
    case Direction::Next:
        // a. Let transition be GetNamedTimeZoneNextTransition(timeZone, zonedDateTime.[[EpochNanoseconds]]).
        transition = get_named_time_zone_next_transition(time_zone, zoned_date_time->epoch_nanoseconds()->big_integer());
        break;
    // 10. Else,
    case Direction::Previous:
        // a. Assert: direction is PREVIOUS.
        // b. Let transition be GetNamedTimeZonePreviousTransition(timeZone, zonedDateTime.[[EpochNanoseconds]]).
        transition = get_named_time_zone_previous_transition(time_zone, zoned_date_time->epoch_nanoseconds()->big_integer());
        break;
    }

    // 11. If transition is null, return null.
    if (!transition.has_value())
        return js_null();

    // 12. Return ! CreateTemporalZonedDateTime(transition, timeZone, zonedDateTime.[[Calendar]]).
    return MUST(create_temporal_zoned_date_time(vm, BigInt::create(vm, transition.release_value()), time_zone, zoned_date_time->calendar()));
}

// 6.3.47 Temporal.ZonedDateTime.prototype.toInstant ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toinstant
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_instant)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Return ! CreateTemporalInstant(zonedDateTime.[[EpochNanoseconds]]).
    return MUST(create_temporal_instant(vm, zoned_date_time->epoch_nanoseconds()));
}

// 6.3.48 Temporal.ZonedDateTime.prototype.toPlainDate ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplaindate
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_plain_date)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
    auto iso_date_time = get_iso_date_time_for(zoned_date_time->time_zone(), zoned_date_time->epoch_nanoseconds()->big_integer());

    // 4. Return ! CreateTemporalDate(isoDateTime.[[ISODate]], zonedDateTime.[[Calendar]].).
    return MUST(create_temporal_date(vm, iso_date_time.iso_date, zoned_date_time->calendar()));
}

// 6.3.49 Temporal.ZonedDateTime.prototype.toPlainTime ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplaintime
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_plain_time)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
    auto iso_date_time = get_iso_date_time_for(zoned_date_time->time_zone(), zoned_date_time->epoch_nanoseconds()->big_integer());

    // 4. Return ! CreateTemporalTime(isoDateTime.[[Time]]).
    return MUST(create_temporal_time(vm, iso_date_time.time));
}

// 6.3.50 Temporal.ZonedDateTime.prototype.toPlainDateTime ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplaindatetime
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::to_plain_date_time)
{
    // 1. Let zonedDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
    auto zoned_date_time = TRY(typed_this_object(vm));

    // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
    auto iso_date_time = get_iso_date_time_for(zoned_date_time->time_zone(), zoned_date_time->epoch_nanoseconds()->big_integer());

    // 4. Return ! CreateTemporalDateTime(isoDateTime, zonedDateTime.[[Calendar]]).
    return MUST(create_temporal_date_time(vm, iso_date_time, zoned_date_time->calendar()));
}

}
