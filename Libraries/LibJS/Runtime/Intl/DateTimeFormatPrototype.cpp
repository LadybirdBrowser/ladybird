/*
 * Copyright (c) 2021-2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/DateTimeFormatFunction.h>
#include <LibJS/Runtime/Intl/DateTimeFormatPrototype.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibUnicode/DateTimeFormat.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(DateTimeFormatPrototype);

// 11.3 Properties of the Intl.DateTimeFormat Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-datetimeformat-prototype-object
DateTimeFormatPrototype::DateTimeFormatPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void DateTimeFormatPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 11.3.2 Intl.DateTimeFormat.prototype [ @@toStringTag ], https://tc39.es/ecma402/#sec-intl.datetimeformat.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Intl.DateTimeFormat"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.format, format, nullptr, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.formatToParts, format_to_parts, 1, attr);
    define_native_function(realm, vm.names.formatRange, format_range, 2, attr);
    define_native_function(realm, vm.names.formatRangeToParts, format_range_to_parts, 2, attr);
    define_native_function(realm, vm.names.resolvedOptions, resolved_options, 0, attr);
}

// 11.3.3 get Intl.DateTimeFormat.prototype.format, https://tc39.es/ecma402/#sec-intl.datetimeformat.prototype.format
JS_DEFINE_NATIVE_FUNCTION(DateTimeFormatPrototype::format)
{
    auto& realm = *vm.current_realm();

    // 1. Let dtf be the this value.
    // 2. If the implementation supports the normative optional constructor mode of 4.3 Note 1, then
    //     a. Set dtf to ? UnwrapDateTimeFormat(dtf).
    // 3. Perform ? RequireInternalSlot(dtf, [[InitializedDateTimeFormat]]).
    auto date_time_format = TRY(typed_this_object(vm));

    // 4. If dtf.[[BoundFormat]] is undefined, then
    if (!date_time_format->bound_format()) {
        // a. Let F be a new built-in function object as defined in DateTime Format Functions (11.1.6).
        // b. Set F.[[DateTimeFormat]] to dtf.
        auto bound_format = DateTimeFormatFunction::create(realm, date_time_format);

        // c. Set dtf.[[BoundFormat]] to F.
        date_time_format->set_bound_format(bound_format);
    }

    // 5. Return dtf.[[BoundFormat]].
    return date_time_format->bound_format();
}

// 11.3.4 Intl.DateTimeFormat.prototype.formatToParts ( date ), https://tc39.es/ecma402/#sec-Intl.DateTimeFormat.prototype.formatToParts
JS_DEFINE_NATIVE_FUNCTION(DateTimeFormatPrototype::format_to_parts)
{
    auto& realm = *vm.current_realm();

    auto date = vm.argument(0);

    // 1. Let dtf be the this value.
    // 2. Perform ? RequireInternalSlot(dtf, [[InitializedDateTimeFormat]]).
    auto date_time_format = TRY(typed_this_object(vm));

    double date_value;

    // 3. If date is undefined, then
    if (date.is_undefined()) {
        // a. Let x be ! Call(%Date.now%, undefined).
        date_value = MUST(call(vm, *realm.intrinsics().date_constructor_now_function(), js_undefined())).as_double();
    }
    // 4. Else,
    else {
        // a. Let x be ? ToNumber(date).
        date_value = TRY(date.to_number(vm)).as_double();
    }

    // 5. Return ? FormatDateTimeToParts(dtf, x).
    return TRY(format_date_time_to_parts(vm, date_time_format, date_value));
}

// 11.3.5 Intl.DateTimeFormat.prototype.formatRange ( startDate, endDate ), https://tc39.es/ecma402/#sec-intl.datetimeformat.prototype.formatRange
JS_DEFINE_NATIVE_FUNCTION(DateTimeFormatPrototype::format_range)
{
    auto start_date = vm.argument(0);
    auto end_date = vm.argument(1);

    // 1. Let dtf be this value.
    // 2. Perform ? RequireInternalSlot(dtf, [[InitializedDateTimeFormat]]).
    auto date_time_format = TRY(typed_this_object(vm));

    // 3. If startDate is undefined or endDate is undefined, throw a TypeError exception.
    if (start_date.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "startDate"sv);
    if (end_date.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "endDate"sv);

    // 4. Let x be ? ToNumber(startDate).
    auto start_date_number = TRY(start_date.to_number(vm)).as_double();

    // 5. Let y be ? ToNumber(endDate).
    auto end_date_number = TRY(end_date.to_number(vm)).as_double();

    // 6. Return ? FormatDateTimeRange(dtf, x, y).
    auto formatted = TRY(format_date_time_range(vm, date_time_format, start_date_number, end_date_number));
    return PrimitiveString::create(vm, move(formatted));
}

// 11.3.6 Intl.DateTimeFormat.prototype.formatRangeToParts ( startDate, endDate ), https://tc39.es/ecma402/#sec-Intl.DateTimeFormat.prototype.formatRangeToParts
JS_DEFINE_NATIVE_FUNCTION(DateTimeFormatPrototype::format_range_to_parts)
{
    auto start_date = vm.argument(0);
    auto end_date = vm.argument(1);

    // 1. Let dtf be this value.
    // 2. Perform ? RequireInternalSlot(dtf, [[InitializedDateTimeFormat]]).
    auto date_time_format = TRY(typed_this_object(vm));

    // 3. If startDate is undefined or endDate is undefined, throw a TypeError exception.
    if (start_date.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "startDate"sv);
    if (end_date.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "endDate"sv);

    // 4. Let x be ? ToNumber(startDate).
    auto start_date_number = TRY(start_date.to_number(vm)).as_double();

    // 5. Let y be ? ToNumber(endDate).
    auto end_date_number = TRY(end_date.to_number(vm)).as_double();

    // 6. Return ? FormatDateTimeRangeToParts(dtf, x, y).
    return TRY(format_date_time_range_to_parts(vm, date_time_format, start_date_number, end_date_number));
}

// 11.3.7 Intl.DateTimeFormat.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-intl.datetimeformat.prototype.resolvedoptions
JS_DEFINE_NATIVE_FUNCTION(DateTimeFormatPrototype::resolved_options)
{
    auto& realm = *vm.current_realm();

    // 1. Let dtf be the this value.
    // 2. If the implementation supports the normative optional constructor mode of 4.3 Note 1, then
    //     a. Set dtf to ? UnwrapDateTimeFormat(dtf).
    // 3. Perform ? RequireInternalSlot(dtf, [[InitializedDateTimeFormat]]).
    auto date_time_format = TRY(typed_this_object(vm));

    // 4. Let options be OrdinaryObjectCreate(%Object.prototype%).
    auto options = Object::create(realm, realm.intrinsics().object_prototype());

    // 5. For each row of Table 15, except the header row, in table order, do
    //    a. Let p be the Property value of the current row.
    //    b. If there is an Internal Slot value in the current row, then
    //        i. Let v be the value of dtf's internal slot whose name is the Internal Slot value of the current row.
    //    c. Else,
    //        i. Let format be dtf.[[DateTimeFormat]].
    //        ii. If format has a field [[<p>]] and dtf.[[DateStyle]] is undefined and dtf.[[TimeStyle]] is undefined, then
    //            1. Let v be format.[[<p>]].
    //        iii. Else,
    //            1. Let v be undefined.
    //    d. If v is not undefined, then
    //        i. If there is a Conversion value in the current row, then
    //            1. Let conversion be the Conversion value of the current row.
    //            2. If conversion is hour12, then
    //                a. If v is "h11" or "h12", set v to true. Otherwise, set v to false.
    //            3. Else,
    //                a. Assert: conversion is number.
    //                b. Set v to 𝔽(v).
    //        ii. Perform ! CreateDataPropertyOrThrow(options, p, v).
    MUST(options->create_data_property_or_throw(vm.names.locale, PrimitiveString::create(vm, date_time_format->locale())));
    MUST(options->create_data_property_or_throw(vm.names.calendar, PrimitiveString::create(vm, date_time_format->calendar())));
    MUST(options->create_data_property_or_throw(vm.names.numberingSystem, PrimitiveString::create(vm, date_time_format->numbering_system())));
    MUST(options->create_data_property_or_throw(vm.names.timeZone, PrimitiveString::create(vm, date_time_format->time_zone())));

    if (auto const hour_cycle = date_time_format->date_time_format().hour_cycle; hour_cycle.has_value()) {
        MUST(options->create_data_property_or_throw(vm.names.hourCycle, PrimitiveString::create(vm, Unicode::hour_cycle_to_string(*hour_cycle))));

        switch (*hour_cycle) {
        case Unicode::HourCycle::H11:
        case Unicode::HourCycle::H12:
            MUST(options->create_data_property_or_throw(vm.names.hour12, Value(true)));
            break;
        case Unicode::HourCycle::H23:
        case Unicode::HourCycle::H24:
            MUST(options->create_data_property_or_throw(vm.names.hour12, Value(false)));
            break;
        }
    }

    if (!date_time_format->has_date_style() && !date_time_format->has_time_style()) {
        MUST(for_each_calendar_field(vm, date_time_format->date_time_format(), [&](auto& option, auto const& property, auto const&) -> ThrowCompletionOr<void> {
            using ValueType = typename RemoveReference<decltype(option)>::ValueType;

            if (!option.has_value())
                return {};

            if constexpr (IsIntegral<ValueType>) {
                MUST(options->create_data_property_or_throw(property, Value(*option)));
            } else {
                auto name = Unicode::calendar_pattern_style_to_string(*option);
                MUST(options->create_data_property_or_throw(property, PrimitiveString::create(vm, name)));
            }

            return {};
        }));
    }

    if (date_time_format->has_date_style())
        MUST(options->create_data_property_or_throw(vm.names.dateStyle, PrimitiveString::create(vm, date_time_format->date_style_string())));
    if (date_time_format->has_time_style())
        MUST(options->create_data_property_or_throw(vm.names.timeStyle, PrimitiveString::create(vm, date_time_format->time_style_string())));

    // 6. Return options.
    return options;
}

}
