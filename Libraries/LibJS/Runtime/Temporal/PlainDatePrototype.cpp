/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainDatePrototype.h>

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
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);
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

}
