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

// 13.4 Properties of the Intl.DurationFormat Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-durationformat-prototype-object
DurationFormatPrototype::DurationFormatPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void DurationFormatPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 13.4.2 Intl.DurationFormat.prototype [ @@toStringTag ], https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Intl.DurationFormat"_string), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.format, format, 1, attr);
    define_native_function(realm, vm.names.formatToParts, format_to_parts, 1, attr);
    define_native_function(realm, vm.names.resolvedOptions, resolved_options, 0, attr);
}

// 13.4.3 Intl.DurationFormat.prototype.format ( duration ), https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype.format
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

// 13.4.4 Intl.DurationFormat.prototype.formatToParts ( duration ), https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype.formatToParts
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

// 13.4.5 Intl.DurationFormat.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-Intl.DurationFormat.prototype.resolvedOptions
JS_DEFINE_NATIVE_FUNCTION(DurationFormatPrototype::resolved_options)
{
    auto& realm = *vm.current_realm();

    // 1. Let df be the this value.
    // 2. Perform ? RequireInternalSlot(df, [[InitializedDurationFormat]]).
    auto duration_format = TRY(typed_this_object(vm));

    // 3. Let options be OrdinaryObjectCreate(%Object.prototype%).
    auto options = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. For each row of Table 23, except the header row, in table order, do
    auto create_option = [&](PropertyKey const& property, StringView value) {
        // a. Let p be the Property value of the current row.
        // b. Let v be the value of df's internal slot whose name is the Internal Slot value of the current row.

        // c. If p is "fractionalDigits", then
        //     i. If v is not undefined, set v to 𝔽(v).
        // d. Else,
        //     i. Assert: v is not undefined.

        // e. If v is "fractional", then
        if (value == "fractional"sv) {
            // i. Assert: The Internal Slot value of the current row is [[MillisecondsStyle]], [[MicrosecondsStyle]], or [[NanosecondsStyle]].
            // ii. Set v to "numeric".
            value = "numeric"sv;
        }

        // f. If v is not undefined, then
        //     i. Perform ! CreateDataPropertyOrThrow(options, p, v).
        MUST(options->create_data_property_or_throw(property, PrimitiveString::create(vm, value)));
    };

    create_option(vm.names.locale, duration_format->locale());
    create_option(vm.names.numberingSystem, duration_format->numbering_system());
    create_option(vm.names.style, duration_format->style_string());
    create_option(vm.names.years, duration_format->years_style_string());
    create_option(vm.names.yearsDisplay, duration_format->years_display_string());
    create_option(vm.names.months, duration_format->months_style_string());
    create_option(vm.names.monthsDisplay, duration_format->months_display_string());
    create_option(vm.names.weeks, duration_format->weeks_style_string());
    create_option(vm.names.weeksDisplay, duration_format->weeks_display_string());
    create_option(vm.names.days, duration_format->days_style_string());
    create_option(vm.names.daysDisplay, duration_format->days_display_string());
    create_option(vm.names.hours, duration_format->hours_style_string());
    create_option(vm.names.hoursDisplay, duration_format->hours_display_string());
    create_option(vm.names.minutes, duration_format->minutes_style_string());
    create_option(vm.names.minutesDisplay, duration_format->minutes_display_string());
    create_option(vm.names.seconds, duration_format->seconds_style_string());
    create_option(vm.names.secondsDisplay, duration_format->seconds_display_string());
    create_option(vm.names.milliseconds, duration_format->milliseconds_style_string());
    create_option(vm.names.millisecondsDisplay, duration_format->milliseconds_display_string());
    create_option(vm.names.microseconds, duration_format->microseconds_style_string());
    create_option(vm.names.microsecondsDisplay, duration_format->microseconds_display_string());
    create_option(vm.names.nanoseconds, duration_format->nanoseconds_style_string());
    create_option(vm.names.nanosecondsDisplay, duration_format->nanoseconds_display_string());

    if (duration_format->has_fractional_digits())
        MUST(options->create_data_property_or_throw(vm.names.fractionalDigits, Value(duration_format->fractional_digits())));

    // 5. Return options.
    return options;
}

}
