/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <math.h>

namespace JS::Temporal {

// https://tc39.es/proposal-temporal/#sec-temporal-units
enum class Unit {
    Year,
    Month,
    Week,
    Day,
    Hour,
    Minute,
    Second,
    Millisecond,
    Microsecond,
    Nanosecond,
};
StringView temporal_unit_to_string(Unit);

// https://tc39.es/proposal-temporal/#sec-temporal-units
enum class UnitCategory {
    Date,
    Time,
};

// https://tc39.es/proposal-temporal/#sec-temporal-units
enum class UnitGroup {
    Date,
    Time,
    DateTime,
};

struct Unset { };
using RoundingIncrement = Variant<Unset, u64>;

struct RelativeTo {
    // FIXME: Make these objects represent their actual types when we re-implement them.
    GC::Ptr<JS::Object> plain_relative_to; // [[PlainRelativeTo]]
    GC::Ptr<JS::Object> zoned_relative_to; // [[ZonedRelativeTo]]
};

ThrowCompletionOr<RelativeTo> get_temporal_relative_to_option(VM&, Object const& options);
bool is_calendar_unit(Unit);
UnitCategory temporal_unit_category(Unit);
ThrowCompletionOr<GC::Ref<Duration>> parse_temporal_duration_string(VM&, StringView iso_string);

// 13.38 ToIntegerWithTruncation ( argument ), https://tc39.es/proposal-temporal/#sec-tointegerwithtruncation
template<typename... Args>
ThrowCompletionOr<double> to_integer_with_truncation(VM& vm, Value argument, ErrorType error_type, Args&&... args)
{
    // 1. Let number be ? ToNumber(argument).
    auto number = TRY(argument.to_number(vm));

    // 2. If number is NaN, +∞𝔽 or -∞𝔽, throw a RangeError exception.
    if (number.is_nan() || number.is_infinity())
        return vm.throw_completion<RangeError>(error_type, forward<Args>(args)...);

    // 3. Return truncate(ℝ(number)).
    return trunc(number.as_double());
}

// 13.38 ToIntegerWithTruncation ( argument ), https://tc39.es/proposal-temporal/#sec-tointegerwithtruncation
// AD-HOC: We often need to use this AO when we have a parsed StringView. This overload allows callers to avoid creating
//         a PrimitiveString for the primary definition.
template<typename... Args>
ThrowCompletionOr<double> to_integer_with_truncation(VM& vm, StringView argument, ErrorType error_type, Args&&... args)
{
    // 1. Let number be ? ToNumber(argument).
    auto number = string_to_number(argument);

    // 2. If number is NaN, +∞𝔽 or -∞𝔽, throw a RangeError exception.
    if (isnan(number) || isinf(number))
        return vm.throw_completion<RangeError>(error_type, forward<Args>(args)...);

    // 3. Return truncate(ℝ(number)).
    return trunc(number);
}

// 13.39 ToIntegerIfIntegral ( argument ), https://tc39.es/proposal-temporal/#sec-tointegerifintegral
template<typename... Args>
ThrowCompletionOr<double> to_integer_if_integral(VM& vm, Value argument, ErrorType error_type, Args&&... args)
{
    // 1. Let number be ? ToNumber(argument).
    auto number = TRY(argument.to_number(vm));

    // 2. If number is not an integral Number, throw a RangeError exception.
    if (!number.is_integral_number())
        return vm.throw_completion<RangeError>(error_type, forward<Args>(args)...);

    // 3. Return ℝ(number).
    return number.as_double();
}

enum class OptionType {
    Boolean,
    String,
};

struct DefaultRequired { };
using OptionDefault = Variant<DefaultRequired, Empty, bool, StringView, double>;

ThrowCompletionOr<GC::Ref<Object>> get_options_object(VM&, Value options);
ThrowCompletionOr<Value> get_option(VM&, Object const& options, PropertyKey const& property, OptionType type, ReadonlySpan<StringView> values, OptionDefault const&);

template<size_t Size>
ThrowCompletionOr<Value> get_option(VM& vm, Object const& options, PropertyKey const& property, OptionType type, StringView const (&values)[Size], OptionDefault const& default_)
{
    return get_option(vm, options, property, type, ReadonlySpan<StringView> { values }, default_);
}

}
