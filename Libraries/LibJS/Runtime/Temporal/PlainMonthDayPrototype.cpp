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

}
