/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/Calendar.h>
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

}
