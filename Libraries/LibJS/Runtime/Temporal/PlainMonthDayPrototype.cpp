/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Intl/DateTimeFormat.h>
#include <LibJS/Runtime/Intl/DateTimeFormatConstructor.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainMonthDayPrototype.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainMonthDayPrototype);

// 10.3 Properties of the Temporal.PlainMonthDay Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plainmonthday-prototype-object
PlainMonthDayPrototype::PlainMonthDayPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void PlainMonthDayPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 10.3.2 Temporal.PlainMonthDay.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.PlainMonthDay"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.calendarId, calendar_id_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.monthCode, month_code_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.day, day_getter, {}, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.with, with, 1, attr);
    define_native_function(realm, vm.names.equals, equals, 1, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);
    define_native_function(realm, vm.names.valueOf, value_of, 0, attr);
    define_native_function(realm, vm.names.toPlainDate, to_plain_date, 1, attr);
}

// 10.3.3 get Temporal.PlainMonthDay.prototype.calendarId, https://tc39.es/proposal-temporal/#sec-get-temporal.plainmonthday.prototype.calendarid
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::calendar_id_getter)
{
    // 1. Let monthDay be the this value
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Return monthDay.[[Calendar]].
    return PrimitiveString::create(vm, month_day->calendar());
}

// 10.3.4 get Temporal.PlainMonthDay.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.plainmonthday.prototype.monthcode
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::month_code_getter)
{
    // 1. Let monthDay be the this value
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Return CalendarISOToDate(monthDay.[[Calendar]], monthDay.[[ISODate]]).[[MonthCode]].
    return PrimitiveString::create(vm, calendar_iso_to_date(month_day->calendar(), month_day->iso_date()).month_code);
}

// 10.3.5 get Temporal.PlainMonthDay.prototype.day, https://tc39.es/proposal-temporal/#sec-get-temporal.plainmonthday.prototype.day
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::day_getter)
{
    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Return 𝔽(CalendarISOToDate(monthDay.[[Calendar]], monthDay.[[ISODate]]).[[Day]]).
    return calendar_iso_to_date(month_day->calendar(), month_day->iso_date()).day;
}

// 10.3.6 Temporal.PlainMonthDay.prototype.with ( temporalMonthDayLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.with
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::with)
{
    auto temporal_month_day_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. If ? IsPartialTemporalObject(temporalMonthDayLike) is false, throw a TypeError exception.
    if (!TRY(is_partial_temporal_object(vm, temporal_month_day_like)))
        return vm.throw_completion<TypeError>(ErrorType::TemporalObjectMustBePartialTemporalObject);

    // 4. Let calendar be monthDay.[[Calendar]].
    auto const& calendar = month_day->calendar();

    // 5. Let fields be ISODateToFields(calendar, monthDay.[[ISODate]], MONTH-DAY).
    auto fields = iso_date_to_fields(calendar, month_day->iso_date(), DateType::MonthDay);

    // 6. Let partialMonthDay be ? PrepareCalendarFields(calendar, temporalMonthDayLike, « YEAR, MONTH, MONTH-CODE, DAY », « », PARTIAL).
    auto partial_month_day = TRY(prepare_calendar_fields(vm, calendar, temporal_month_day_like.as_object(), { { CalendarField::Year, CalendarField::Month, CalendarField::MonthCode, CalendarField::Day } }, {}, Partial {}));

    // 7. Set fields to CalendarMergeFields(calendar, fields, partialMonthDay).
    fields = calendar_merge_fields(calendar, fields, partial_month_day);

    // 8. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 9. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
    auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

    // 10. Let isoDate be ? CalendarMonthDayFromFields(calendar, fields, overflow).
    auto iso_date = TRY(calendar_month_day_from_fields(vm, calendar, fields, overflow));

    // 11. Return ! CreateTemporalMonthDay(isoDate, calendar).
    return MUST(create_temporal_month_day(vm, iso_date, calendar));
}

// 10.3.7 Temporal.PlainMonthDay.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.equals
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::equals)
{
    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Set other to ? ToTemporalMonthDay(other).
    auto other = TRY(to_temporal_month_day(vm, vm.argument(0)));

    // 4. If CompareISODate(monthDay.[[ISODate]], other.[[ISODate]]) ≠ 0, return false.
    if (compare_iso_date(month_day->iso_date(), other->iso_date()) != 0)
        return false;

    // 5. Return CalendarEquals(monthDay.[[Calendar]], other.[[Calendar]]).
    return calendar_equals(month_day->calendar(), other->calendar());
}

// 10.3.8 Temporal.PlainMonthDay.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::to_string)
{
    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, vm.argument(0)));

    // 4. Let showCalendar be ? GetTemporalShowCalendarNameOption(resolvedOptions).
    auto show_calendar = TRY(get_temporal_show_calendar_name_option(vm, resolved_options));

    // 5. Return TemporalMonthDayToString(monthDay, showCalendar).
    return PrimitiveString::create(vm, temporal_month_day_to_string(month_day, show_calendar));
}

// 10.3.9 Temporal.PlainMonthDay.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.tolocalestring
// 15.12.5.1 Temporal.PlainMonthDay.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sup-temporal.plainmonthday.prototype.tolocalestring
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::to_locale_string)
{
    auto& realm = *vm.current_realm();

    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Let dateFormat be ? CreateDateTimeFormat(%Intl.DateTimeFormat%, locales, options, DATE, DATE).
    auto date_format = TRY(Intl::create_date_time_format(vm, realm.intrinsics().intl_date_time_format_constructor(), locales, options, Intl::OptionRequired::Date, Intl::OptionDefaults::Date));

    // 4. Return ? FormatDateTime(dateFormat, monthDay).
    return PrimitiveString::create(vm, TRY(Intl::format_date_time(vm, date_format, month_day)));
}

// 10.3.10 Temporal.PlainMonthDay.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.tolocalestring
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::to_json)
{
    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Return TemporalMonthDayToString(monthDay, auto).
    return PrimitiveString::create(vm, temporal_month_day_to_string(month_day, ShowCalendar::Auto));
}

// 10.3.11 Temporal.PlainMonthDay.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.PlainMonthDay", "a primitive value");
}

// 10.3.12 Temporal.PlainMonthDay.prototype.toPlainDate ( item ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.toplaindate
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::to_plain_date)
{
    auto item = vm.argument(0);

    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. If item is not an Object, then
    if (!item.is_object()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, item);
    }

    // 4. Let calendar be monthDay.[[Calendar]].
    auto const& calendar = month_day->calendar();

    // 5. Let fields be ISODateToFields(calendar, monthDay.[[ISODate]], MONTH-DAY).
    auto fields = iso_date_to_fields(calendar, month_day->iso_date(), DateType::MonthDay);

    // 6. Let inputFields be ? PrepareCalendarFields(calendar, item, « YEAR », « », « »).
    auto input_fields = TRY(prepare_calendar_fields(vm, calendar, item.as_object(), { { CalendarField::Year } }, {}, CalendarFieldList {}));

    // 7. Let mergedFields be CalendarMergeFields(calendar, fields, inputFields).
    auto merged_fields = calendar_merge_fields(calendar, fields, input_fields);

    // 8. Let isoDate be ? CalendarDateFromFields(calendar, mergedFields, CONSTRAIN).
    auto iso_date = TRY(calendar_date_from_fields(vm, calendar, merged_fields, Overflow::Constrain));

    // 9. Return ! CreateTemporalDate(isoDate, calendar).
    return MUST(create_temporal_date(vm, iso_date, calendar));
}

}
