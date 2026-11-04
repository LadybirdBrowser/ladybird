/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "NumericType.h"
#include <LibWeb/CSS/ValueType.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

Optional<NumericType::BaseType> NumericType::base_type_from_value_type(ValueType value_type)
{
    switch (value_type) {
    case ValueType::Angle:
        return BaseType::Angle;
    case ValueType::Flex:
        return BaseType::Flex;
    case ValueType::Frequency:
        return BaseType::Frequency;
    case ValueType::Length:
        return BaseType::Length;
    case ValueType::Percentage:
        return BaseType::Percent;
    case ValueType::Resolution:
        return BaseType::Resolution;
    case ValueType::Time:
        return BaseType::Time;

    default:
        return {};
    }
}

static StyleValueFFI::FfiNumericType to_ffi_numeric_type(NumericType const& type)
{
    StyleValueFFI::FfiNumericType result {};
    result.valid = true;
    type.for_each_type_and_exponent([&](auto base_type, i32 exponent) {
        result.has_exponent_bits |= 1u << to_underlying(base_type);
        result.exponents[to_underlying(base_type)] = exponent;
    });
    if (auto hint = type.percent_hint(); hint.has_value()) {
        result.has_percent_hint = true;
        result.percent_hint = to_underlying(*hint);
    }
    return result;
}

Optional<NumericType> NumericType::from_ffi(StyleValueFFI::FfiNumericType const& type)
{
    if (!type.valid)
        return {};
    NumericType result;
    for (auto i = 0; i < to_underlying(NumericType::BaseType::__Count); ++i) {
        if (type.has_exponent_bits & (1u << i))
            result.set_exponent(static_cast<NumericType::BaseType>(i), type.exponents[i]);
    }
    if (type.has_percent_hint)
        result.set_percent_hint(static_cast<NumericType::BaseType>(type.percent_hint));
    return result;
}

static bool matches(StyleValueFFI::FfiNumericTypeMatch match_kind, NumericType const& type, NumericType::BaseType base_type, Optional<ValueType> percentages_resolve_as)
{
    auto ffi_type = to_ffi_numeric_type(type);
    return StyleValueFFI::rust_numeric_type_matches(
        match_kind,
        &ffi_type,
        to_underlying(base_type),
        percentages_resolve_as.has_value(),
        percentages_resolve_as.has_value() ? to_underlying(*percentages_resolve_as) : 0);
}

bool NumericType::matches_dimension(BaseType type, Optional<ValueType> percentages_resolve_as) const
{
    return matches(StyleValueFFI::FfiNumericTypeMatch::Dimension, *this, type, percentages_resolve_as);
}

bool NumericType::matches_percentage() const
{
    return matches(StyleValueFFI::FfiNumericTypeMatch::Percentage, *this, BaseType::Percent, {});
}

bool NumericType::matches_dimension_percentage(BaseType type, Optional<ValueType> percentages_resolve_as) const
{
    return matches(StyleValueFFI::FfiNumericTypeMatch::DimensionPercentage, *this, type, percentages_resolve_as);
}

bool NumericType::matches_number(Optional<ValueType> percentages_resolve_as) const
{
    return matches(StyleValueFFI::FfiNumericTypeMatch::Number, *this, BaseType::Percent, percentages_resolve_as);
}

}
