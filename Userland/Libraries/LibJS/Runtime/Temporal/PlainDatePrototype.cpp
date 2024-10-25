/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDatePrototype.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(PlainDatePrototype);

// 3.3 Properties of the Temporal.PlainDate Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plaindate-prototype-object
PlainDatePrototype::PlainDatePrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void PlainDatePrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 3.3.2 Temporal.PlainDate.prototype[ @@toStringTag ], https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.PlainDate"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.calendar, calendar_getter, {}, Attribute::Configurable);
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
    define_native_accessor(realm, vm.names.era, era_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.eraYear, era_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.calendarId, calendar_id_getter, {}, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.toPlainYearMonth, to_plain_year_month, 0, attr);
    define_native_function(realm, vm.names.toPlainMonthDay, to_plain_month_day, 0, attr);
    define_native_function(realm, vm.names.getISOFields, get_iso_fields, 0, attr);
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

// 3.3.3 get Temporal.PlainDate.prototype.calendar, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.calendar
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::calendar_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return temporalDate.[[Calendar]].
    return Value(&temporal_date->calendar());
}

// 3.3.4 get Temporal.PlainDate.prototype.year, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.year
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::year_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Return 𝔽(? CalendarYear(calendar, temporalDate)).
    return TRY(calendar_year(vm, calendar, temporal_date));
}

// 3.3.5 get Temporal.PlainDate.prototype.month, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.month
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::month_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Return 𝔽(? CalendarMonth(calendar, temporalDate)).
    return TRY(calendar_month(vm, calendar, temporal_date));
}

// 3.3.6 get Temporal.PlainDate.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.monthCode
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::month_code_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Return ? CalendarMonthCode(calendar, temporalDate).
    return PrimitiveString::create(vm, TRY(calendar_month_code(vm, calendar, temporal_date)));
}

// 3.3.7 get Temporal.PlainDate.prototype.day, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.day
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::day_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Return 𝔽(? CalendarDay(calendar, temporalDate)).
    return TRY(calendar_day(vm, calendar, temporal_date));
}

// 3.3.8 get Temporal.PlainDate.prototype.dayOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.dayofweek
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::day_of_week_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // Return 𝔽(? CalendarDayOfWeek(calendar, temporalDate)).
    return TRY(calendar_day_of_week(vm, calendar, temporal_date));
}

// 3.3.9 get Temporal.PlainDate.prototype.dayOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.dayofyear
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::day_of_year_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Return 𝔽(? CalendarDayOfYear(calendar, temporalDate)).
    return TRY(calendar_day_of_year(vm, calendar, temporal_date));
}

// 3.3.10 get Temporal.PlainDate.prototype.weekOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.weekofyear
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::week_of_year_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Return 𝔽(? CalendarWeekOfYear(calendar, temporalDate)).
    return TRY(calendar_week_of_year(vm, calendar, temporal_date));
}

// 3.3.11 get Temporal.PlainDate.prototype.yearOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.yearofweek
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::year_of_week_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Return 𝔽(? CalendarYearOfWeek(calendar, temporalDate)).
    return TRY(calendar_year_of_week(vm, calendar, temporal_date));
}

// 3.3.12 get Temporal.PlainDate.prototype.daysInWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.daysinweek
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::days_in_week_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Return 𝔽(? CalendarDaysInWeek(calendar, temporalDate)).
    return TRY(calendar_days_in_week(vm, calendar, temporal_date));
}

// 3.3.13 get Temporal.PlainDate.prototype.daysInMonth, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.daysinmonth
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::days_in_month_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Return 𝔽(? CalendarDaysInMonth(calendar, temporalDate)).
    return TRY(calendar_days_in_month(vm, calendar, temporal_date));
}

// 3.3.14 get Temporal.PlainDate.prototype.daysInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.daysinyear
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::days_in_year_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Return 𝔽(? CalendarDaysInYear(calendar, temporalDate)).
    return TRY(calendar_days_in_year(vm, calendar, temporal_date));
}

// 3.3.15 get Temporal.PlainDate.prototype.monthsInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.monthsinyear
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::months_in_year_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Return 𝔽(? CalendarMonthsInYear(calendar, temporalDate)).
    return TRY(calendar_months_in_year(vm, calendar, temporal_date));
}

// 3.3.16 get Temporal.PlainDate.prototype.inLeapYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.inleapyear
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::in_leap_year_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Return ? CalendarInLeapYear(calendar, temporalDate).
    return Value(TRY(calendar_in_leap_year(vm, calendar, temporal_date)));
}

// 15.6.5.2 get Temporal.PlainDate.prototype.era, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.era
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::era_getter)
{
    // 1. Let plainDate be the this value.
    // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
    auto plain_date = TRY(typed_this_object(vm));

    // 3. Let calendar be plainDate.[[Calendar]].
    auto& calendar = plain_date->calendar();

    // 4. Return ? CalendarEra(calendar, plainDate).
    return TRY(calendar_era(vm, calendar, plain_date));
}

// 15.6.5.3 get Temporal.PlainDate.prototype.eraYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.erayear
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::era_year_getter)
{
    // 1. Let plainDate be the this value.
    // 2. Perform ? RequireInternalSlot(plainDate, [[InitializedTemporalDate]]).
    auto plain_date = TRY(typed_this_object(vm));

    // 3. Let calendar be plainDate.[[Calendar]].
    auto& calendar = plain_date->calendar();

    // 4. Return ? CalendarEraYear(calendar, plainDate).
    return TRY(calendar_era_year(vm, calendar, plain_date));
}

// 3.3.17 Temporal.PlainDate.prototype.toPlainYearMonth ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.toplainyearmonth
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_plain_year_month)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Let fieldNames be ? CalendarFields(calendar, « "monthCode", "year" »).
    auto field_names = TRY(calendar_fields(vm, calendar, { "monthCode"sv, "year"sv }));

    // 5. Let fields be ? PrepareTemporalFields(temporalDate, fieldNames, «»).
    auto* fields = TRY(prepare_temporal_fields(vm, temporal_date, field_names, Vector<StringView> {}));

    // 6. Return ? CalendarYearMonthFromFields(calendar, fields).
    return TRY(calendar_year_month_from_fields(vm, calendar, *fields));
}

// 3.3.18 Temporal.PlainDate.prototype.toPlainMonthDay ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.toplainmonthday
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_plain_month_day)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 4. Let fieldNames be ? CalendarFields(calendar, « "day", "monthCode" »).
    auto field_names = TRY(calendar_fields(vm, calendar, { "day"sv, "monthCode"sv }));

    // 5. Let fields be ? PrepareTemporalFields(temporalDate, fieldNames, «»).
    auto* fields = TRY(prepare_temporal_fields(vm, temporal_date, field_names, Vector<StringView> {}));

    // 6. Return ? CalendarMonthDayFromFields(calendar, fields).
    return TRY(calendar_month_day_from_fields(vm, calendar, *fields));
}

// 3.3.19 Temporal.PlainDate.prototype.getISOFields ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.getisofields
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::get_iso_fields)
{
    auto& realm = *vm.current_realm();

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let fields be OrdinaryObjectCreate(%Object.prototype%).
    auto fields = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. Perform ! CreateDataPropertyOrThrow(fields, "calendar", temporalDate.[[Calendar]]).
    MUST(fields->create_data_property_or_throw(vm.names.calendar, Value(&temporal_date->calendar())));

    // 5. Perform ! CreateDataPropertyOrThrow(fields, "isoDay", 𝔽(temporalDate.[[ISODay]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoDay, Value(temporal_date->iso_day())));

    // 6. Perform ! CreateDataPropertyOrThrow(fields, "isoMonth", 𝔽(temporalDate.[[ISOMonth]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoMonth, Value(temporal_date->iso_month())));

    // 7. Perform ! CreateDataPropertyOrThrow(fields, "isoYear", 𝔽(temporalDate.[[ISOYear]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoYear, Value(temporal_date->iso_year())));

    // 8. Return fields.
    return fields;
}

// 3.3.20 Temporal.PlainDate.prototype.add ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.add
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::add)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let duration be ? ToTemporalDuration(temporalDurationLike).
    auto duration = TRY(to_temporal_duration(vm, vm.argument(0)));

    // 4. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(1)));

    // 5. Return ? CalendarDateAdd(temporalDate.[[Calendar]], temporalDate, duration, options).
    return TRY(calendar_date_add(vm, temporal_date->calendar(), temporal_date, *duration, options));
}

// 3.3.21 Temporal.PlainDate.prototype.subtract ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.subtract
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::subtract)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let duration be ? ToTemporalDuration(temporalDurationLike).
    auto duration = TRY(to_temporal_duration(vm, vm.argument(0)));

    // 4. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(1)));

    // 5. Let negatedDuration be ! CreateNegatedTemporalDuration(duration).
    auto negated_duration = create_negated_temporal_duration(vm, *duration);

    // 6. Return ? CalendarDateAdd(temporalDate.[[Calendar]], temporalDate, negatedDuration, options).
    return TRY(calendar_date_add(vm, temporal_date->calendar(), temporal_date, *negated_duration, options));
}

// 3.3.22 Temporal.PlainDate.prototype.with ( temporalDateLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.with
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::with)
{
    auto temporal_date_like = vm.argument(0);

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. If Type(temporalDateLike) is not Object, then
    if (!temporal_date_like.is_object()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, temporal_date_like.to_string_without_side_effects());
    }

    // 4. Perform ? RejectObjectWithCalendarOrTimeZone(temporalDateLike).
    TRY(reject_object_with_calendar_or_time_zone(vm, temporal_date_like.as_object()));

    // 5. Let calendar be temporalDate.[[Calendar]].
    auto& calendar = temporal_date->calendar();

    // 6. Let fieldNames be ? CalendarFields(calendar, « "day", "month", "monthCode", "year" »).
    auto field_names = TRY(calendar_fields(vm, calendar, { "day"sv, "month"sv, "monthCode"sv, "year"sv }));

    // 7. Let partialDate be ? PrepareTemporalFields(temporalDateLike, fieldNames, partial).
    auto* partial_date = TRY(prepare_temporal_fields(vm, temporal_date_like.as_object(), field_names, PrepareTemporalFieldsPartial {}));

    // 8. Set options to ? GetOptionsObject(options).
    auto const* options = TRY(get_options_object(vm, vm.argument(1)));

    // 9. Let fields be ? PrepareTemporalFields(temporalDate, fieldNames, «»).
    auto* fields = TRY(prepare_temporal_fields(vm, temporal_date, field_names, Vector<StringView> {}));

    // 10. Set fields to ? CalendarMergeFields(calendar, fields, partialDate).
    fields = TRY(calendar_merge_fields(vm, calendar, *fields, *partial_date));

    // 11. Set fields to ? PrepareTemporalFields(fields, fieldNames, «»).
    fields = TRY(prepare_temporal_fields(vm, *fields, field_names, Vector<StringView> {}));

    // 12. Return ? CalendarDateFromFields(calendar, fields, options).
    return TRY(calendar_date_from_fields(vm, calendar, *fields, options));
}

// 3.3.23 Temporal.PlainDate.prototype.withCalendar ( calendarLike ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.withcalendar
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::with_calendar)
{
    auto calendar_like = vm.argument(0);

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Let calendar be ? ToTemporalCalendar(calendarLike).
    auto* calendar = TRY(to_temporal_calendar(vm, calendar_like));

    // 4. Return ! CreateTemporalDate(temporalDate.[[ISOYear]], temporalDate.[[ISOMonth]], temporalDate.[[ISODay]], calendar).
    return MUST(create_temporal_date(vm, temporal_date->iso_year(), temporal_date->iso_month(), temporal_date->iso_day(), *calendar));
}

// 3.3.24 Temporal.PlainDate.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.until
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::until)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalPlainDate(until, temporalDate, other, options).
    return TRY(difference_temporal_plain_date(vm, DifferenceOperation::Until, temporal_date, other, options));
}

// 3.3.25 Temporal.PlainDate.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.since
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::since)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalPlainDate(since, temporalDate, other, options).
    return TRY(difference_temporal_plain_date(vm, DifferenceOperation::Since, temporal_date, other, options));
}

// 3.3.26 Temporal.PlainDate.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.equals
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::equals)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Set other to ? ToTemporalDate(other).
    auto* other = TRY(to_temporal_date(vm, vm.argument(0)));

    // 4. If temporalDate.[[ISOYear]] ≠ other.[[ISOYear]], return false.
    if (temporal_date->iso_year() != other->iso_year())
        return Value(false);
    // 5. If temporalDate.[[ISOMonth]] ≠ other.[[ISOMonth]], return false.
    if (temporal_date->iso_month() != other->iso_month())
        return Value(false);
    // 6. If temporalDate.[[ISODay]] ≠ other.[[ISODay]], return false.
    if (temporal_date->iso_day() != other->iso_day())
        return Value(false);
    // 7. Return ? CalendarEquals(temporalDate.[[Calendar]], other.[[Calendar]]).
    return Value(TRY(calendar_equals(vm, temporal_date->calendar(), other->calendar())));
}

// 3.3.27 Temporal.PlainDate.prototype.toPlainDateTime ( [ temporalTime ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.toplaindatetime
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_plain_date_time)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. If temporalTime is undefined, then
    if (vm.argument(0).is_undefined()) {
        // a. Return ? CreateTemporalDateTime(temporalDate.[[ISOYear]], temporalDate.[[ISOMonth]], temporalDate.[[ISODay]], 0, 0, 0, 0, 0, 0, temporalDate.[[Calendar]]).
        return TRY(create_temporal_date_time(vm, temporal_date->iso_year(), temporal_date->iso_month(), temporal_date->iso_day(), 0, 0, 0, 0, 0, 0, temporal_date->calendar()));
    }

    // 4. Set temporalTime to ? ToTemporalTime(temporalTime).
    auto* temporal_time = TRY(to_temporal_time(vm, vm.argument(0)));

    // 5. Return ? CreateTemporalDateTime(temporalDate.[[ISOYear]], temporalDate.[[ISOMonth]], temporalDate.[[ISODay]], temporalTime.[[ISOHour]], temporalTime.[[ISOMinute]], temporalTime.[[ISOSecond]], temporalTime.[[ISOMillisecond]], temporalTime.[[ISOMicrosecond]], temporalTime.[[ISONanosecond]], temporalDate.[[Calendar]]).
    return TRY(create_temporal_date_time(vm, temporal_date->iso_year(), temporal_date->iso_month(), temporal_date->iso_day(), temporal_time->iso_hour(), temporal_time->iso_minute(), temporal_time->iso_second(), temporal_time->iso_millisecond(), temporal_time->iso_microsecond(), temporal_time->iso_nanosecond(), temporal_date->calendar()));
}

// 3.3.28 Temporal.PlainDate.prototype.toZonedDateTime ( item ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tozoneddatetime
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_zoned_date_time)
{
    auto item = vm.argument(0);

    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    auto temporal_time_value = js_undefined();
    Object* time_zone;

    // 3. If Type(item) is Object, then
    if (item.is_object()) {
        // a. If item has an [[InitializedTemporalTimeZone]] internal slot, then
        if (is<TimeZone>(item.as_object())) {
            // i. Let timeZone be item.
            time_zone = &item.as_object();

            // ii. Let temporalTime be undefined.
        }
        // b. Else,
        else {
            // i. Let timeZoneLike be ? Get(item, "timeZone").
            auto time_zone_like = TRY(item.as_object().get(vm.names.timeZone));

            // ii. If timeZoneLike is undefined, then
            if (time_zone_like.is_undefined()) {
                // 1. Let timeZone be ? ToTemporalTimeZone(item).
                time_zone = TRY(to_temporal_time_zone(vm, item));

                // 2. Let temporalTime be undefined.
            }
            // iii. Else,
            else {
                // 1. Let timeZone be ? ToTemporalTimeZone(timeZoneLike).
                time_zone = TRY(to_temporal_time_zone(vm, time_zone_like));

                // 2. Let temporalTime be ? Get(item, "plainTime").
                temporal_time_value = TRY(item.as_object().get(vm.names.plainTime));
            }
        }
    }
    // 4. Else,
    else {
        // a. Let timeZone be ? ToTemporalTimeZone(item).
        time_zone = TRY(to_temporal_time_zone(vm, item));

        // b. Let temporalTime be undefined.
    }

    PlainDateTime* temporal_date_time;

    // 5. If temporalTime is undefined, then
    if (temporal_time_value.is_undefined()) {
        // a. Let temporalDateTime be ? CreateTemporalDateTime(temporalDate.[[ISOYear]], temporalDate.[[ISOMonth]], temporalDate.[[ISODay]], 0, 0, 0, 0, 0, 0, temporalDate.[[Calendar]]).
        temporal_date_time = TRY(create_temporal_date_time(vm, temporal_date->iso_year(), temporal_date->iso_month(), temporal_date->iso_day(), 0, 0, 0, 0, 0, 0, temporal_date->calendar()));
    }
    // 6. Else,
    else {
        // a. Set temporalTime to ? ToTemporalTime(temporalTime).
        auto* temporal_time = TRY(to_temporal_time(vm, temporal_time_value));

        // b. Let temporalDateTime be ? CreateTemporalDateTime(temporalDate.[[ISOYear]], temporalDate.[[ISOMonth]], temporalDate.[[ISODay]], temporalTime.[[ISOHour]], temporalTime.[[ISOMinute]], temporalTime.[[ISOSecond]], temporalTime.[[ISOMillisecond]], temporalTime.[[ISOMicrosecond]], temporalTime.[[ISONanosecond]], temporalDate.[[Calendar]]).
        temporal_date_time = TRY(create_temporal_date_time(vm, temporal_date->iso_year(), temporal_date->iso_month(), temporal_date->iso_day(), temporal_time->iso_hour(), temporal_time->iso_minute(), temporal_time->iso_second(), temporal_time->iso_millisecond(), temporal_time->iso_microsecond(), temporal_time->iso_nanosecond(), temporal_time->calendar()));
    }

    // 7. Let instant be ? BuiltinTimeZoneGetInstantFor(timeZone, temporalDateTime, "compatible").
    auto instant = TRY(builtin_time_zone_get_instant_for(vm, time_zone, *temporal_date_time, "compatible"sv));

    // 8. Return ! CreateTemporalZonedDateTime(instant.[[Nanoseconds]], timeZone, temporalDate.[[Calendar]]).
    return MUST(create_temporal_zoned_date_time(vm, instant->nanoseconds(), *time_zone, temporal_date->calendar()));
}

// 3.3.29 Temporal.PlainDate.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_string)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(0)));

    // 4. Let showCalendar be ? ToCalendarNameOption(options).
    auto show_calendar = TRY(to_calendar_name_option(vm, *options));

    // 5. Return ? TemporalDateToString(temporalDate, showCalendar).
    return PrimitiveString::create(vm, TRY(temporal_date_to_string(vm, temporal_date, show_calendar)));
}

// 3.3.30 Temporal.PlainDate.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.tolocalestring
// NOTE: This is the minimum toLocaleString implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_locale_string)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return ? TemporalDateToString(temporalDate, "auto").
    return PrimitiveString::create(vm, TRY(temporal_date_to_string(vm, temporal_date, "auto"sv)));
}

// 3.3.31 Temporal.PlainDate.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::to_json)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return ? TemporalDateToString(temporalDate, "auto").
    return PrimitiveString::create(vm, TRY(temporal_date_to_string(vm, temporal_date, "auto"sv)));
}

// 3.3.32 Temporal.PlainDate.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.PlainDate", "a primitive value");
}

// 3.3.3 get Temporal.PlainDate.prototype.calendarId, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindate.prototype.calendarid
JS_DEFINE_NATIVE_FUNCTION(PlainDatePrototype::calendar_id_getter)
{
    // 1. Let temporalDate be the this value.
    // 2. Perform ? RequireInternalSlot(temporalDate, [[InitializedTemporalDate]]).
    auto temporal_date = TRY(typed_this_object(vm));

    // 3. Return temporalDate.[[Calendar]].
    auto& calendar = static_cast<Calendar&>(temporal_date->calendar());
    return PrimitiveString::create(vm, calendar.identifier());
}

}
