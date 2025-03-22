/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/StringBuilder.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/DurationFormatPrototype.h>
#include <LibJS/Runtime/Temporal/Duration.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(DurationFormatPrototype);

// 13.3 Properties of the Intl.DurationFormat Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-durationformat-prototype-object
DurationFormatPrototype::DurationFormatPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void DurationFormatPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 13.3.5 Intl.DurationFormat.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Intl.DurationFormat"_string), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.resolvedOptions, resolved_options, 0, attr);
    define_native_function(realm, vm.names.format, format, 1, attr);
    define_native_function(realm, vm.names.formatToParts, format_to_parts, 1, attr);
}

// 13.3.2 Intl.DurationFormat.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype.resolvedOptions
JS_DEFINE_NATIVE_FUNCTION(DurationFormatPrototype::resolved_options)
{
    auto& realm = *vm.current_realm();

    // 1. Let df be the this value.
    // 2. Perform ? RequireInternalSlot(df, [[InitializedDurationFormat]]).
    auto duration_format = TRY(typed_this_object(vm));

    // 3. Let options be OrdinaryObjectCreate(%Object.prototype%).
    auto options = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. For each row of Table 21, except the header row, in table order, do
    auto create_option = [&]<typename T>(PropertyKey const& property, Optional<PropertyKey const&> display_property, T value) {
        // a. Let p be the Property value of the current row.
        // b. Let v be the value of df's internal slot whose name is the Internal Slot value of the current row.

        // c. If v is not undefined, then
        //     i. If there is a Conversion value in the current row, let conversion be that value; else let conversion be empty.
        //     ii. If conversion is number, then
        //         1. Set v to 𝔽(v).
        // NOTE: This case is for fractionalDigits and is handled separately below.

        //     iii. Else if conversion is not empty, then
        if constexpr (IsSame<T, DurationFormat::DurationUnitOptions>) {
            // 1. Assert: conversion is STYLE+DISPLAY and v is a Duration Unit Options Record.
            // 2. NOTE: v.[[Style]] will be represented with a property named p (a plural Temporal unit), then v.[[Display]] will be represented with a property whose name suffixes p with "Display".
            VERIFY(display_property.has_value());

            // 3. Let style be v.[[Style]].
            auto style = value.style;

            // 4. If style is "fractional", then
            if (style == DurationFormat::ValueStyle::Fractional) {
                // a. Assert: IsFractionalSecondUnitName(p) is true.
                // b. Set style to "numeric".
                style = DurationFormat::ValueStyle::Numeric;
            }

            // 5. Perform ! CreateDataPropertyOrThrow(options, p, style).
            MUST(options->create_data_property_or_throw(property, PrimitiveString::create(vm, DurationFormat::value_style_to_string(style))));

            // 6. Set p to the string-concatenation of p and "Display".
            // 7. Set v to v.[[Display]].
            MUST(options->create_data_property_or_throw(*display_property, PrimitiveString::create(vm, DurationFormat::display_to_string(value.display))));
        } else {
            // iv. Perform ! CreateDataPropertyOrThrow(options, p, v).
            MUST(options->create_data_property_or_throw(property, PrimitiveString::create(vm, move(value))));
        }
    };

    create_option(vm.names.locale, {}, duration_format->locale());
    create_option(vm.names.numberingSystem, {}, duration_format->numbering_system());
    create_option(vm.names.style, {}, duration_format->style_string());
    create_option(vm.names.years, vm.names.yearsDisplay, duration_format->years_options());
    create_option(vm.names.months, vm.names.monthsDisplay, duration_format->months_options());
    create_option(vm.names.weeks, vm.names.weeksDisplay, duration_format->weeks_options());
    create_option(vm.names.days, vm.names.daysDisplay, duration_format->days_options());
    create_option(vm.names.hours, vm.names.hoursDisplay, duration_format->hours_options());
    create_option(vm.names.minutes, vm.names.minutesDisplay, duration_format->minutes_options());
    create_option(vm.names.seconds, vm.names.secondsDisplay, duration_format->seconds_options());
    create_option(vm.names.milliseconds, vm.names.millisecondsDisplay, duration_format->milliseconds_options());
    create_option(vm.names.microseconds, vm.names.microsecondsDisplay, duration_format->microseconds_options());
    create_option(vm.names.nanoseconds, vm.names.nanosecondsDisplay, duration_format->nanoseconds_options());

    if (duration_format->has_fractional_digits())
        MUST(options->create_data_property_or_throw(vm.names.fractionalDigits, Value(duration_format->fractional_digits())));

    // 5. Return options.
    return options;
}

// 13.3.3 Intl.DurationFormat.prototype.format ( duration ), https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype.format
// 15.10.1 Intl.DurationFormat.prototype.format ( durationLike ), https://tc39.es/proposal-temporal/#sec-Intl.DurationFormat.prototype.format
JS_DEFINE_NATIVE_FUNCTION(DurationFormatPrototype::format)
{
    // 1. Let df be this value.
    // 2. Perform ? RequireInternalSlot(df, [[InitializedDurationFormat]]).
    auto duration_format = TRY(typed_this_object(vm));

    // 3. Let duration be ? ToTemporalDuration(durationLike).
    auto duration = TRY(Temporal::to_temporal_duration(vm, vm.argument(0)));

    // 4. Let parts be PartitionDurationFormatPattern(df, duration).
    auto parts = partition_duration_format_pattern(vm, duration_format, duration);

    // 5. Let result be a new empty String.
    StringBuilder result;

    // 6. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
    for (auto const& part : parts) {
        // a. Set result to the string-concatenation of result and part.[[Value]].
        result.append(part.value);
    }

    // 7. Return result.
    return PrimitiveString::create(vm, MUST(result.to_string()));
}

// 13.3.4 Intl.DurationFormat.prototype.formatToParts ( duration ), https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype.formatToParts
// 15.10.2 Intl.DurationFormat.prototype.formatToParts ( durationLike ), https://tc39.es/proposal-temporal/#sec-Intl.DurationFormat.prototype.formatToParts
JS_DEFINE_NATIVE_FUNCTION(DurationFormatPrototype::format_to_parts)
{
    auto& realm = *vm.current_realm();

    // 1. Let df be this value.
    // 2. Perform ? RequireInternalSlot(df, [[InitializedDurationFormat]]).
    auto duration_format = TRY(typed_this_object(vm));

    // 3. Let duration be ? ToTemporalDuration(durationLike).
    auto duration = TRY(Temporal::to_temporal_duration(vm, vm.argument(0)));

    // 4. Let parts be PartitionDurationFormatPattern(df, duration).
    auto parts = partition_duration_format_pattern(vm, duration_format, duration);

    // 5. Let result be ! ArrayCreate(0).
    auto result = MUST(Array::create(realm, 0));

    // 6. Let n be 0.
    // 7. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
    for (auto [n, part] : enumerate(parts)) {
        // a. Let obj be OrdinaryObjectCreate(%Object.prototype%).
        auto object = Object::create(realm, realm.intrinsics().object_prototype());

        // b. Perform ! CreateDataPropertyOrThrow(obj, "type", part.[[Type]]).
        MUST(object->create_data_property_or_throw(vm.names.type, PrimitiveString::create(vm, part.type)));

        // c. Perform ! CreateDataPropertyOrThrow(obj, "value", part.[[Value]]).
        MUST(object->create_data_property_or_throw(vm.names.value, PrimitiveString::create(vm, move(part.value))));

        // d. If part.[[Unit]] is not empty, perform ! CreateDataPropertyOrThrow(obj, "unit", part.[[Unit]]).
        if (!part.unit.is_empty())
            MUST(object->create_data_property_or_throw(vm.names.unit, PrimitiveString::create(vm, part.unit)));

        // e. Perform ! CreateDataPropertyOrThrow(result, ! ToString(n), obj).
        MUST(result->create_data_property_or_throw(n, object));

        // f. Set n to n + 1.
    }

    // 8. Return result.
    return result;
}

}
