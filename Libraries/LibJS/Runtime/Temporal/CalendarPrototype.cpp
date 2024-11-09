/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/CalendarPrototype.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(CalendarPrototype);

[[nodiscard]] static i32 iso_year(Object& temporal_object);
[[nodiscard]] static u8 iso_month(Object& temporal_object);
[[nodiscard]] static u8 iso_day(Object& temporal_object);

// 12.4 Properties of the Temporal.Calendar Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-calendar-prototype-object
CalendarPrototype::CalendarPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void CalendarPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 12.4.2 Temporal.Calendar.prototype[ @@toStringTag ], https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.Calendar"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.id, id_getter, {}, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.dateFromFields, date_from_fields, 1, attr);
    define_native_function(realm, vm.names.yearMonthFromFields, year_month_from_fields, 1, attr);
    define_native_function(realm, vm.names.monthDayFromFields, month_day_from_fields, 1, attr);
    define_native_function(realm, vm.names.dateAdd, date_add, 2, attr);
    define_native_function(realm, vm.names.dateUntil, date_until, 2, attr);
    define_native_function(realm, vm.names.year, year, 1, attr);
    define_native_function(realm, vm.names.month, month, 1, attr);
    define_native_function(realm, vm.names.monthCode, month_code, 1, attr);
    define_native_function(realm, vm.names.day, day, 1, attr);
    define_native_function(realm, vm.names.dayOfWeek, day_of_week, 1, attr);
    define_native_function(realm, vm.names.dayOfYear, day_of_year, 1, attr);
    define_native_function(realm, vm.names.weekOfYear, week_of_year, 1, attr);
    define_native_function(realm, vm.names.yearOfWeek, year_of_week, 1, attr);
    define_native_function(realm, vm.names.daysInWeek, days_in_week, 1, attr);
    define_native_function(realm, vm.names.daysInMonth, days_in_month, 1, attr);
    define_native_function(realm, vm.names.daysInYear, days_in_year, 1, attr);
    define_native_function(realm, vm.names.monthsInYear, months_in_year, 1, attr);
    define_native_function(realm, vm.names.inLeapYear, in_leap_year, 1, attr);
    define_native_function(realm, vm.names.fields, fields, 1, attr);
    define_native_function(realm, vm.names.mergeFields, merge_fields, 2, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);
    define_native_function(realm, vm.names.era, era, 1, attr);
    define_native_function(realm, vm.names.eraYear, era_year, 1, attr);
}

// 12.4.3 get Temporal.Calendar.prototype.id, https://tc39.es/proposal-temporal/#sec-get-temporal.calendar.prototype.id
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::id_getter)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Return calendar.[[Identifier]].
    return { PrimitiveString::create(vm, calendar->identifier()) };
}

// 12.4.4 Temporal.Calendar.prototype.dateFromFields ( fields [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.datefromfields
// NOTE: This is the minimum dateFromFields implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::date_from_fields)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    // 4. If Type(fields) is not Object, throw a TypeError exception.
    auto fields = vm.argument(0);
    if (!fields.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, fields.to_string_without_side_effects());

    // 5. Set options to ? GetOptionsObject(options).
    auto const* options = TRY(get_options_object(vm, vm.argument(1)));

    // 6. Let result be ? ISODateFromFields(fields, options).
    auto result = TRY(iso_date_from_fields(vm, fields.as_object(), *options));

    // 7. Return ? CreateTemporalDate(result.[[Year]], result.[[Month]], result.[[Day]], calendar).
    return TRY(create_temporal_date(vm, result.year, result.month, result.day, calendar));
}

// 12.4.5 Temporal.Calendar.prototype.yearMonthFromFields ( fields [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.yearmonthfromfields
// NOTE: This is the minimum yearMonthFromFields implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::year_month_from_fields)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    // 4. If Type(fields) is not Object, throw a TypeError exception.
    auto fields = vm.argument(0);
    if (!fields.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, fields.to_string_without_side_effects());

    // 5. Set options to ? GetOptionsObject(options).
    auto const* options = TRY(get_options_object(vm, vm.argument(1)));

    // 6. Let result be ? ISOYearMonthFromFields(fields, options).
    auto result = TRY(iso_year_month_from_fields(vm, fields.as_object(), *options));

    // 7. Return ? CreateTemporalYearMonth(result.[[Year]], result.[[Month]], calendar, result.[[ReferenceISODay]]).
    return TRY(create_temporal_year_month(vm, result.year, result.month, calendar, result.reference_iso_day));
}

// 12.4.6 Temporal.Calendar.prototype.monthDayFromFields ( fields [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.monthdayfromfields
// NOTE: This is the minimum monthDayFromFields implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::month_day_from_fields)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    // 4. If Type(fields) is not Object, throw a TypeError exception.
    auto fields = vm.argument(0);
    if (!fields.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, fields.to_string_without_side_effects());

    // 5. Set options to ? GetOptionsObject(options).
    auto const* options = TRY(get_options_object(vm, vm.argument(1)));

    // 6. Let result be ? ISOMonthDayFromFields(fields, options).
    auto result = TRY(iso_month_day_from_fields(vm, fields.as_object(), *options));

    // 7. Return ? CreateTemporalMonthDay(result.[[Month]], result.[[Day]], calendar, result.[[ReferenceISOYear]]).
    return TRY(create_temporal_month_day(vm, result.month, result.day, calendar, result.reference_iso_year));
}

// 12.4.7 Temporal.Calendar.prototype.dateAdd ( date, duration [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.dateadd
// NOTE: This is the minimum dateAdd implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::date_add)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    // 4. Set date to ? ToTemporalDate(date).
    auto* date = TRY(to_temporal_date(vm, vm.argument(0)));

    // 5. Set duration to ? ToTemporalDuration(duration).
    auto duration = TRY(to_temporal_duration(vm, vm.argument(1)));

    // 6. Set options to ? GetOptionsObject(options).
    auto const* options = TRY(get_options_object(vm, vm.argument(2)));

    // 7. Let overflow be ? ToTemporalOverflow(options).
    auto overflow = TRY(to_temporal_overflow(vm, options));

    // 8. Let balanceResult be ? BalanceDuration(duration.[[Days]], duration.[[Hours]], duration.[[Minutes]], duration.[[Seconds]], duration.[[Milliseconds]], duration.[[Microseconds]], duration.[[Nanoseconds]], "day").
    auto balance_result = TRY(balance_duration(vm, duration->days(), duration->hours(), duration->minutes(), duration->seconds(), duration->milliseconds(), duration->microseconds(), Crypto::SignedBigInteger { duration->nanoseconds() }, "day"sv));

    // 9. Let result be ? AddISODate(date.[[ISOYear]], date.[[ISOMonth]], date.[[ISODay]], duration.[[Years]], duration.[[Months]], duration.[[Weeks]], balanceResult.[[Days]], overflow).
    auto result = TRY(add_iso_date(vm, date->iso_year(), date->iso_month(), date->iso_day(), duration->years(), duration->months(), duration->weeks(), balance_result.days, overflow));

    // 10. Return ? CreateTemporalDate(result.[[Year]], result.[[Month]], result.[[Day]], calendar).
    return TRY(create_temporal_date(vm, result.year, result.month, result.day, calendar));
}

// 12.4.8 Temporal.Calendar.prototype.dateUntil ( one, two [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.dateuntil
// NOTE: This is the minimum dateUntil implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::date_until)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    // 4. Set one to ? ToTemporalDate(one).
    auto* one = TRY(to_temporal_date(vm, vm.argument(0)));

    // 5. Set two to ? ToTemporalDate(two).
    auto* two = TRY(to_temporal_date(vm, vm.argument(1)));

    // 6. Set options to ? GetOptionsObject(options).
    auto const* options = TRY(get_options_object(vm, vm.argument(2)));

    // 7. Let largestUnit be ? GetTemporalUnit(options, "largestUnit", date, "auto").
    auto largest_unit = TRY(get_temporal_unit(vm, *options, vm.names.largestUnit, UnitGroup::Date, { "auto"sv }));

    // 8. If largestUnit is "auto", set largestUnit to "day".
    if (largest_unit == "auto")
        largest_unit = "day"_string;

    // 9. Let result be DifferenceISODate(one.[[ISOYear]], one.[[ISOMonth]], one.[[ISODay]], two.[[ISOYear]], two.[[ISOMonth]], two.[[ISODay]], largestUnit).
    auto result = difference_iso_date(vm, one->iso_year(), one->iso_month(), one->iso_day(), two->iso_year(), two->iso_month(), two->iso_day(), *largest_unit);

    // 10. Return ! CreateTemporalDuration(result.[[Years]], result.[[Months]], result.[[Weeks]], result.[[Days]], 0, 0, 0, 0, 0, 0).
    return MUST(create_temporal_duration(vm, result.years, result.months, result.weeks, result.days, 0, 0, 0, 0, 0, 0));
}

// 12.4.9 Temporal.Calendar.prototype.year ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.year
// NOTE: This is the minimum year implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::year)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    auto temporal_date_like = vm.argument(0);
    // 4. If Type(temporalDateLike) is not Object or temporalDateLike does not have an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], or [[InitializedTemporalYearMonth]] internal slot, then
    if (!temporal_date_like.is_object() || !(is<PlainDate>(temporal_date_like.as_object()) || is<PlainDateTime>(temporal_date_like.as_object()) || is<PlainYearMonth>(temporal_date_like.as_object()))) {
        // a. Set temporalDateLike to ? ToTemporalDate(temporalDateLike).
        temporal_date_like = TRY(to_temporal_date(vm, temporal_date_like));
    }

    // 5. Assert: temporalDateLike has an [[ISOYear]] internal slot.
    // NOTE: The assertion happens in iso_year() call.

    // 6. Return 𝔽(temporalDateLike.[[ISOYear]]).
    return Value(iso_year(temporal_date_like.as_object()));
}

// 12.4.10 Temporal.Calendar.prototype.month ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.month
// NOTE: This is the minimum month implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::month)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    auto temporal_date_like = vm.argument(0);

    // 4. If Type(temporalDateLike) is Object and temporalDateLike has an [[InitializedTemporalMonthDay]] internal slot, then
    if (temporal_date_like.is_object() && is<PlainMonthDay>(temporal_date_like.as_object())) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::TemporalAmbiguousMonthOfPlainMonthDay);
    }

    // 5. If Type(temporalDateLike) is not Object or temporalDateLike does not have an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], or [[InitializedTemporalYearMonth]] internal slot, then
    if (!temporal_date_like.is_object() || !(is<PlainDate>(temporal_date_like.as_object()) || is<PlainDateTime>(temporal_date_like.as_object()) || is<PlainYearMonth>(temporal_date_like.as_object()))) {
        // a. Set temporalDateLike to ? ToTemporalDate(temporalDateLike).
        temporal_date_like = TRY(to_temporal_date(vm, temporal_date_like));
    }
    // 6. Assert: temporalDateLike has an [[ISOMonth]] internal slot.
    // NOTE: The assertion happens in iso_month() call.

    // 7. Return 𝔽(temporalDateLike.[[ISOMonth]]).
    return Value(iso_month(temporal_date_like.as_object()));
}

// 12.4.11 Temporal.Calendar.prototype.monthCode ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.monthcode
// NOTE: This is the minimum monthCode implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::month_code)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    auto temporal_date_like = vm.argument(0);
    // 4. If Type(temporalDateLike) is not Object or temporalDateLike does not have an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], [[InitializedTemporalMonthDay]], or [[InitializedTemporalYearMonth]] internal slot, then
    if (!temporal_date_like.is_object() || !(is<PlainDate>(temporal_date_like.as_object()) || is<PlainDateTime>(temporal_date_like.as_object()) || is<PlainMonthDay>(temporal_date_like.as_object()) || is<PlainYearMonth>(temporal_date_like.as_object()))) {
        // a. Set temporalDateLike to ? ToTemporalDate(temporalDateLike).
        temporal_date_like = TRY(to_temporal_date(vm, temporal_date_like));
    }

    // 5. Assert: temporalDateLike has an [[ISOMonth]] internal slot.
    // NOTE: The assertion happens in iso_month() call.

    // 6. Return ISOMonthCode(temporalDateLike.[[ISOMonth]]).
    return PrimitiveString::create(vm, TRY(iso_month_code(vm, iso_month(temporal_date_like.as_object()))));
}

// 12.4.12 Temporal.Calendar.prototype.day ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.day
// NOTE: This is the minimum day implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::day)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    auto temporal_date_like = vm.argument(0);
    // 4. If Type(temporalDateLike) is not Object or temporalDateLike does not have an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]] or [[InitializedTemporalMonthDay]] internal slot, then
    if (!temporal_date_like.is_object() || !(is<PlainDate>(temporal_date_like.as_object()) || is<PlainDateTime>(temporal_date_like.as_object()) || is<PlainMonthDay>(temporal_date_like.as_object()))) {
        // a. Set temporalDateLike to ? ToTemporalDate(temporalDateLike).
        temporal_date_like = TRY(to_temporal_date(vm, temporal_date_like));
    }
    // 5. Assert: temporalDateLike has an [[ISODay]] internal slot.
    // NOTE: The assertion happens in iso_day() call.

    // 6. Return 𝔽(temporalDateLike.[[ISODay]]).
    return Value(iso_day(temporal_date_like.as_object()));
}

// 12.4.13 Temporal.Calendar.prototype.dayOfWeek ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.dayofweek
// NOTE: This is the minimum dayOfWeek implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::day_of_week)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    // 4. Let temporalDate be ? ToTemporalDate(temporalDateLike).
    auto* temporal_date = TRY(to_temporal_date(vm, vm.argument(0)));

    // 5. Return 𝔽(ToISODayOfWeek(temporalDate.[[ISOYear]], temporalDate.[[ISOMonth]], temporalDate.[[ISODay]])).
    return Value(to_iso_day_of_week(temporal_date->iso_year(), temporal_date->iso_month(), temporal_date->iso_day()));
}

// 12.4.14 Temporal.Calendar.prototype.dayOfYear ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.dayofyear
// NOTE: This is the minimum dayOfYear implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::day_of_year)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    // 4. Let temporalDate be ? ToTemporalDate(temporalDateLike).
    auto* temporal_date = TRY(to_temporal_date(vm, vm.argument(0)));

    // 5. Return 𝔽(ToISODayOfYear(temporalDate.[[ISOYear]], temporalDate.[[ISOMonth]], temporalDate.[[ISODay]])).
    return Value(to_iso_day_of_year(temporal_date->iso_year(), temporal_date->iso_month(), temporal_date->iso_day()));
}

// 12.4.15 Temporal.Calendar.prototype.weekOfYear ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.weekofyear
// NOTE: This is the minimum weekOfYear implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::week_of_year)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    // 4. Let temporalDate be ? ToTemporalDate(temporalDateLike).
    auto* temporal_date = TRY(to_temporal_date(vm, vm.argument(0)));

    // 5. Let isoYearWeek be ToISOWeekOfYear(temporalDate.[[ISOYear]], temporalDate.[[ISOMonth]], temporalDate.[[ISODay]]).
    auto iso_year_week = to_iso_week_of_year(temporal_date->iso_year(), temporal_date->iso_month(), temporal_date->iso_day());

    // 6. Return 𝔽(isoYearWeek.[[Week]]).
    return Value(iso_year_week.week);
}

// 12.5.16 Temporal.Calendar.prototype.yearOfWeek ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.yearofweek
// NOTE: This is the minimum yearOfWeek implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::year_of_week)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    // 4. Let temporalDate be ? ToTemporalDate(temporalDateLike).
    auto* temporal_date = TRY(to_temporal_date(vm, vm.argument(0)));

    // 5. Let isoYearWeek be ToISOWeekOfYear(temporalDate.[[ISOYear]], temporalDate.[[ISOMonth]], temporalDate.[[ISODay]]).
    auto iso_year_week = to_iso_week_of_year(temporal_date->iso_year(), temporal_date->iso_month(), temporal_date->iso_day());

    // 6. Return 𝔽(isoYearWeek.[[Year]]).
    return Value(iso_year_week.year);
}

// 12.4.17 Temporal.Calendar.prototype.daysInWeek ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.daysinweek
// NOTE: This is the minimum daysInWeek implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::days_in_week)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    // 4. Let temporalDate be ? ToTemporalDate(temporalDateLike).
    [[maybe_unused]] auto* temporal_date = TRY(to_temporal_date(vm, vm.argument(0)));

    // 5. Return 7𝔽.
    return Value(7);
}

// 12.4.18 Temporal.Calendar.prototype.daysInMonth ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.daysinweek
// NOTE: This is the minimum daysInMonth implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::days_in_month)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    auto temporal_date_like = vm.argument(0);
    // 4. If Type(temporalDateLike) is not Object or temporalDateLike does not have an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], or [[InitializedTemporalYearMonth]] internal slots, then
    if (!temporal_date_like.is_object() || !(is<PlainDate>(temporal_date_like.as_object()) || is<PlainDateTime>(temporal_date_like.as_object()) || is<PlainYearMonth>(temporal_date_like.as_object()))) {
        // a. Set temporalDateLike to ? ToTemporalDate(temporalDateLike).
        temporal_date_like = TRY(to_temporal_date(vm, temporal_date_like));
    }

    // 5. Return 𝔽(! ISODaysInMonth(temporalDateLike.[[ISOYear]], temporalDateLike.[[ISOMonth]])).
    return Value(iso_days_in_month(iso_year(temporal_date_like.as_object()), iso_month(temporal_date_like.as_object())));
}

// 12.4.19 Temporal.Calendar.prototype.daysInYear ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.daysinyear
// NOTE: This is the minimum daysInYear implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::days_in_year)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    auto temporal_date_like = vm.argument(0);
    // 4. If Type(temporalDateLike) is not Object or temporalDateLike does not have an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], or [[InitializedTemporalYearMonth]] internal slot, then
    if (!temporal_date_like.is_object() || !(is<PlainDate>(temporal_date_like.as_object()) || is<PlainDateTime>(temporal_date_like.as_object()) || is<PlainYearMonth>(temporal_date_like.as_object()))) {
        // a. Set temporalDateLike to ? ToTemporalDate(temporalDateLike).
        temporal_date_like = TRY(to_temporal_date(vm, temporal_date_like));
    }

    // 5. Return DaysInYear(𝔽(temporalDateLike.[[ISOYear]])).
    return Value(JS::days_in_year(iso_year(temporal_date_like.as_object())));
}

// 12.4.20 Temporal.Calendar.prototype.monthsInYear ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.monthsinyear
// NOTE: This is the minimum monthsInYear implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::months_in_year)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    auto temporal_date_like = vm.argument(0);
    // 4. If Type(temporalDateLike) is not Object or temporalDateLike does not have an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], or [[InitializedTemporalYearMonth]] internal slot, then
    if (!temporal_date_like.is_object() || !(is<PlainDate>(temporal_date_like.as_object()) || is<PlainDateTime>(temporal_date_like.as_object()) || is<PlainYearMonth>(temporal_date_like.as_object()))) {
        // a. Perform ? ToTemporalDate(temporalDateLike).
        (void)TRY(to_temporal_date(vm, temporal_date_like));
    }

    // 5. Return 12𝔽.
    return Value(12);
}

// 12.4.21 Temporal.Calendar.prototype.inLeapYear ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.inleapyear
// NOTE: This is the minimum inLeapYear implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::in_leap_year)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    auto temporal_date_like = vm.argument(0);
    // 4. If Type(temporalDateLike) is not Object or temporalDateLike does not have an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], or [[InitializedTemporalYearMonth]] internal slot, then
    if (!temporal_date_like.is_object() || !(is<PlainDate>(temporal_date_like.as_object()) || is<PlainDateTime>(temporal_date_like.as_object()) || is<PlainYearMonth>(temporal_date_like.as_object()))) {
        // a. Set temporalDateLike to ? ToTemporalDate(temporalDateLike).
        temporal_date_like = TRY(to_temporal_date(vm, temporal_date_like));
    }

    // 5. If InLeapYear(TimeFromYear(𝔽(temporalDateLike.[[ISOYear]]))) is 1𝔽, return true.
    if (JS::in_leap_year(time_from_year(iso_year(temporal_date_like.as_object()))))
        return Value(true);

    // 6. Return false.
    return Value(false);
}

// 12.4.22 Temporal.Calendar.prototype.fields ( fields ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.fields
// NOTE: This is the minimum fields implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::fields)
{
    auto& realm = *vm.current_realm();

    auto fields = vm.argument(0);

    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    // 4. Let iteratorRecord be ? GetIterator(fields, sync).
    auto iterator_record = TRY(get_iterator(vm, fields, IteratorHint::Sync));

    // 5. Let fieldNames be a new empty List.
    auto field_names = MarkedVector<Value> { vm.heap() };

    // 6. Let next be true.
    // 7. Repeat, while next is not false,
    while (true) {
        // a. Set next to ? IteratorStep(iteratorRecord).
        auto next = TRY(iterator_step(vm, iterator_record));

        // b. If next is not false, then
        if (!next)
            break;

        // i. Let nextValue be ? IteratorValue(next).
        auto next_value = TRY(iterator_value(vm, *next));

        // ii. If Type(nextValue) is not String, then
        if (!next_value.is_string()) {
            // 1. Let completion be ThrowCompletion(a newly created TypeError object).
            auto completion = vm.throw_completion<TypeError>(ErrorType::TemporalInvalidCalendarFieldValue, next_value.to_string_without_side_effects());

            // 2. Return ? IteratorClose(iteratorRecord, completion).
            return *TRY(iterator_close(vm, iterator_record, move(completion)));
        }

        auto next_value_string = next_value.as_string().utf8_string();

        // iii. If fieldNames contains nextValue, then
        if (field_names.contains_slow(next_value)) {
            // 1. Let completion be ThrowCompletion(a newly created RangeError object).
            auto completion = vm.throw_completion<RangeError>(ErrorType::TemporalDuplicateCalendarField, next_value_string);

            // 2. Return ? IteratorClose(iteratorRecord, completion).
            return *TRY(iterator_close(vm, iterator_record, move(completion)));
        }

        // iv. If nextValue is not one of "year", "month", "monthCode", "day", "hour", "minute", "second", "millisecond", "microsecond", "nanosecond", then
        if (!next_value_string.is_one_of("year"sv, "month"sv, "monthCode"sv, "day"sv, "hour"sv, "minute"sv, "second"sv, "millisecond"sv, "microsecond"sv, "nanosecond"sv)) {
            // 1. Let completion be ThrowCompletion(a newly created RangeError object).
            auto completion = vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, next_value_string);

            // 2. Return ? IteratorClose(iteratorRecord, completion).
            return *TRY(iterator_close(vm, iterator_record, move(completion)));
        }

        // v. Append nextValue to the end of the List fieldNames.
        field_names.append(next_value);
    }

    // 8. Return CreateArrayFromList(fieldNames).
    return Array::create_from(realm, field_names);
}

// 12.4.23 Temporal.Calendar.prototype.mergeFields ( fields, additionalFields ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.mergefields
// NOTE: This is the minimum mergeFields implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::merge_fields)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Set fields to ? ToObject(fields).
    auto fields = TRY(vm.argument(0).to_object(vm));

    // 4. Set additionalFields to ? ToObject(additionalFields).
    auto additional_fields = TRY(vm.argument(1).to_object(vm));

    // 5. Assert: calendar.[[Identifier]] is "iso8601".
    VERIFY(calendar->identifier() == "iso8601"sv);

    // 6. Return ? DefaultMergeCalendarFields(fields, additionalFields).
    return TRY(default_merge_calendar_fields(vm, fields, additional_fields));
}

// 12.4.24 Temporal.Calendar.prototype.toString ( ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::to_string)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Return calendar.[[Identifier]].
    return PrimitiveString::create(vm, calendar->identifier());
}

// 12.4.25 Temporal.Calendar.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::to_json)
{
    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. Return ? ToString(calendar).
    return PrimitiveString::create(vm, TRY(Value(calendar).to_string(vm)));
}

// 15.6.2.6 Temporal.Calendar.prototype.era ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.era
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::era)
{
    auto temporal_date_like = vm.argument(0);

    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. If Type(temporalDateLike) is not Object or temporalDateLike does not have an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], or [[InitializedTemporalYearMonth]] internal slot, then
    if (!temporal_date_like.is_object() || !(is<PlainDate>(temporal_date_like.as_object()) || is<PlainDateTime>(temporal_date_like.as_object()) || is<PlainYearMonth>(temporal_date_like.as_object()))) {
        // a. Set temporalDateLike to ? ToTemporalDate(temporalDateLike).
        temporal_date_like = TRY(to_temporal_date(vm, temporal_date_like));
    }

    // 4. If calendar.[[Identifier]] is "iso8601", then
    if (calendar->identifier() == "iso8601"sv) {
        // a. Return undefined.
        return js_undefined();
    }

    // 5. Let era be the result of implementation-defined processing of temporalDateLike and calendar.[[Identifier]].
    // 6. Return era.

    // NOTE: No support for non-iso8601 calendars yet.
    VERIFY_NOT_REACHED();
}

// 15.6.2.7 Temporal.Calendar.prototype.eraYear ( temporalDateLike ), https://tc39.es/proposal-temporal/#sec-temporal.calendar.prototype.erayear
JS_DEFINE_NATIVE_FUNCTION(CalendarPrototype::era_year)
{
    auto temporal_date_like = vm.argument(0);

    // 1. Let calendar be the this value.
    // 2. Perform ? RequireInternalSlot(calendar, [[InitializedTemporalCalendar]]).
    auto calendar = TRY(typed_this_object(vm));

    // 3. If Type(temporalDateLike) is not Object or temporalDateLike does not have an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], or [[InitializedTemporalYearMonth]] internal slot, then
    if (!temporal_date_like.is_object() || !(is<PlainDate>(temporal_date_like.as_object()) || is<PlainDateTime>(temporal_date_like.as_object()) || is<PlainYearMonth>(temporal_date_like.as_object()))) {
        // a. Set temporalDateLike to ? ToTemporalDate(temporalDateLike).
        temporal_date_like = TRY(to_temporal_date(vm, temporal_date_like));
    }

    // 4. If calendar.[[Identifier]] is "iso8601", then
    if (calendar->identifier() == "iso8601"sv) {
        // a. Return undefined.
        return js_undefined();
    }

    // 5. Let eraYear be the result of implementation-defined processing of temporalDateLike and calendar.[[Identifier]].
    // 6. Return 𝔽(eraYear).

    // NOTE: No support for non-iso8601 calendars yet.
    VERIFY_NOT_REACHED();
}

static i32 iso_year(Object& temporal_object)
{
    if (is<PlainDate>(temporal_object))
        return static_cast<PlainDate&>(temporal_object).iso_year();
    if (is<PlainDateTime>(temporal_object))
        return static_cast<PlainDateTime&>(temporal_object).iso_year();
    if (is<PlainYearMonth>(temporal_object))
        return static_cast<PlainYearMonth&>(temporal_object).iso_year();
    if (is<PlainMonthDay>(temporal_object))
        return static_cast<PlainMonthDay&>(temporal_object).iso_year();
    VERIFY_NOT_REACHED();
}

static u8 iso_month(Object& temporal_object)
{
    if (is<PlainDate>(temporal_object))
        return static_cast<PlainDate&>(temporal_object).iso_month();
    if (is<PlainDateTime>(temporal_object))
        return static_cast<PlainDateTime&>(temporal_object).iso_month();
    if (is<PlainYearMonth>(temporal_object))
        return static_cast<PlainYearMonth&>(temporal_object).iso_month();
    if (is<PlainMonthDay>(temporal_object))
        return static_cast<PlainMonthDay&>(temporal_object).iso_month();
    VERIFY_NOT_REACHED();
}

static u8 iso_day(Object& temporal_object)
{
    if (is<PlainDate>(temporal_object))
        return static_cast<PlainDate&>(temporal_object).iso_day();
    if (is<PlainDateTime>(temporal_object))
        return static_cast<PlainDateTime&>(temporal_object).iso_day();
    if (is<PlainYearMonth>(temporal_object))
        return static_cast<PlainYearMonth&>(temporal_object).iso_day();
    if (is<PlainMonthDay>(temporal_object))
        return static_cast<PlainMonthDay&>(temporal_object).iso_day();
    VERIFY_NOT_REACHED();
}

}
