/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainDateTimePrototype.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(PlainDateTimePrototype);

// 5.3 Properties of the Temporal.PlainDateTime Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plaindatetime-prototype-object
PlainDateTimePrototype::PlainDateTimePrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void PlainDateTimePrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 5.3.2 Temporal.PlainDateTime.prototype[ @@toStringTag ], https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.PlainDateTime"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.calendar, calendar_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.calendarId, calendar_id_getter, {}, Attribute::Configurable);
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
    define_native_accessor(realm, vm.names.era, era_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.eraYear, era_year_getter, {}, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.with, with, 1, attr);
    define_native_function(realm, vm.names.withPlainTime, with_plain_time, 0, attr);
    define_native_function(realm, vm.names.withPlainDate, with_plain_date, 1, attr);
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
    define_native_function(realm, vm.names.toPlainYearMonth, to_plain_year_month, 0, attr);
    define_native_function(realm, vm.names.toPlainMonthDay, to_plain_month_day, 0, attr);
    define_native_function(realm, vm.names.toPlainTime, to_plain_time, 0, attr);
    define_native_function(realm, vm.names.getISOFields, get_iso_fields, 0, attr);
}

// 5.3.3 get Temporal.PlainDateTime.prototype.calendar, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.calendar
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::calendar_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return dateTime.[[Calendar]].
    return Value(&date_time->calendar());
}

// 5.3.4 get Temporal.PlainDateTime.prototype.year, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.year
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::year_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return 𝔽(? CalendarYear(calendar, dateTime)).
    return TRY(calendar_year(vm, calendar, date_time));
}

// 5.3.5 get Temporal.PlainDateTime.prototype.month, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.month
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::month_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return 𝔽(? CalendarMonth(calendar, dateTime)).
    return TRY(calendar_month(vm, calendar, date_time));
}

// 5.3.6 get Temporal.PlainDateTime.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.monthcode
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::month_code_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return ? CalendarMonthCode(calendar, dateTime).
    return PrimitiveString::create(vm, TRY(calendar_month_code(vm, calendar, date_time)));
}

// 5.3.7 get Temporal.PlainDateTime.prototype.day, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.day
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::day_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return 𝔽(? CalendarDay(calendar, dateTime)).
    return TRY(calendar_day(vm, calendar, date_time));
}

// 5.3.8 get Temporal.PlainDateTime.prototype.hour, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.hour
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::hour_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return 𝔽(dateTime.[[ISOHour]]).
    return Value(date_time->iso_hour());
}

// 5.3.9 get Temporal.PlainDateTime.prototype.minute, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.minute
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::minute_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return 𝔽(dateTime.[[ISOMinute]]).
    return Value(date_time->iso_minute());
}

// 5.3.10 get Temporal.PlainDateTime.prototype.second, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.second
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::second_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return 𝔽(dateTime.[[ISOSecond]]).
    return Value(date_time->iso_second());
}

// 5.3.11 get Temporal.PlainDateTime.prototype.millisecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.millisecond
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::millisecond_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return 𝔽(dateTime.[[ISOMillisecond]]).
    return Value(date_time->iso_millisecond());
}

// 5.3.12 get Temporal.PlainDateTime.prototype.microsecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.microsecond
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::microsecond_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return 𝔽(dateTime.[[ISOMicrosecond]]).
    return Value(date_time->iso_microsecond());
}

// 5.3.13 get Temporal.PlainDateTime.prototype.nanosecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.nanosecond
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::nanosecond_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return 𝔽(dateTime.[[ISONanosecond]]).
    return Value(date_time->iso_nanosecond());
}

// 5.3.14 get Temporal.PlainDateTime.prototype.dayOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.dayofweek
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::day_of_week_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return 𝔽(? CalendarDayOfWeek(calendar, dateTime)).
    return TRY(calendar_day_of_week(vm, calendar, date_time));
}

// 5.3.15 get Temporal.PlainDateTime.prototype.dayOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.dayofyear
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::day_of_year_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return 𝔽(? CalendarDayOfYear(calendar, dateTime)).
    return TRY(calendar_day_of_year(vm, calendar, date_time));
}

// 5.3.16 get Temporal.PlainDateTime.prototype.weekOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.weekofyear
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::week_of_year_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return 𝔽(? CalendarWeekOfYear(calendar, dateTime)).
    return TRY(calendar_week_of_year(vm, calendar, date_time));
}

// 5.3.17 get Temporal.PlainDateTime.prototype.yearOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.yearofweek
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::year_of_week_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return 𝔽(? CalendarYearOfWeek(calendar, dateTime)).
    return TRY(calendar_year_of_week(vm, calendar, date_time));
}

// 5.3.18 get Temporal.PlainDateTime.prototype.daysInWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.daysinweek
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::days_in_week_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return 𝔽(? CalendarDaysInWeek(calendar, dateTime)).
    return TRY(calendar_days_in_week(vm, calendar, date_time));
}

// 5.3.19 get Temporal.PlainDateTime.prototype.daysInMonth, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.daysinmonth
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::days_in_month_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return 𝔽(? CalendarDaysInMonth(calendar, dateTime)).
    return TRY(calendar_days_in_month(vm, calendar, date_time));
}

// 5.3.20 get Temporal.PlainDateTime.prototype.daysInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.daysinyear
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::days_in_year_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return 𝔽(? CalendarDaysInYear(calendar, dateTime)).
    return TRY(calendar_days_in_year(vm, calendar, date_time));
}

// 5.3.21 get Temporal.PlainDateTime.prototype.monthsInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.monthsinyear
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::months_in_year_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return 𝔽(? CalendarMonthsInYear(calendar, dateTime)).
    return TRY(calendar_months_in_year(vm, calendar, date_time));
}

// 5.3.22 get Temporal.PlainDateTime.prototype.inLeapYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.inleapyear
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::in_leap_year_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Return ? CalendarInLeapYear(calendar, dateTime).
    return TRY(calendar_in_leap_year(vm, calendar, date_time));
}

// 15.6.6.2 get Temporal.PlainDateTime.prototype.era, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.era
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::era_getter)
{
    // 1. Let plainDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
    auto plain_date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be plainDateTime.[[Calendar]].
    auto& calendar = plain_date_time->calendar();

    // 4. Return ? CalendarEra(calendar, plainDateTime).
    return TRY(calendar_era(vm, calendar, plain_date_time));
}

// 15.6.6.3 get Temporal.PlainDateTime.prototype.eraYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.erayear
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::era_year_getter)
{
    // 1. Let plainDateTime be the this value.
    // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
    auto plain_date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be plainDateTime.[[Calendar]].
    auto& calendar = plain_date_time->calendar();

    // 4. Return ? CalendarEraYear(calendar, plainDateTime).
    return TRY(calendar_era_year(vm, calendar, plain_date_time));
}

// 5.3.23 Temporal.PlainDateTime.prototype.with ( temporalDateTimeLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.with
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::with)
{
    auto temporal_date_time_like = vm.argument(0);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. If Type(temporalDateTimeLike) is not Object, then
    if (!temporal_date_time_like.is_object()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, temporal_date_time_like.to_string_without_side_effects());
    }

    // 4. Perform ? RejectObjectWithCalendarOrTimeZone(temporalDateTimeLike).
    TRY(reject_object_with_calendar_or_time_zone(vm, temporal_date_time_like.as_object()));

    // 5. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 6. Let fieldNames be ? CalendarFields(calendar, « "day", "hour", "microsecond", "millisecond", "minute", "month", "monthCode", "nanosecond", "second", "year" »).
    auto field_names = TRY(calendar_fields(vm, calendar, { "day"sv, "hour"sv, "microsecond"sv, "millisecond"sv, "minute"sv, "month"sv, "monthCode"sv, "nanosecond"sv, "second"sv, "year"sv }));

    // 7. Let partialDateTime be ? PrepareTemporalFields(temporalDateTimeLike, fieldNames, partial).
    auto* partial_date_time = TRY(prepare_temporal_fields(vm, temporal_date_time_like.as_object(), field_names, PrepareTemporalFieldsPartial {}));

    // 8. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(1)));

    // 9. Let fields be ? PrepareTemporalFields(dateTime, fieldNames, «»).
    auto* fields = TRY(prepare_temporal_fields(vm, date_time, field_names, Vector<StringView> {}));

    // 10. Set fields to ? CalendarMergeFields(calendar, fields, partialDateTime).
    fields = TRY(calendar_merge_fields(vm, calendar, *fields, *partial_date_time));

    // 11. Set fields to ? PrepareTemporalFields(fields, fieldNames, «»).
    fields = TRY(prepare_temporal_fields(vm, *fields, field_names, Vector<StringView> {}));

    // 12. Let result be ? InterpretTemporalDateTimeFields(calendar, fields, options).
    auto result = TRY(interpret_temporal_date_time_fields(vm, calendar, *fields, options));

    // 13. Assert: IsValidISODate(result.[[Year]], result.[[Month]], result.[[Day]]) is true.
    VERIFY(is_valid_iso_date(result.year, result.month, result.day));

    // 14. Assert: IsValidTime(result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]]) is true.
    VERIFY(is_valid_time(result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond));

    // 15. Return ? CreateTemporalDateTime(result.[[Year]], result.[[Month]], result.[[Day]], result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]], calendar).
    return TRY(create_temporal_date_time(vm, result.year, result.month, result.day, result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond, calendar));
}

// 5.3.24 Temporal.PlainDateTime.prototype.withPlainTime ( [ plainTimeLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.withplaintime
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::with_plain_time)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. If plainTimeLike is undefined, then
    if (vm.argument(0).is_undefined()) {
        // a. Return ? CreateTemporalDateTime(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], 0, 0, 0, 0, 0, 0, dateTime.[[Calendar]]).
        return TRY(create_temporal_date_time(vm, date_time->iso_year(), date_time->iso_month(), date_time->iso_day(), 0, 0, 0, 0, 0, 0, date_time->calendar()));
    }

    // 4. Let plainTime be ? ToTemporalTime(plainTimeLike).
    auto* plain_time = TRY(to_temporal_time(vm, vm.argument(0)));

    // 5. Return ? CreateTemporalDateTime(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], plainTime.[[ISOHour]], plainTime.[[ISOMinute]], plainTime.[[ISOSecond]], plainTime.[[ISOMillisecond]], plainTime.[[ISOMicrosecond]], plainTime.[[ISONanosecond]], dateTime.[[Calendar]]).
    return TRY(create_temporal_date_time(vm, date_time->iso_year(), date_time->iso_month(), date_time->iso_day(), plain_time->iso_hour(), plain_time->iso_minute(), plain_time->iso_second(), plain_time->iso_millisecond(), plain_time->iso_microsecond(), plain_time->iso_nanosecond(), date_time->calendar()));
}

// 5.3.25 Temporal.PlainDateTime.prototype.withPlainDate ( plainDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.withplaindate
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::with_plain_date)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let plainDate be ? ToTemporalDate(plainDateLike).
    auto* plain_date = TRY(to_temporal_date(vm, vm.argument(0)));

    // 4. Let calendar be ? ConsolidateCalendars(dateTime.[[Calendar]], plainDate.[[Calendar]]).
    auto* calendar = TRY(consolidate_calendars(vm, date_time->calendar(), plain_date->calendar()));

    // 5. Return ? CreateTemporalDateTime(plainDate.[[ISOYear]], plainDate.[[ISOMonth]], plainDate.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]], calendar).
    return TRY(create_temporal_date_time(vm, plain_date->iso_year(), plain_date->iso_month(), plain_date->iso_day(), date_time->iso_hour(), date_time->iso_minute(), date_time->iso_second(), date_time->iso_millisecond(), date_time->iso_microsecond(), date_time->iso_nanosecond(), *calendar));
}

// 5.3.26 Temporal.PlainDateTime.prototype.withCalendar ( calendarLike ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.withcalendar
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::with_calendar)
{
    auto calendar_like = vm.argument(0);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be ? ToTemporalCalendar(calendarLike).
    auto* calendar = TRY(to_temporal_calendar(vm, calendar_like));

    // 4. Return ? CreateTemporalDateTime(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]], calendar).
    return TRY(create_temporal_date_time(vm, date_time->iso_year(), date_time->iso_month(), date_time->iso_day(), date_time->iso_hour(), date_time->iso_minute(), date_time->iso_second(), date_time->iso_millisecond(), date_time->iso_microsecond(), date_time->iso_nanosecond(), *calendar));
}

// 5.3.27 Temporal.PlainDateTime.prototype.add ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.add
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::add)
{
    auto temporal_duration_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToOrSubtractDurationFromPlainDateTime(add, dateTime, temporalDurationLike, options).
    return TRY(add_duration_to_or_subtract_duration_from_plain_date_time(vm, ArithmeticOperation::Add, date_time, temporal_duration_like, options));
}

// 5.3.28 Temporal.PlainDateTime.prototype.subtract ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.subtract
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::subtract)
{
    auto temporal_duration_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToOrSubtractDurationFromPlainDateTime(subtract, dateTime, temporalDurationLike, options).
    return TRY(add_duration_to_or_subtract_duration_from_plain_date_time(vm, ArithmeticOperation::Subtract, date_time, temporal_duration_like, options));
}

// 5.3.29 Temporal.PlainDateTime.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.since
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::until)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalPlainDateTime(until, dateTime, other, options).
    return TRY(difference_temporal_plain_date_time(vm, DifferenceOperation::Until, date_time, other, options));
}

// 5.3.30 Temporal.PlainDateTime.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.since
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::since)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalPlainDateTime(since, dateTime, other, options).
    return TRY(difference_temporal_plain_date_time(vm, DifferenceOperation::Since, date_time, other, options));
}

// 5.3.31 Temporal.PlainDateTime.prototype.round ( roundTo ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.round
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::round)
{
    auto& realm = *vm.current_realm();

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

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

    // 11. Perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, false).
    TRY(validate_temporal_rounding_increment(vm, rounding_increment, *maximum, false));

    // 12. Let result be ! RoundISODateTime(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]], roundingIncrement, smallestUnit, roundingMode).
    auto result = round_iso_date_time(date_time->iso_year(), date_time->iso_month(), date_time->iso_day(), date_time->iso_hour(), date_time->iso_minute(), date_time->iso_second(), date_time->iso_millisecond(), date_time->iso_microsecond(), date_time->iso_nanosecond(), rounding_increment, *smallest_unit, rounding_mode);

    // 13. Return ? CreateTemporalDateTime(result.[[Year]], result.[[Month]], result.[[Day]], result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]], dateTime.[[Calendar]]).
    return TRY(create_temporal_date_time(vm, result.year, result.month, result.day, result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond, date_time->calendar()));
}

// 5.3.32 Temporal.PlainDateTime.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.equals
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::equals)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Set other to ? ToTemporalDateTime(other).
    auto* other = TRY(to_temporal_date_time(vm, vm.argument(0)));

    // 4. Let result be ! CompareISODateTime(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]], other.[[ISOYear]], other.[[ISOMonth]], other.[[ISODay]], other.[[ISOHour]], other.[[ISOMinute]], other.[[ISOSecond]], other.[[ISOMillisecond]], other.[[ISOMicrosecond]], other.[[ISONanosecond]]).
    auto result = compare_iso_date_time(date_time->iso_year(), date_time->iso_month(), date_time->iso_day(), date_time->iso_hour(), date_time->iso_minute(), date_time->iso_second(), date_time->iso_millisecond(), date_time->iso_microsecond(), date_time->iso_nanosecond(), other->iso_year(), other->iso_month(), other->iso_day(), other->iso_hour(), other->iso_minute(), other->iso_second(), other->iso_millisecond(), other->iso_microsecond(), other->iso_nanosecond());

    // 5. If result is not 0, return false.
    if (result != 0)
        return Value(false);

    // 6. Return ? CalendarEquals(dateTime.[[Calendar]], other.[[Calendar]]).
    return Value(TRY(calendar_equals(vm, date_time->calendar(), other->calendar())));
}

// 5.3.33 Temporal.PlainDateTime.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_string)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(0)));

    // 4. Let precision be ? ToSecondsStringPrecisionRecord(options).
    auto precision = TRY(to_seconds_string_precision_record(vm, *options));

    // 5. Let roundingMode be ? ToTemporalRoundingMode(options, "trunc").
    auto rounding_mode = TRY(to_temporal_rounding_mode(vm, *options, "trunc"sv));

    // 6. Let showCalendar be ? ToCalendarNameOption(options).
    auto show_calendar = TRY(to_calendar_name_option(vm, *options));

    // 7. Let result be ! RoundISODateTime(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]], precision.[[Increment]], precision.[[Unit]], roundingMode).
    auto result = round_iso_date_time(date_time->iso_year(), date_time->iso_month(), date_time->iso_day(), date_time->iso_hour(), date_time->iso_minute(), date_time->iso_second(), date_time->iso_millisecond(), date_time->iso_microsecond(), date_time->iso_nanosecond(), precision.increment, precision.unit, rounding_mode);

    // 8. Return ? TemporalDateTimeToString(result.[[Year]], result.[[Month]], result.[[Day]], result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]], dateTime.[[Calendar]], precision.[[Precision]], showCalendar).
    return PrimitiveString::create(vm, TRY(temporal_date_time_to_string(vm, result.year, result.month, result.day, result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond, &date_time->calendar(), precision.precision, show_calendar)));
}

// 5.3.34 Temporal.PlainDateTime.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tolocalestring
// NOTE: This is the minimum toLocaleString implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_locale_string)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ? TemporalDateTimeToString(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]], dateTime.[[Calendar]], "auto", "auto").
    return PrimitiveString::create(vm, TRY(temporal_date_time_to_string(vm, date_time->iso_year(), date_time->iso_month(), date_time->iso_day(), date_time->iso_hour(), date_time->iso_minute(), date_time->iso_second(), date_time->iso_millisecond(), date_time->iso_microsecond(), date_time->iso_nanosecond(), &date_time->calendar(), "auto"sv, "auto"sv)));
}

// 5.3.35 Temporal.PlainDateTime.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_json)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ? TemporalDateTimeToString(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]], dateTime.[[Calendar]], "auto", "auto").
    return PrimitiveString::create(vm, TRY(temporal_date_time_to_string(vm, date_time->iso_year(), date_time->iso_month(), date_time->iso_day(), date_time->iso_hour(), date_time->iso_minute(), date_time->iso_second(), date_time->iso_millisecond(), date_time->iso_microsecond(), date_time->iso_nanosecond(), &date_time->calendar(), "auto"sv, "auto"sv)));
}

// 5.3.36 Temporal.PlainDateTime.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.PlainDateTime", "a primitive value");
}

// 5.3.37 Temporal.PlainDateTime.prototype.toZonedDateTime ( temporalTimeZoneLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tozoneddatetime
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_zoned_date_time)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let timeZone be ? ToTemporalTimeZone(temporalTimeZoneLike).
    auto* time_zone = TRY(to_temporal_time_zone(vm, vm.argument(0)));

    // 4. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(1)));

    // 5. Let disambiguation be ? ToTemporalDisambiguation(options).
    auto disambiguation = TRY(to_temporal_disambiguation(vm, options));

    // 6. Let instant be ? BuiltinTimeZoneGetInstantFor(timeZone, dateTime, disambiguation).
    auto instant = TRY(builtin_time_zone_get_instant_for(vm, time_zone, date_time, disambiguation));

    // 7. Return ! CreateTemporalZonedDateTime(instant.[[Nanoseconds]], timeZone, dateTime.[[Calendar]]).
    return MUST(create_temporal_zoned_date_time(vm, instant->nanoseconds(), *time_zone, date_time->calendar()));
}

// 5.3.38 Temporal.PlainDateTime.prototype.toPlainDate ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.toplaindate
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_plain_date)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ! CreateTemporalDate(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[Calendar]]).
    return MUST(create_temporal_date(vm, date_time->iso_year(), date_time->iso_month(), date_time->iso_day(), date_time->calendar()));
}

// 5.3.39 Temporal.PlainDateTime.prototype.toPlainYearMonth ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.toplainyearmonth
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_plain_year_month)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Let fieldNames be ? CalendarFields(calendar, « "monthCode", "year" »).
    auto field_names = TRY(calendar_fields(vm, calendar, { "monthCode"sv, "year"sv }));

    // 5. Let fields be ? PrepareTemporalFields(dateTime, fieldNames, «»).
    auto* fields = TRY(prepare_temporal_fields(vm, date_time, field_names, Vector<StringView> {}));

    // 6. Return ? CalendarYearMonthFromFields(calendar, fields).
    return TRY(calendar_year_month_from_fields(vm, calendar, *fields));
}

// 5.3.40 Temporal.PlainDateTime.prototype.toPlainMonthDay ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.toplainmonthday
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_plain_month_day)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let calendar be dateTime.[[Calendar]].
    auto& calendar = date_time->calendar();

    // 4. Let fieldNames be ? CalendarFields(calendar, « "day", "monthCode" »).
    auto field_names = TRY(calendar_fields(vm, calendar, { "day"sv, "monthCode"sv }));

    // 5. Let fields be ? PrepareTemporalFields(dateTime, fieldNames, «»).
    auto* fields = TRY(prepare_temporal_fields(vm, date_time, field_names, Vector<StringView> {}));

    // 6. Return ? CalendarMonthDayFromFields(calendar, fields).
    return TRY(calendar_month_day_from_fields(vm, calendar, *fields));
}

// 5.3.41 Temporal.PlainDateTime.prototype.toPlainTime ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.toplaintime
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::to_plain_time)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Return ! CreateTemporalTime(dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]]).
    return MUST(create_temporal_time(vm, date_time->iso_hour(), date_time->iso_minute(), date_time->iso_second(), date_time->iso_millisecond(), date_time->iso_microsecond(), date_time->iso_nanosecond()));
}

// 5.3.42 Temporal.PlainDateTime.prototype.getISOFields ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.getisofields
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::get_iso_fields)
{
    auto& realm = *vm.current_realm();

    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto date_time = TRY(typed_this_object(vm));

    // 3. Let fields be OrdinaryObjectCreate(%Object.prototype%).
    auto fields = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. Perform ! CreateDataPropertyOrThrow(fields, "calendar", dateTime.[[Calendar]]).
    MUST(fields->create_data_property_or_throw(vm.names.calendar, Value(&date_time->calendar())));

    // 5. Perform ! CreateDataPropertyOrThrow(fields, "isoDay", 𝔽(dateTime.[[ISODay]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoDay, Value(date_time->iso_day())));

    // 6. Perform ! CreateDataPropertyOrThrow(fields, "isoHour", 𝔽(dateTime.[[ISOHour]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoHour, Value(date_time->iso_hour())));

    // 7. Perform ! CreateDataPropertyOrThrow(fields, "isoMicrosecond", 𝔽(dateTime.[[ISOMicrosecond]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoMicrosecond, Value(date_time->iso_microsecond())));

    // 8. Perform ! CreateDataPropertyOrThrow(fields, "isoMillisecond", 𝔽(dateTime.[[ISOMillisecond]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoMillisecond, Value(date_time->iso_millisecond())));

    // 9. Perform ! CreateDataPropertyOrThrow(fields, "isoMinute", 𝔽(dateTime.[[ISOMinute]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoMinute, Value(date_time->iso_minute())));

    // 10. Perform ! CreateDataPropertyOrThrow(fields, "isoMonth", 𝔽(dateTime.[[ISOMonth]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoMonth, Value(date_time->iso_month())));

    // 11. Perform ! CreateDataPropertyOrThrow(fields, "isoNanosecond", 𝔽(dateTime.[[ISONanosecond]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoNanosecond, Value(date_time->iso_nanosecond())));

    // 12. Perform ! CreateDataPropertyOrThrow(fields, "isoSecond", 𝔽(dateTime.[[ISOSecond]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoSecond, Value(date_time->iso_second())));

    // 13. Perform ! CreateDataPropertyOrThrow(fields, "isoYear", 𝔽(dateTime.[[ISOYear]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoYear, Value(date_time->iso_year())));

    // 14. Return fields.
    return fields;
}

// 5.3.3 get Temporal.PlainDateTime.prototype.calendarId, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.calendarid
JS_DEFINE_NATIVE_FUNCTION(PlainDateTimePrototype::calendar_id_getter)
{
    // 1. Let dateTime be the this value.
    // 2. Perform ? RequireInternalSlot(dateTime, [[InitializedTemporalDateTime]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return dateTime.[[Calendar]].
    auto& calendar = static_cast<Calendar&>(temporal_date->calendar());
    return PrimitiveString::create(vm, calendar.identifier());
}

}
