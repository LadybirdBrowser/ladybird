/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS::Temporal {

enum class OptionType {
    Boolean,
    String,
    Number
};

struct GetOptionRequired { };
using OptionDefault = Variant<GetOptionRequired, Empty, bool, StringView, double>;

ThrowCompletionOr<Object*> get_options_object(VM&, Value options);
ThrowCompletionOr<Value> get_option(VM&, Object const& options, PropertyKey const& property, OptionType type, ReadonlySpan<StringView> values, OptionDefault const&);

template<size_t Size>
ThrowCompletionOr<Value> get_option(VM& vm, Object const& options, PropertyKey const& property, OptionType type, StringView const (&values)[Size], OptionDefault const& default_)
{
    return get_option(vm, options, property, type, ReadonlySpan<StringView> { values }, default_);
}

// 13.41 ToIntegerIfIntegral ( argument ), https://tc39.es/proposal-temporal/#sec-tointegerifintegral
template<typename... Args>
ThrowCompletionOr<double> to_integer_if_integral(VM& vm, Value argument, ErrorType error_type, Args... args)
{
    // 1. Let number be ? ToNumber(argument).
    auto number = TRY(argument.to_number(vm));

    // 2. If number is NaN, +0𝔽, or -0𝔽, return 0.
    if (number.is_nan() || number.is_positive_zero() || number.is_negative_zero())
        return 0;

    // 3. If IsIntegralNumber(number) is false, throw a RangeError exception.
    if (!number.is_integral_number())
        return vm.template throw_completion<RangeError>(error_type, args...);

    // 4. Return ℝ(number).
    return number.as_double();
}

}
