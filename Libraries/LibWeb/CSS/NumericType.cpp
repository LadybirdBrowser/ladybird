/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "NumericType.h"
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Flex.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/RustStyleBridge.h>
#include <LibWeb/CSS/Time.h>
#include <LibWeb/CSS/ValueType.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#product-of-two-unit-maps
UnitMap product_of_two_unit_maps(UnitMap const& units1, UnitMap const& units2)
{
    // 1. Let result be a copy of units1.
    auto result = units1;

    // 2. For each unit → power in units2:
    for (auto const& [unit, power] : units2) {
        // 1. If result[unit] exists, increment result[unit] by power.
        // 2. Otherwise, set result[unit] to power.
        result.ensure(unit) += power;
    }

    // 3. Return result.
    return result;
}

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

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-create-a-type
Optional<NumericType> NumericType::create_from_unit(Utf16FlyString const& unit)
{
    // To create a type from a string unit, follow the appropriate branch of the following:

    // unit is "number"
    if (unit == "number"_utf16_fly_string) {
        // Return «[ ]» (empty map)
        return NumericType {};
    }

    // unit is "percent"
    if (unit == "percent"_utf16_fly_string) {
        // Return «[ "percent" → 1 ]»
        return NumericType { BaseType::Percent, 1 };
    }

    if (auto dimension = dimension_for_unit(unit); dimension.has_value()) {
        switch (*dimension) {
        // unit is a <length> unit
        case DimensionType::Length:
            // Return «[ "length" → 1 ]»
            return NumericType { BaseType::Length, 1 };

        // unit is an <angle> unit
        case DimensionType::Angle:
            // Return «[ "angle" → 1 ]»
            return NumericType { BaseType::Angle, 1 };

        // unit is a <time> unit
        case DimensionType::Time:
            // Return «[ "time" → 1 ]»
            return NumericType { BaseType::Time, 1 };

        // unit is a <frequency> unit
        case DimensionType::Frequency:
            // Return «[ "frequency" → 1 ]»
            return NumericType { BaseType::Frequency, 1 };

        // unit is a <resolution> unit
        case DimensionType::Resolution:
            // Return «[ "resolution" → 1 ]»
            return NumericType { BaseType::Resolution, 1 };

        // unit is a <flex> unit
        case DimensionType::Flex:
            // Return «[ "flex" → 1 ]»
            return NumericType { BaseType::Flex, 1 };
        }
    }

    // anything else
    //    Return failure.
    return {};

    // In all cases, the associated percent hint is null.
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-a-type-from-a-unit-map
Optional<NumericType> NumericType::create_from_unit_map(UnitMap const& unit_map)
{
    // To create a type from a unit map unit map:

    // 1. Let types be an initially empty list.
    Vector<NumericType> types;

    // 2. For each unit → power in unit map:
    for (auto const& [unit, power] : unit_map) {
        // 1. Let type be the result of creating a type from unit.
        auto type = create_from_unit(unit).release_value();

        // 2. Set type’s sole value to power.
        auto sole_type = [&type] {
            for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
                auto base_type = static_cast<BaseType>(i);
                if (type.exponent(base_type).has_value())
                    return base_type;
            }
            VERIFY_NOT_REACHED();
        }();
        type.set_exponent(sole_type, power);

        // 3. Append type to types.
        types.empend(type);
    }

    // 3. Return the result of multiplying all the items of types.
    if (types.is_empty())
        return {};
    auto result = types.first();
    bool first = true;
    for (auto const& type : types) {
        if (first) {
            first = false;
            continue;
        }
        if (auto multiplied_type = result.multiplied_by(type); multiplied_type.has_value()) {
            result = multiplied_type.release_value();
        } else {
            return {};
        }
    }
    return result;
}

static StyleValueFFI::FfiNumericType to_ffi_numeric_type(NumericType const& type)
{
    StyleValueFFI::FfiNumericType result {};
    result.valid = true;
    type.for_each_type_and_exponent([&](auto base_type, i32 exponent) {
        result.has_exponent[to_underlying(base_type)] = true;
        result.exponents[to_underlying(base_type)] = exponent;
    });
    if (auto hint = type.percent_hint(); hint.has_value()) {
        result.has_percent_hint = true;
        result.percent_hint = to_underlying(*hint);
    }
    return result;
}

static Optional<NumericType> from_ffi_numeric_type(StyleValueFFI::FfiNumericType const& type)
{
    if (!type.valid)
        return {};
    NumericType result;
    for (auto i = 0; i < to_underlying(NumericType::BaseType::__Count); ++i) {
        if (type.has_exponent[i])
            result.set_exponent(static_cast<NumericType::BaseType>(i), type.exponents[i]);
    }
    if (type.has_percent_hint)
        result.set_percent_hint(static_cast<NumericType::BaseType>(type.percent_hint));
    return result;
}

static Optional<NumericType> operate(StyleValueFFI::FfiNumericTypeOperation operation, NumericType const& first, NumericType const* second = nullptr)
{
    auto ffi_first = to_ffi_numeric_type(first);
    auto ffi_second = second ? to_ffi_numeric_type(*second) : StyleValueFFI::FfiNumericType {};
    return from_ffi_numeric_type(invoke_rust_numeric_type_operate(operation, &ffi_first, second ? &ffi_second : nullptr));
}

Optional<NumericType> NumericType::added_to(NumericType const& other) const
{
    return operate(StyleValueFFI::FfiNumericTypeOperation::Add, *this, &other);
}

Optional<NumericType> NumericType::multiplied_by(NumericType const& other) const
{
    return operate(StyleValueFFI::FfiNumericTypeOperation::Multiply, *this, &other);
}

NumericType NumericType::inverted() const
{
    return operate(StyleValueFFI::FfiNumericTypeOperation::Invert, *this).release_value();
}

static bool matches(StyleValueFFI::FfiNumericTypeMatch match_kind, NumericType const& type, NumericType::BaseType base_type, Optional<ValueType> percentages_resolve_as)
{
    auto ffi_type = to_ffi_numeric_type(type);
    return invoke_rust_numeric_type_matches(
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
