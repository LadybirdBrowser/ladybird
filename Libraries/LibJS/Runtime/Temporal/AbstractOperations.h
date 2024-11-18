/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <math.h>

namespace JS::Temporal {

enum class ArithmeticOperation {
    Add,
    Subtract,
};

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

// https://tc39.es/proposal-temporal/#table-unsigned-rounding-modes
enum class RoundingMode {
    Ceil,
    Floor,
    Expand,
    Trunc,
    HalfCeil,
    HalfFloor,
    HalfExpand,
    HalfTrunc,
    HalfEven,
};

// https://tc39.es/proposal-temporal/#table-unsigned-rounding-modes
enum class UnsignedRoundingMode {
    HalfEven,
    HalfInfinity,
    HalfZero,
    Infinity,
    Zero,
};

// https://tc39.es/proposal-temporal/#table-unsigned-rounding-modes
enum class Sign {
    Negative,
    Positive,
};

struct Auto { };
struct Required { };
struct Unset { };
using Precision = Variant<Auto, u8>;
using RoundingIncrement = Variant<Unset, u64>;
using UnitDefault = Variant<Required, Unset, Auto, Unit>;
using UnitValue = Variant<Unset, Auto, Unit>;

struct SecondsStringPrecision {
    struct Minute { };

    Variant<Minute, Auto, u8> precision;
    Unit unit;
    u8 increment { 0 };
};

struct RelativeTo {
    // FIXME: Make these objects represent their actual types when we re-implement them.
    GC::Ptr<JS::Object> plain_relative_to; // [[PlainRelativeTo]]
    GC::Ptr<JS::Object> zoned_relative_to; // [[ZonedRelativeTo]]
};

ThrowCompletionOr<void> validate_temporal_rounding_increment(VM&, u64 increment, u64 dividend, bool inclusive);
ThrowCompletionOr<Precision> get_temporal_fractional_second_digits_option(VM&, Object const& options);
SecondsStringPrecision to_seconds_string_precision_record(UnitValue, Precision);
ThrowCompletionOr<UnitValue> get_temporal_unit_valued_option(VM&, Object const& options, PropertyKey const&, UnitGroup, UnitDefault const&, ReadonlySpan<UnitValue> extra_values = {});
ThrowCompletionOr<RelativeTo> get_temporal_relative_to_option(VM&, Object const& options);
Unit larger_of_two_temporal_units(Unit, Unit);
bool is_calendar_unit(Unit);
UnitCategory temporal_unit_category(Unit);
RoundingIncrement maximum_temporal_duration_rounding_increment(Unit);
Crypto::UnsignedBigInteger const& temporal_unit_length_in_nanoseconds(Unit);
String format_fractional_seconds(u64, Precision);
UnsignedRoundingMode get_unsigned_rounding_mode(RoundingMode, Sign);
double apply_unsigned_rounding_mode(double, double r1, double r2, UnsignedRoundingMode);
Crypto::SignedBigInteger apply_unsigned_rounding_mode(Crypto::SignedDivisionResult const&, Crypto::SignedBigInteger const& r1, Crypto::SignedBigInteger const& r2, UnsignedRoundingMode, Crypto::UnsignedBigInteger const& increment);
double round_number_to_increment(double, u64 increment, RoundingMode);
Crypto::SignedBigInteger round_number_to_increment(Crypto::SignedBigInteger const&, Crypto::UnsignedBigInteger const& increment, RoundingMode);
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

using OptionDefault = Variant<Required, Empty, bool, StringView, double>;

ThrowCompletionOr<GC::Ref<Object>> get_options_object(VM&, Value options);
ThrowCompletionOr<Value> get_option(VM&, Object const& options, PropertyKey const& property, OptionType type, ReadonlySpan<StringView> values, OptionDefault const&);

template<size_t Size>
ThrowCompletionOr<Value> get_option(VM& vm, Object const& options, PropertyKey const& property, OptionType type, StringView const (&values)[Size], OptionDefault const& default_)
{
    return get_option(vm, options, property, type, ReadonlySpan<StringView> { values }, default_);
}

ThrowCompletionOr<RoundingMode> get_rounding_mode_option(VM&, Object const& options, RoundingMode fallback);
ThrowCompletionOr<u64> get_rounding_increment_option(VM&, Object const& options);

}
