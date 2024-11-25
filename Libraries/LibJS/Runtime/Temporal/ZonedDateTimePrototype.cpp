/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
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
    define_native_function(realm, vm.names.valueOf, value_of, 0, attr);
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

// 6.3.44 Temporal.ZonedDateTime.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(ZonedDateTimePrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.ZonedDateTime", "a primitive value");
}

}
