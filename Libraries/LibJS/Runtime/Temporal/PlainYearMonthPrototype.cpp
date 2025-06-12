/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
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
#include <LibJS/Runtime/Temporal/PlainYearMonthPrototype.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainYearMonthPrototype);

// 9.3 Properties of the Temporal.PlainYearMonth Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plainyearmonth-prototype-object
PlainYearMonthPrototype::PlainYearMonthPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void PlainYearMonthPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 9.3.2 Temporal.PlainYearMonth.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.PlainYearMonth"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.calendarId, calendar_id_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.era, era_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.eraYear, era_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.year, year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.month, month_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.monthCode, month_code_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.daysInYear, days_in_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.daysInMonth, days_in_month_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.monthsInYear, months_in_year_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.inLeapYear, in_leap_year_getter, {}, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.with, with, 1, attr);
    define_native_function(realm, vm.names.add, add, 1, attr);
    define_native_function(realm, vm.names.subtract, subtract, 1, attr);
    define_native_function(realm, vm.names.until, until, 1, attr);
    define_native_function(realm, vm.names.since, since, 1, attr);
    define_native_function(realm, vm.names.equals, equals, 1, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);
    define_native_function(realm, vm.names.valueOf, value_of, 0, attr);
    define_native_function(realm, vm.names.toPlainDate, to_plain_date, 1, attr);
}

// 9.3.3 get Temporal.PlainYearMonth.prototype.calendarId, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.calendarid
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::calendar_id_getter)
{
    // 1. Let yearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. Return yearMonth.[[Calendar]].
    return PrimitiveString::create(vm, year_month->calendar());
}

// 9.3.4 get Temporal.PlainYearMonth.prototype.era, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.era
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::era_getter)
{
    // 1. Let plainYearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. Return CalendarISOToDate(plainYearMonth.[[Calendar]], plainYearMonth.[[ISODate]]).[[Era]].
    auto result = calendar_iso_to_date(year_month->calendar(), year_month->iso_date()).era;

    if (!result.has_value())
        return js_undefined();

    return PrimitiveString::create(vm, result.release_value());
}

// 9.3.5 get Temporal.PlainYearMonth.prototype.eraYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.erayear
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::era_year_getter)
{
    // 1. Let plainYearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(plainYearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. Let result be CalendarISOToDate(plainYearMonth.[[Calendar]], plainYearMonth.[[ISODate]]).[[EraYear]].
    auto result = calendar_iso_to_date(year_month->calendar(), year_month->iso_date()).era_year;

    // 4. If result is undefined, return undefined.
    if (!result.has_value())
        return js_undefined();

    // 5. Return 𝔽(result).
    return *result;
}

#define JS_ENUMERATE_PLAIN_MONTH_YEAR_SIMPLE_FIELDS \
    __JS_ENUMERATE(year)                            \
    __JS_ENUMERATE(month)                           \
    __JS_ENUMERATE(days_in_year)                    \
    __JS_ENUMERATE(days_in_month)                   \
    __JS_ENUMERATE(months_in_year)                  \
    __JS_ENUMERATE(in_leap_year)

// 9.3.6 get Temporal.PlainYearMonth.prototype.year, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.year
// 9.3.7 get Temporal.PlainYearMonth.prototype.month, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.month
// 9.3.9 get Temporal.PlainYearMonth.prototype.daysInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.daysinyear
// 9.3.10 get Temporal.PlainYearMonth.prototype.daysInMonth, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.daysinmonth
// 9.3.11 get Temporal.PlainYearMonth.prototype.monthsInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.monthsinyear
// 9.3.12 get Temporal.PlainYearMonth.prototype.inLeapYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.inleapyear
#define __JS_ENUMERATE(field)                                                                         \
    JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::field##_getter)                                \
    {                                                                                                 \
        /* 1. Let yearMonth be the this value. */                                                     \
        /* 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]). */          \
        auto year_month = TRY(typed_this_object(vm));                                                 \
                                                                                                      \
        /* 3. Return CalendarISOToDate(yearMonth.[[Calendar]], yearMonth.[[ISODate]]).[[<field>]]. */ \
        return calendar_iso_to_date(year_month->calendar(), year_month->iso_date()).field;            \
    }
JS_ENUMERATE_PLAIN_MONTH_YEAR_SIMPLE_FIELDS
#undef __JS_ENUMERATE

// 9.3.8 get Temporal.PlainYearMonth.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.plainyearmonth.prototype.monthcode
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::month_code_getter)
{
    // 1. Let yearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. Return CalendarISOToDate(yearMonth.[[Calendar]], yearMonth.[[ISODate]]).[[MonthCode]].
    auto month_code = calendar_iso_to_date(year_month->calendar(), year_month->iso_date()).month_code;
    return PrimitiveString::create(vm, move(month_code));
}

// 9.3.13 Temporal.PlainYearMonth.prototype.with ( temporalYearMonthLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.with
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::with)
{
    auto temporal_year_month_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let yearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. If ? IsPartialTemporalObject(temporalYearMonthLike) is false, throw a TypeError exception.
    if (!TRY(is_partial_temporal_object(vm, temporal_year_month_like)))
        return vm.throw_completion<TypeError>(ErrorType::TemporalObjectMustBePartialTemporalObject);

    // 4. Let calendar be yearMonth.[[Calendar]].
    auto const& calendar = year_month->calendar();

    // 5. Let fields be ISODateToFields(calendar, yearMonth.[[ISODate]], YEAR-MONTH).
    auto fields = iso_date_to_fields(calendar, year_month->iso_date(), DateType::YearMonth);

    // 6. Let partialYearMonth be ? PrepareCalendarFields(calendar, temporalYearMonthLike, « YEAR, MONTH, MONTH-CODE », « », PARTIAL).
    auto partial_year_month = TRY(prepare_calendar_fields(vm, calendar, temporal_year_month_like.as_object(), { { CalendarField::Year, CalendarField::Month, CalendarField::MonthCode } }, {}, Partial {}));

    // 7. Set fields to CalendarMergeFields(calendar, fields, partialYearMonth).
    fields = calendar_merge_fields(calendar, fields, partial_year_month);

    // 8. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 9. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
    auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

    // 10. Let isoDate be ? CalendarYearMonthFromFields(calendar, fields, overflow).
    auto iso_date = TRY(calendar_year_month_from_fields(vm, calendar, fields, overflow));

    // 11. Return ! CreateTemporalYearMonth(isoDate, calendar).
    return MUST(create_temporal_year_month(vm, iso_date, calendar));
}

// 9.3.14 Temporal.PlainYearMonth.prototype.add ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.add
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::add)
{
    auto temporal_duration_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let yearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToYearMonth(ADD, yearMonth, temporalDurationLike, options).
    return TRY(add_duration_to_year_month(vm, ArithmeticOperation::Add, year_month, temporal_duration_like, options));
}

// 9.3.15 Temporal.PlainYearMonth.prototype.subtract ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.subtract
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::subtract)
{
    auto temporal_duration_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let yearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToYearMonth(SUBTRACT, yearMonth, temporalDurationLike, options).
    return TRY(add_duration_to_year_month(vm, ArithmeticOperation::Subtract, year_month, temporal_duration_like, options));
}

// 9.3.16 Temporal.PlainYearMonth.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.until
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::until)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let yearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalPlainYearMonth(UNTIL, yearMonth, other, options).
    return TRY(difference_temporal_plain_year_month(vm, DurationOperation::Until, year_month, other, options));
}

// 9.3.17 Temporal.PlainYearMonth.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.since
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::since)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let yearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalPlainYearMonth(SINCE, yearMonth, other, options).
    return TRY(difference_temporal_plain_year_month(vm, DurationOperation::Since, year_month, other, options));
}

// 9.3.18 Temporal.PlainYearMonth.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.equals
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::equals)
{
    // 1. Let yearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. Set other to ? ToTemporalYearMonth(other).
    auto other = TRY(to_temporal_year_month(vm, vm.argument(0)));

    // 4. If CompareISODate(yearMonth.[[ISODate]], other.[[ISODate]]) ≠ 0, return false.
    if (compare_iso_date(year_month->iso_date(), other->iso_date()) != 0)
        return false;

    // 5. Return CalendarEquals(yearMonth.[[Calendar]], other.[[Calendar]]).
    return calendar_equals(year_month->calendar(), other->calendar());
}

// 9.3.19 Temporal.PlainYearMonth.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::to_string)
{
    // 1. Let yearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, vm.argument(0)));

    // 4. Let showCalendar be ? GetTemporalShowCalendarNameOption(resolvedOptions).
    auto show_calendar = TRY(get_temporal_show_calendar_name_option(vm, resolved_options));

    // 5. Return TemporalYearMonthToString(yearMonth, showCalendar).
    return PrimitiveString::create(vm, temporal_year_month_to_string(year_month, show_calendar));
}

// 9.3.20 Temporal.PlainYearMonth.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.tolocalestring
// 15.12.7.1 Temporal.PlainYearMonth.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sup-temporal.plainyearmonth.prototype.tolocalestring
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::to_locale_string)
{
    auto& realm = *vm.current_realm();

    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let yearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. Let dateFormat be ? CreateDateTimeFormat(%Intl.DateTimeFormat%, locales, options, DATE, DATE).
    auto date_format = TRY(Intl::create_date_time_format(vm, realm.intrinsics().intl_date_time_format_constructor(), locales, options, Intl::OptionRequired::Date, Intl::OptionDefaults::Date));

    // 4. Return ? FormatDateTime(dateFormat, yearMonth).
    return PrimitiveString::create(vm, TRY(Intl::format_date_time(vm, date_format, year_month)));
}

// 9.3.21 Temporal.PlainYearMonth.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::to_json)
{
    // 1. Let yearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. Return TemporalYearMonthToString(yearMonth, AUTO).
    return PrimitiveString::create(vm, temporal_year_month_to_string(year_month, ShowCalendar::Auto));
}

// 9.3.22 Temporal.PlainYearMonth.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.PlainYearMonth", "a primitive value");
}

// 9.3.23 Temporal.PlainYearMonth.prototype.toPlainDate ( item ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype.toplaindate
JS_DEFINE_NATIVE_FUNCTION(PlainYearMonthPrototype::to_plain_date)
{
    auto item = vm.argument(0);

    // 1. Let yearMonth be the this value.
    // 2. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month = TRY(typed_this_object(vm));

    // 3. If item is not an Object, then
    if (!item.is_object()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, item);
    }

    // 4. Let calendar be yearMonth.[[Calendar]].
    auto const& calendar = year_month->calendar();

    // 5. Let fields be ISODateToFields(calendar, yearMonth.[[ISODate]], YEAR-MONTH).
    auto fields = iso_date_to_fields(calendar, year_month->iso_date(), DateType::YearMonth);

    // 6. Let inputFields be ? PrepareCalendarFields(calendar, item, « DAY », « », « »).
    auto input_fields = TRY(prepare_calendar_fields(vm, calendar, item.as_object(), { { CalendarField::Day } }, {}, CalendarFieldList {}));

    // 7. Let mergedFields be CalendarMergeFields(calendar, fields, inputFields).
    auto merged_fields = calendar_merge_fields(calendar, fields, input_fields);

    // 8. Let isoDate be ? CalendarDateFromFields(calendar, mergedFields, CONSTRAIN).
    auto iso_date = TRY(calendar_date_from_fields(vm, calendar, merged_fields, Overflow::Constrain));

    // 9. Return ! CreateTemporalDate(isoDate, calendar).
    return MUST(create_temporal_date(vm, iso_date, calendar));
}

}
