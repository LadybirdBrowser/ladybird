/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/CalcNodeRef.h>
#include <LibWeb/CSS/Units.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> create_calculated_style_value(CalcNodeRef root, NumericType const& resolved_type, CalculationContext const& context)
{
    Vector<StyleValueFFI::RetainedNumericRangeByType> ranges;
    ranges.ensure_capacity(context.accepted_ranges_by_type.size());
    for (auto const& [value_type, range] : context.accepted_ranges_by_type)
        ranges.unchecked_append({ to_underlying(value_type), range.min, range.max });
    auto resolve_as_base = context.percentages_resolve_as.has_value()
        ? NumericType::base_type_from_value_type(*context.percentages_resolve_as)
        : OptionalNone {};
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_calculated(
        root.release(),
        resolved_type.to_ffi(),
        context.percentages_resolve_as.has_value(),
        context.percentages_resolve_as == ValueType::Number,
        resolve_as_base.has_value() ? to_underlying(*resolve_as_base) : 0,
        context.percentages_resolve_as.has_value() ? to_underlying(*context.percentages_resolve_as) : 0,
        context.resolve_numbers_as_integers,
        ranges.data(), ranges.size()));
}

CalculationContext CalculationContext::for_property(PropertyNameAndID const& property)
{
    // FIXME: Handle registered custom properties, which may limit which types they accept.
    return {
        .percentages_resolve_as = property_resolves_percentages_relative_to(property.id()),
        .resolve_numbers_as_integers = property_accepts_type(property.id(), ValueType::Integer),
        .accepted_ranges_by_type = property_accepted_ranges_by_value_type(property.id()),
    };
}

// The C++ ValueType discriminants mirrored in the Rust range lookup; pin them.
static_assert(to_underlying(ValueType::Angle) == 2);
static_assert(to_underlying(ValueType::Flex) == 15);
static_assert(to_underlying(ValueType::Frequency) == 21);
static_assert(to_underlying(ValueType::Integer) == 24);
static_assert(to_underlying(ValueType::Length) == 25);
static_assert(to_underlying(ValueType::Number) == 27);
static_assert(to_underlying(ValueType::Percentage) == 31);
static_assert(to_underlying(ValueType::Resolution) == 35);
static_assert(to_underlying(ValueType::Time) == 38);

Optional<NumericLeaf> NumericLeaf::from_typed_om_unit(double value, Utf16FlyString const& unit)
{
    if (unit == "number"_utf16_fly_string)
        return NumericLeaf { Kind::Number, value };

    if (unit == "percent"_utf16_fly_string)
        return NumericLeaf { Kind::Percentage, value };

    if (auto dimension_type = dimension_for_unit(unit); dimension_type.has_value()) {
        switch (*dimension_type) {
        case DimensionType::Angle:
            return NumericLeaf { Kind::Angle, value, to_underlying(string_to_angle_unit(unit).release_value()) };
        case DimensionType::Flex:
            return NumericLeaf { Kind::Flex, value, to_underlying(string_to_flex_unit(unit).release_value()) };
        case DimensionType::Frequency:
            return NumericLeaf { Kind::Frequency, value, to_underlying(string_to_frequency_unit(unit).release_value()) };
        case DimensionType::Length:
            return NumericLeaf { Kind::Length, value, to_underlying(string_to_length_unit(unit).release_value()) };
        case DimensionType::Resolution:
            return NumericLeaf { Kind::Resolution, value, to_underlying(string_to_resolution_unit(unit).release_value()) };
        case DimensionType::Time:
            return NumericLeaf { Kind::Time, value, to_underlying(string_to_time_unit(unit).release_value()) };
        }
    }

    return {};
}

Utf16FlyString NumericLeaf::typed_om_unit() const
{
    switch (kind) {
    case Kind::Number:
        return "number"_utf16_fly_string;
    case Kind::Angle:
        return CSS::to_string(static_cast<AngleUnit>(unit_or_number_type));
    case Kind::Flex:
        return CSS::to_string(static_cast<FlexUnit>(unit_or_number_type));
    case Kind::Frequency:
        return CSS::to_string(static_cast<FrequencyUnit>(unit_or_number_type));
    case Kind::Length:
        return CSS::to_string(static_cast<LengthUnit>(unit_or_number_type));
    case Kind::Percentage:
        return "percent"_utf16_fly_string;
    case Kind::Resolution:
        return CSS::to_string(static_cast<ResolutionUnit>(unit_or_number_type));
    case Kind::Time:
        return CSS::to_string(static_cast<TimeUnit>(unit_or_number_type));
    }
    VERIFY_NOT_REACHED();
}

ValueType NumericLeaf::value_type() const
{
    switch (kind) {
    case Kind::Number:
        return ValueType::Number;
    case Kind::Angle:
        return ValueType::Angle;
    case Kind::Flex:
        return ValueType::Flex;
    case Kind::Frequency:
        return ValueType::Frequency;
    case Kind::Length:
        return ValueType::Length;
    case Kind::Percentage:
        return ValueType::Percentage;
    case Kind::Resolution:
        return ValueType::Resolution;
    case Kind::Time:
        return ValueType::Time;
    }
    VERIFY_NOT_REACHED();
}

NonnullRefPtr<StyleValue const> NumericLeaf::to_style_value() const
{
    auto data = [&] {
        switch (kind) {
        case Kind::Number:
            return StyleValueFFI::rust_style_value_create_number(value);
        case Kind::Angle:
            return StyleValueFFI::rust_style_value_create_angle(value, unit_or_number_type);
        case Kind::Flex:
            return StyleValueFFI::rust_style_value_create_flex(value, unit_or_number_type);
        case Kind::Frequency:
            return StyleValueFFI::rust_style_value_create_frequency(value, unit_or_number_type);
        case Kind::Length:
            return StyleValueFFI::rust_style_value_create_length(value, unit_or_number_type);
        case Kind::Percentage:
            return StyleValueFFI::rust_style_value_create_percentage(value);
        case Kind::Resolution:
            return StyleValueFFI::rust_style_value_create_resolution(value, unit_or_number_type);
        case Kind::Time:
            return StyleValueFFI::rust_style_value_create_time(value, unit_or_number_type);
        }
        VERIFY_NOT_REACHED();
    }();
    return StyleValue::adopt_rust_style_value_data(data);
}

CalcNodeRef CalcNodeRef::numeric(NumericLeaf const& leaf)
{
    return adopt(StyleValueFFI::rust_calc_node_create_numeric_dimension(to_underlying(leaf.kind), leaf.value, leaf.unit_or_number_type));
}

static CalcNodeRef make_variadic_node(u8 kind, Vector<CalcNodeRef> children)
{
    Vector<StyleValueFFI::CalcNode const*> handles;
    handles.ensure_capacity(children.size());
    for (auto& child : children)
        handles.unchecked_append(child.release());
    return CalcNodeRef::adopt(StyleValueFFI::rust_calc_node_create_variadic(kind, handles.data(), handles.size()));
}

CalcNodeRef CalcNodeRef::sum(Vector<CalcNodeRef> children) { return make_variadic_node(0, move(children)); }
CalcNodeRef CalcNodeRef::product(Vector<CalcNodeRef> children) { return make_variadic_node(1, move(children)); }
CalcNodeRef CalcNodeRef::min(Vector<CalcNodeRef> children) { return make_variadic_node(2, move(children)); }
CalcNodeRef CalcNodeRef::max(Vector<CalcNodeRef> children) { return make_variadic_node(3, move(children)); }

static CalcNodeRef make_unary_node(u8 kind, CalcNodeRef child)
{
    return CalcNodeRef::adopt(StyleValueFFI::rust_calc_node_create_unary(kind, child.release()));
}

CalcNodeRef CalcNodeRef::negate(CalcNodeRef child) { return make_unary_node(0, move(child)); }
CalcNodeRef CalcNodeRef::invert(CalcNodeRef child) { return make_unary_node(1, move(child)); }

CalcNodeRef CalcNodeRef::clamp(CalcNodeRef minimum, CalcNodeRef center, CalcNodeRef maximum)
{
    auto const* minimum_handle = minimum.release();
    auto const* center_handle = center.release();
    return adopt(StyleValueFFI::rust_calc_node_create_clamp(minimum_handle, center_handle, maximum.release()));
}

Optional<NumericType> CalcNodeRef::determine_type(CalculationContext const& context) const
{
    auto resolve_as_base = context.percentages_resolve_as.has_value()
        ? NumericType::base_type_from_value_type(*context.percentages_resolve_as)
        : OptionalNone {};
    return NumericType::from_ffi(StyleValueFFI::rust_calc_node_determine_type(
        m_node,
        context.percentages_resolve_as.has_value(),
        context.percentages_resolve_as == ValueType::Number,
        resolve_as_base.has_value() ? to_underlying(*resolve_as_base) : 0));
}

}
