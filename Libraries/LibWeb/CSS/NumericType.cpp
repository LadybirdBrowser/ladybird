/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "NumericType.h"
#include <LibWeb/CSS/ValueType.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

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
