/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/StringBuilder.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/NumberFormat.h>
#include <LibJS/Runtime/Intl/NumberFormatConstructor.h>
#include <LibJS/Runtime/Intl/PluralRules.h>
#include <LibJS/Runtime/Intl/RelativeTimeFormat.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(RelativeTimeFormat);

// 18 RelativeTimeFormat Objects, https://tc39.es/ecma402/#relativetimeformat-objects
RelativeTimeFormat::RelativeTimeFormat(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

// 18.5.1 SingularRelativeTimeUnit ( unit ), https://tc39.es/ecma402/#sec-singularrelativetimeunit
ThrowCompletionOr<Unicode::TimeUnit> singular_relative_time_unit(VM& vm, StringView unit)
{
    // 1. If unit is "seconds", return "second".
    if (unit == "seconds"sv)
        return Unicode::TimeUnit::Second;
    // 2. If unit is "minutes", return "minute".
    if (unit == "minutes"sv)
        return Unicode::TimeUnit::Minute;
    // 3. If unit is "hours", return "hour".
    if (unit == "hours"sv)
        return Unicode::TimeUnit::Hour;
    // 4. If unit is "days", return "day".
    if (unit == "days"sv)
        return Unicode::TimeUnit::Day;
    // 5. If unit is "weeks", return "week".
    if (unit == "weeks"sv)
        return Unicode::TimeUnit::Week;
    // 6. If unit is "months", return "month".
    if (unit == "months"sv)
        return Unicode::TimeUnit::Month;
    // 7. If unit is "quarters", return "quarter".
    if (unit == "quarters"sv)
        return Unicode::TimeUnit::Quarter;
    // 8. If unit is "years", return "year".
    if (unit == "years"sv)
        return Unicode::TimeUnit::Year;

    // 9. If unit is not one of "second", "minute", "hour", "day", "week", "month", "quarter", or "year", throw a RangeError exception.
    // 10. Return unit.
    if (auto time_unit = Unicode::time_unit_from_string(unit); time_unit.has_value())
        return *time_unit;
    return vm.throw_completion<RangeError>(ErrorType::IntlInvalidUnit, unit);
}

// 18.5.2 PartitionRelativeTimePattern ( relativeTimeFormat, value, unit ), https://tc39.es/ecma402/#sec-PartitionRelativeTimePattern
ThrowCompletionOr<Vector<Unicode::RelativeTimeFormat::Partition>> partition_relative_time_pattern(VM& vm, RelativeTimeFormat& relative_time_format, double value, StringView unit)
{
    // 1. If value is NaN, +∞𝔽, or -∞𝔽, throw a RangeError exception.
    if (!Value(value).is_finite_number())
        return vm.throw_completion<RangeError>(ErrorType::NumberIsNaNOrInfinity);

    // 2. Let unit be ? SingularRelativeTimeUnit(unit).
    auto time_unit = TRY(singular_relative_time_unit(vm, unit));

    return relative_time_format.formatter().format_to_parts(value, time_unit, relative_time_format.numeric());
}

// 18.5.4 FormatRelativeTime ( relativeTimeFormat, value, unit ), https://tc39.es/ecma402/#sec-FormatRelativeTime
ThrowCompletionOr<String> format_relative_time(VM& vm, RelativeTimeFormat& relative_time_format, double value, StringView unit)
{
    // 1. Let parts be ? PartitionRelativeTimePattern(relativeTimeFormat, value, unit).
    auto time_unit = TRY([&]() -> ThrowCompletionOr<Unicode::TimeUnit> {
        // NOTE: We short-circuit PartitionRelativeTimePattern as we do not need individual partitions. But we must still
        //       perform the NaN/Infinity sanity checks and unit parsing from its first steps.

        // 1. If value is NaN, +∞𝔽, or -∞𝔽, throw a RangeError exception.
        if (!Value(value).is_finite_number())
            return vm.throw_completion<RangeError>(ErrorType::NumberIsNaNOrInfinity);

        // 2. Let unit be ? SingularRelativeTimeUnit(unit).
        return TRY(singular_relative_time_unit(vm, unit));
    }());

    // 2. Let result be the empty String.
    // 3. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
    //     a. Set result to the string-concatenation of result and part.[[Value]].
    // 4. Return result.
    return relative_time_format.formatter().format(value, time_unit, relative_time_format.numeric());
}

// 18.5.5 FormatRelativeTimeToParts ( relativeTimeFormat, value, unit ), https://tc39.es/ecma402/#sec-FormatRelativeTimeToParts
ThrowCompletionOr<GC::Ref<Array>> format_relative_time_to_parts(VM& vm, RelativeTimeFormat& relative_time_format, double value, StringView unit)
{
    auto& realm = *vm.current_realm();

    // 1. Let parts be ? PartitionRelativeTimePattern(relativeTimeFormat, value, unit).
    auto parts = TRY(partition_relative_time_pattern(vm, relative_time_format, value, unit));

    // 2. Let result be ! ArrayCreate(0).
    auto result = MUST(Array::create(realm, 0));

    // 3. Let n be 0.
    // 4. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
    for (auto [n, part] : enumerate(parts)) {
        // a. Let O be OrdinaryObjectCreate(%Object.prototype%).
        auto object = Object::create(realm, realm.intrinsics().object_prototype());

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        MUST(object->create_data_property_or_throw(vm.names.type, PrimitiveString::create(vm, part.type)));

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        MUST(object->create_data_property_or_throw(vm.names.value, PrimitiveString::create(vm, move(part.value))));

        // d. If part.[[Unit]] is not empty, then
        if (!part.unit.is_empty()) {
            // i. Perform ! CreateDataPropertyOrThrow(O, "unit", part.[[Unit]]).
            MUST(object->create_data_property_or_throw(vm.names.unit, PrimitiveString::create(vm, part.unit)));
        }

        // e. Perform ! CreateDataPropertyOrThrow(result, ! ToString(n), O).
        MUST(result->create_data_property_or_throw(n, object));

        // f. Increment n by 1.
    }

    // 5. Return result.
    return result;
}

}
