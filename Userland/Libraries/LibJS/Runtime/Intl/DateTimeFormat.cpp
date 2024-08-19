/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Intl/DateTimeFormat.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <math.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(DateTimeFormat);

// 11 DateTimeFormat Objects, https://tc39.es/ecma402/#datetimeformat-objects
DateTimeFormat::DateTimeFormat(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

void DateTimeFormat::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_bound_format);
}

// 11.5.5 FormatDateTimePattern ( dateTimeFormat, patternParts, x, rangeFormatOptions ), https://tc39.es/ecma402/#sec-formatdatetimepattern
ThrowCompletionOr<Vector<Unicode::DateTimeFormat::Partition>> format_date_time_pattern(VM& vm, DateTimeFormat& date_time_format, double time)
{
    // 1. Let x be TimeClip(x).
    time = time_clip(time);

    // 2. If x is NaN, throw a RangeError exception.
    if (isnan(time))
        return vm.throw_completion<RangeError>(ErrorType::IntlInvalidTime);

    return date_time_format.formatter().format_to_parts(time);
}

// 11.5.6 PartitionDateTimePattern ( dateTimeFormat, x ), https://tc39.es/ecma402/#sec-partitiondatetimepattern
ThrowCompletionOr<Vector<Unicode::DateTimeFormat::Partition>> partition_date_time_pattern(VM& vm, DateTimeFormat& date_time_format, double time)
{
    // 1. Let patternParts be PartitionPattern(dateTimeFormat.[[Pattern]]).
    // 2. Let result be ? FormatDateTimePattern(dateTimeFormat, patternParts, x, undefined).
    return format_date_time_pattern(vm, date_time_format, time);
}

// 11.5.7 FormatDateTime ( dateTimeFormat, x ), https://tc39.es/ecma402/#sec-formatdatetime
ThrowCompletionOr<String> format_date_time(VM& vm, DateTimeFormat& date_time_format, double time)
{
    // 1. Let parts be ? PartitionDateTimePattern(dateTimeFormat, x).
    {
        // NOTE: We short-circuit PartitionDateTimePattern as we do not need individual partitions. But we must still
        //       perform the time clip and NaN sanity checks from its call to FormatDateTimePattern.

        // 1. Let x be TimeClip(x).
        time = time_clip(time);

        // 2. If x is NaN, throw a RangeError exception.
        if (isnan(time))
            return vm.throw_completion<RangeError>(ErrorType::IntlInvalidTime);
    }

    // 2. Let result be the empty String.
    // 3. For each Record { [[Type]], [[Value]] } part in parts, do
    //     a. Set result to the string-concatenation of result and part.[[Value]].
    // 4. Return result.
    return date_time_format.formatter().format(time);
}

// 11.5.8 FormatDateTimeToParts ( dateTimeFormat, x ), https://tc39.es/ecma402/#sec-formatdatetimetoparts
ThrowCompletionOr<GC::Ref<Array>> format_date_time_to_parts(VM& vm, DateTimeFormat& date_time_format, double time)
{
    auto& realm = *vm.current_realm();

    // 1. Let parts be ? PartitionDateTimePattern(dateTimeFormat, x).
    auto parts = TRY(partition_date_time_pattern(vm, date_time_format, time));

    // 2. Let result be ! ArrayCreate(0).
    auto result = MUST(Array::create(realm, 0));

    // 3. Let n be 0.
    size_t n = 0;

    // 4. For each Record { [[Type]], [[Value]] } part in parts, do
    for (auto& part : parts) {
        // a. Let O be OrdinaryObjectCreate(%Object.prototype%).
        auto object = Object::create(realm, realm.intrinsics().object_prototype());

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        MUST(object->create_data_property_or_throw(vm.names.type, PrimitiveString::create(vm, part.type)));

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        MUST(object->create_data_property_or_throw(vm.names.value, PrimitiveString::create(vm, move(part.value))));

        // d. Perform ! CreateDataProperty(result, ! ToString(n), O).
        MUST(result->create_data_property_or_throw(n, object));

        // e. Increment n by 1.
        ++n;
    }

    // 5. Return result.
    return result;
}

// 11.5.9 PartitionDateTimeRangePattern ( dateTimeFormat, x, y ), https://tc39.es/ecma402/#sec-partitiondatetimerangepattern
ThrowCompletionOr<Vector<Unicode::DateTimeFormat::Partition>> partition_date_time_range_pattern(VM& vm, DateTimeFormat& date_time_format, double start, double end)
{
    // 1. Let x be TimeClip(x).
    start = time_clip(start);

    // 2. If x is NaN, throw a RangeError exception.
    if (isnan(start))
        return vm.throw_completion<RangeError>(ErrorType::IntlInvalidTime);

    // 3. Let y be TimeClip(y).
    end = time_clip(end);

    // 4. If y is NaN, throw a RangeError exception.
    if (isnan(end))
        return vm.throw_completion<RangeError>(ErrorType::IntlInvalidTime);

    return date_time_format.formatter().format_range_to_parts(start, end);
}

// 11.5.10 FormatDateTimeRange ( dateTimeFormat, x, y ), https://tc39.es/ecma402/#sec-formatdatetimerange
ThrowCompletionOr<String> format_date_time_range(VM& vm, DateTimeFormat& date_time_format, double start, double end)
{
    {
        // NOTE: We short-circuit PartitionDateTimeRangePattern as we do not need individual partitions. But we must
        //       still perform the time clip and NaN sanity checks from its its first steps.

        // 1. Let x be TimeClip(x).
        start = time_clip(start);

        // 2. If x is NaN, throw a RangeError exception.
        if (isnan(start))
            return vm.throw_completion<RangeError>(ErrorType::IntlInvalidTime);

        // 3. Let y be TimeClip(y).
        end = time_clip(end);

        // 4. If y is NaN, throw a RangeError exception.
        if (isnan(end))
            return vm.throw_completion<RangeError>(ErrorType::IntlInvalidTime);
    }

    // 1. Let parts be ? PartitionDateTimeRangePattern(dateTimeFormat, x, y).
    // 2. Let result be the empty String.
    // 3. For each Record { [[Type]], [[Value]], [[Source]] } part in parts, do
    //     a. Set result to the string-concatenation of result and part.[[Value]].
    // 4. Return result.
    return date_time_format.formatter().format_range(start, end);
}

// 11.5.11 FormatDateTimeRangeToParts ( dateTimeFormat, x, y ), https://tc39.es/ecma402/#sec-formatdatetimerangetoparts
ThrowCompletionOr<GC::Ref<Array>> format_date_time_range_to_parts(VM& vm, DateTimeFormat& date_time_format, double start, double end)
{
    auto& realm = *vm.current_realm();

    // 1. Let parts be ? PartitionDateTimeRangePattern(dateTimeFormat, x, y).
    auto parts = TRY(partition_date_time_range_pattern(vm, date_time_format, start, end));

    // 2. Let result be ! ArrayCreate(0).
    auto result = MUST(Array::create(realm, 0));

    // 3. Let n be 0.
    size_t n = 0;

    // 4. For each Record { [[Type]], [[Value]], [[Source]] } part in parts, do
    for (auto& part : parts) {
        // a. Let O be OrdinaryObjectCreate(%ObjectPrototype%).
        auto object = Object::create(realm, realm.intrinsics().object_prototype());

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        MUST(object->create_data_property_or_throw(vm.names.type, PrimitiveString::create(vm, part.type)));

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        MUST(object->create_data_property_or_throw(vm.names.value, PrimitiveString::create(vm, move(part.value))));

        // d. Perform ! CreateDataPropertyOrThrow(O, "source", part.[[Source]]).
        MUST(object->create_data_property_or_throw(vm.names.source, PrimitiveString::create(vm, part.source)));

        // e. Perform ! CreateDataProperty(result, ! ToString(n), O).
        MUST(result->create_data_property_or_throw(n, object));

        // f. Increment n by 1.
        ++n;
    }

    // 5. Return result.
    return result;
}

}
