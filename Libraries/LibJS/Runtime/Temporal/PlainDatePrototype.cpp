/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/PlainDatePrototype.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainDatePrototype);

// 3.3 Properties of the Temporal.PlainDate Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plaindate-prototype-object
PlainDatePrototype::PlainDatePrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void PlainDatePrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 3.3.2 Temporal.PlainDate.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.PlainDate"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.calendarId, calendar_id_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.era, era_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.eraYear, era_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.year, year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.month, month_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.monthCode, month_code_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.day, day_getter, {}, Attribute::Configurable);
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
    define_native_function(realm, vm.names.toPlainYearMonth, to_plain_year_month, 0, attr);
    define_native_function(realm, vm.names.toPlainMonthDay, to_plain_month_day, 0, attr);
    define_native_function(realm, vm.names.add, add, 1, attr);
    define_native_function(realm, vm.names.subtract, subtract, 1, attr);
    define_native_function(realm, vm.names.with, with, 1, attr);
    define_native_function(realm, vm.names.withCalendar, with_calendar, 1, attr);
    define_native_function(realm, vm.names.until, until, 1, attr);
    define_native_function(realm, vm.names.since, since, 1, attr);
    define_native_function(realm, vm.names.equals, equals, 1, attr);
    define_native_function(realm, vm.names.toPlainDateTime, to_plain_date_time, 0, attr);
    define_native_function(realm, vm.names.toZonedDateTime, to_zoned_date_time, 1, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);
    define_native_function(realm, vm.names.valueOf, value_of, 0, attr);
}

// 3.3.3 get Temporal.PlainDate.prototype.calendarId, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.calendarid
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::calendar_id_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return temporalDate.[[Calendar]].
    return PrimitiveString::create(vm, temporal_date->calendar());
}

// 3.3.4 get Temporal.PlainDate.prototype.era, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.era
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::era_getter)
{
    // 1. Let plainDate be the this value.
    // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
    auto plain_date = TRY(typed_this_object(vm));

    // 3. Return CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[Era]].
    auto result = calendar_iso_to_date(plain_date->calendar(), plain_date->iso_date()).era;

    if (!result.has_value())
        return js_undefined();

    return PrimitiveString::create(vm, result.release_value());
}

// 3.3.5 get Temporal.PlainDate.prototype.eraYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.erayear
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::era_year_getter)
{
    // 1. Let plainDate be the this value.
    // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
    auto plain_date = TRY(typed_this_object(vm));

    // 3. Let result be CalendarISOToDate(plainDate.[[Calendar]], plainDate.[[ISODate]]).[[EraYear]].
    auto result = calendar_iso_to_date(plain_date->calendar(), plain_date->iso_date()).era_year;

    // 4. If result is undefined, return undefined.
    if (!result.has_value())
        return js_undefined();

    // 5. Return 𝔽(result).
    return *result;
}

// 3.3.6 get Temporal.PlainDate.prototype.year, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.year
// 3.3.7 get Temporal.PlainDate.prototype.month, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.month
// 3.3.9 get Temporal.PlainDate.prototype.day, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.day
// 3.3.10 get Temporal.PlainDate.prototype.dayOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.dayofweek
// 3.3.11 get Temporal.PlainDate.prototype.dayOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.dayofyear
// 3.3.14 get Temporal.PlainDate.prototype.daysInWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.daysinweek
// 3.3.15 get Temporal.PlainDate.prototype.daysInMonth, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.daysinmonth
// 3.3.16 get Temporal.PlainDate.prototype.daysInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.daysinyear
// 3.3.17 get Temporal.PlainDate.prototype.monthsInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.monthsinyear
// 3.3.18 get Temporal.PlainDate.prototype.inLeapYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.inleapyear
#define JS_ENUMERATE_PLAIN_DATE_SIMPLE_FIELDS \
    __JS_ENUMERATE(year)                      \
    __JS_ENUMERATE(month)                     \
    __JS_ENUMERATE(day)                       \
    __JS_ENUMERATE(day_of_week)               \
    __JS_ENUMERATE(day_of_year)               \
    __JS_ENUMERATE(days_in_week)              \
    __JS_ENUMERATE(days_in_month)             \
    __JS_ENUMERATE(days_in_year)              \
    __JS_ENUMERATE(months_in_year)            \
    __JS_ENUMERATE(in_leap_year)

#define __JS_ENUMERATE(field)                                                                               \
    JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::field##_getter)                                           \
    {                                                                                                       \
        /* 1. Let temporalDate be the this value. */                                                        \
        /* 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]). */                  \
        auto temporal_date = TRY(typed_this_object(vm));                                                    \
                                                                                                            \
        /* 3. Return CalendarISOToDate(temporalDate.[[Calendar]], temporalDate.[[ISODate]]).[[<field>]]. */ \
        return calendar_iso_to_date(temporal_date->calendar(), temporal_date->iso_date()).field;            \
    }
JS_ENUMERATE_PLAIN_DATE_SIMPLE_FIELDS
#undef __JS_ENUMERATE

// 3.3.8 get Temporal.PlainDate.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.monthcode
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::month_code_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return CalendarISOToDate(temporalDate.[[Calendar]], temporalDate.[[ISODate]]).[[MonthCode]].
    auto month_code = calendar_iso_to_date(temporal_date->calendar(), temporal_date->iso_date()).month_code;
    return PrimitiveString::create(vm, move(month_code));
}

// 3.3.12 get Temporal.PlainDate.prototype.weekOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.weekofyear
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::week_of_year_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let result be CalendarISOToDate(temporalDate.[[Calendar]], temporalDate.[[ISODate]]).[[WeekOfYear]].[[Week]].
    auto result = calendar_iso_to_date(temporal_date->calendar(), temporal_date->iso_date()).week_of_year.week;

    // 4. If result is undefined, return undefined.
    if (!result.has_value())
        return js_undefined();

    // 5. Return 𝔽(result).
    return *result;
}

// 3.3.13 get Temporal.PlainDate.prototype.yearOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.yearofweek
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::year_of_week_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let result be CalendarISOToDate(temporalDate.[[Calendar]], temporalDate.[[ISODate]]).[[WeekOfYear]].[[Year]].
    auto result = calendar_iso_to_date(temporal_date->calendar(), temporal_date->iso_date()).week_of_year.year;

    // 4. If result is undefined, return undefined.
    if (!result.has_value())
        return js_undefined();

    // 5. Return 𝔽(result).
    return *result;
}

// 3.3.19 Temporal.PlainDate.prototype.toPlainYearMonth ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.toplainyearmonth
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_plain_year_month)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto const& calendar = temporal_date->calendar();

    // 4. Let fields be ISODateToFields(calendar, temporalDate.[[ISODate]], DATE).
    auto fields = iso_date_to_fields(calendar, temporal_date->iso_date(), DateType::Date);

    // 5. Let isoDate be ? CalendarYearMonthFromFields(calendar, fields, CONSTRAIN).
    auto iso_date = TRY(calendar_year_month_from_fields(vm, calendar, fields, Overflow::Constrain));

    // 6. Return ! CreateTemporalYearMonth(isoDate, calendar).
    return MUST(create_temporal_year_month(vm, iso_date, calendar));
}

// 3.3.20 Temporal.PlainDate.prototype.toPlainMonthDay ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.toplainmonthday
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_plain_month_day)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto const& calendar = temporal_date->calendar();

    // 4. Let fields be ISODateToFields(calendar, temporalDate.[[ISODate]], DATE).
    auto fields = iso_date_to_fields(calendar, temporal_date->iso_date(), DateType::Date);

    // 5. Let isoDate be ? CalendarMonthDayFromFields(calendar, fields, CONSTRAIN).
    auto iso_date = TRY(calendar_month_day_from_fields(vm, calendar, fields, Overflow::Constrain));

    // 6. Return ! CreateTemporalMonthDay(isoDate, calendar).
    return MUST(create_temporal_month_day(vm, iso_date, calendar));
}

// 3.3.21 Temporal.PlainDate.prototype.add ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.add
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::add)
{
    auto temporal_duration_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToDate(ADD, temporalDate, temporalDurationLike, options).
    return TRY(add_duration_to_date(vm, ArithmeticOperation::Add, temporal_date, temporal_duration_like, options));
}

// 3.3.22 Temporal.PlainDate.prototype.subtract ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.subtract
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::subtract)
{
    auto temporal_duration_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let temporalDate be the this value.
    auto temporal_date = TRY(typed_this_object(vm));

    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    // 3. Return ? AddDurationToDate(SUBTRACT, temporalDate, temporalDurationLike, options).
    return TRY(add_duration_to_date(vm, ArithmeticOperation::Subtract, temporal_date, temporal_duration_like, options));
}

// 3.3.23 Temporal.PlainDate.prototype.with ( temporalDateLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.with
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::with)
{
    auto temporal_date_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. If ? IsPartialTemporalObject(temporalDateLike) is false, throw a TypeError exception.
    if (!TRY(is_partial_temporal_object(vm, temporal_date_like)))
        return vm.throw_completion<TypeError>(ErrorType::TemporalObjectMustBePartialTemporalObject);

    // 4. Let calendar be temporalDate.[[Calendar]].
    auto const& calendar = temporal_date->calendar();

    // 5. Let fields be ISODateToFields(calendar, temporalDate.[[ISODate]], DATE).
    auto fields = iso_date_to_fields(calendar, temporal_date->iso_date(), DateType::Date);

    // 6. Let partialDate be ? PrepareCalendarFields(calendar, temporalDateLike, « YEAR, MONTH, MONTH-CODE, DAY », « », PARTIAL).
    auto partial_date = TRY(prepare_calendar_fields(vm, calendar, temporal_date_like.as_object(), { { CalendarField::Year, CalendarField::Month, CalendarField::MonthCode, CalendarField::Day } }, {}, Partial {}));

    // 7. Set fields to CalendarMergeFields(calendar, fields, partialDate).
    fields = calendar_merge_fields(calendar, fields, partial_date);

    // 8. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 9. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
    auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

    // 10. Let isoDate be ? CalendarDateFromFields(calendar, fields, overflow).
    auto iso_date = TRY(calendar_date_from_fields(vm, calendar, fields, overflow));

    // 11. Return ! CreateTemporalDate(isoDate, calendar).
    return MUST(create_temporal_date(vm, iso_date, calendar));
}

// 3.3.24 Temporal.PlainDate.prototype.withCalendar ( calendarLike ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.with
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::with_calendar)
{
    auto calendar_like = vm.argument(0);

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be ? ToTemporalCalendarIdentifier(calendarLike).
    auto calendar = TRY(to_temporal_calendar_identifier(vm, calendar_like));

    // 4. Return ! CreateTemporalDate(temporalDate.[[ISODate]], calendar).
    return MUST(create_temporal_date(vm, temporal_date->iso_date(), calendar));
}

// 3.3.25 Temporal.PlainDate.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.until
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::until)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalPlainDate(UNTIL, temporalDate, other, options).
    return TRY(difference_temporal_plain_date(vm, DurationOperation::Until, temporal_date, other, options));
}

// 3.3.26 Temporal.PlainDate.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.since
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::since)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalPlainDate(SINCE, temporalDate, other, options).
    return TRY(difference_temporal_plain_date(vm, DurationOperation::Since, temporal_date, other, options));
}

// 3.3.27 Temporal.PlainDate.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.equals
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::equals)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Set other to ? ToTemporalDate(other).
    auto other = TRY(to_temporal_date(vm, vm.argument(0)));

    // 4. If CompareISODate(temporalDate.[[ISODate]], other.[[ISODate]]) ≠ 0, return false.
    if (compare_iso_date(temporal_date->iso_date(), other->iso_date()) != 0)
        return false;

    // 5. Return CalendarEquals(temporalDate.[[Calendar]], other.[[Calendar]]).
    return calendar_equals(temporal_date->calendar(), other->calendar());
}

// 3.3.28 Temporal.PlainDate.prototype.toPlainDateTime ( [ temporalTime ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.toplaindatetime
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_plain_date_time)
{
    auto temporal_time = vm.argument(0);

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let time be ? ToTimeRecordOrMidnight(temporalTime).
    auto time = TRY(to_time_record_or_midnight(vm, temporal_time));

    // 4. Let isoDateTime be CombineISODateAndTimeRecord(temporalDate.[[ISODate]], time).
    auto iso_date_time = combine_iso_date_and_time_record(temporal_date->iso_date(), time);

    // 5. Return ? CreateTemporalDateTime(isoDateTime, temporalDate.[[Calendar]]).
    return TRY(create_temporal_date_time(vm, iso_date_time, temporal_date->calendar()));
}

// 3.3.29 Temporal.PlainDate.prototype.toZonedDateTime ( item ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.tozoneddatetime
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_zoned_date_time)
{
    auto item = vm.argument(0);

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    String time_zone;
    Value temporal_time;

    // 3. If item is an Object, then
    if (item.is_object()) {
        // a. Let timeZoneLike be ? Get(item, "timeZone").
        auto time_zone_like = TRY(item.as_object().get(vm.names.timeZone));

        // b. If timeZoneLike is undefined, then
        if (time_zone_like.is_undefined()) {
            // i. Let timeZone be ? ToTemporalTimeZoneIdentifier(item).
            time_zone = TRY(to_temporal_time_zone_identifier(vm, item));

            // ii. Let temporalTime be undefined.
            temporal_time = js_undefined();
        }
        // c. Else,
        else {
            // i. Let timeZone be ? ToTemporalTimeZoneIdentifier(timeZoneLike).
            time_zone = TRY(to_temporal_time_zone_identifier(vm, time_zone_like));

            // ii. Let temporalTime be ? Get(item, "plainTime").
            temporal_time = TRY(item.as_object().get(vm.names.plainTime));
        }
    }
    // 4. Else,
    else {
        // a. Let timeZone be ? ToTemporalTimeZoneIdentifier(item).
        time_zone = TRY(to_temporal_time_zone_identifier(vm, item));

        // b. Let temporalTime be undefined.
        temporal_time = js_undefined();
    }

    Crypto::SignedBigInteger epoch_nanoseconds;

    // 5. If temporalTime is undefined, then
    if (temporal_time.is_undefined()) {
        // a. Let epochNs be ? GetStartOfDay(timeZone, temporalDate.[[ISODate]]).
        epoch_nanoseconds = TRY(get_start_of_day(vm, time_zone, temporal_date->iso_date()));
    }
    // 6. Else,
    else {
        // a. Set temporalTime to ? ToTemporalTime(temporalTime).
        auto plain_temporal_time = TRY(to_temporal_time(vm, temporal_time));

        // b. Let isoDateTime be CombineISODateAndTimeRecord(temporalDate.[[ISODate]], temporalTime.[[Time]]).
        auto iso_date_time = combine_iso_date_and_time_record(temporal_date->iso_date(), plain_temporal_time->time());

        // c. If ISODateTimeWithinLimits(isoDateTime) is false, throw a RangeError exception.
        if (!iso_date_time_within_limits(iso_date_time))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODateTime);

        // d. Let epochNs be ? GetEpochNanosecondsFor(timeZone, isoDateTime, COMPATIBLE).
        epoch_nanoseconds = TRY(get_epoch_nanoseconds_for(vm, time_zone, iso_date_time, Disambiguation::Compatible));
    }

    // 7. Return ! CreateTemporalZonedDateTime(epochNs, timeZone, temporalDate.[[Calendar]]).
    return MUST(create_temporal_zoned_date_time(vm, BigInt::create(vm, move(epoch_nanoseconds)), move(time_zone), temporal_date->calendar()));
}

// 3.3.30 Temporal.PlainDate.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_string)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, vm.argument(0)));

    // 4. Let showCalendar be ? GetTemporalShowCalendarNameOption(resolvedOptions).
    auto show_calendar = TRY(get_temporal_show_calendar_name_option(vm, resolved_options));

    // 5. Return TemporalDateToString(temporalDate, showCalendar).
    return PrimitiveString::create(vm, temporal_date_to_string(temporal_date, show_calendar));
}

// 3.3.31 Temporal.PlainDate.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.tolocalestring
// NOTE: This is the minimum toLocaleString implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_locale_string)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return TemporalDateToString(temporalDate, AUTO).
    return PrimitiveString::create(vm, temporal_date_to_string(temporal_date, ShowCalendar::Auto));
}

// 3.3.32 Temporal.PlainDate.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_json)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return TemporalDateToString(temporalDate, AUTO).
    return PrimitiveString::create(vm, temporal_date_to_string(temporal_date, ShowCalendar::Auto));
}

// 3.3.33 Temporal.PlainDate.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.PlainDate", "a primitive value");
}

}
