/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/Calendar.h>
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
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);
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
// NOTE: This is the minimum toLocaleString implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::to_locale_string)
{
    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Return TemporalMonthDayToString(monthDay, auto).
    return PrimitiveString::create(vm, temporal_month_day_to_string(month_day, ShowCalendar::Auto));
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

}
