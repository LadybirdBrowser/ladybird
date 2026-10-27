/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CalculatedStyleValue.h"
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/CSS/CSSMathClamp.h>
#include <LibWeb/CSS/CSSMathInvert.h>
#include <LibWeb/CSS/CSSMathMax.h>
#include <LibWeb/CSS/CSSMathMin.h>
#include <LibWeb/CSS/CSSMathNegate.h>
#include <LibWeb/CSS/CSSMathProduct.h>
#include <LibWeb/CSS/CSSMathSum.h>
#include <LibWeb/CSS/CSSNumericArray.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalcNodeRef.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>

namespace Web::CSS {

// Builds the Rust mirror of a calculation tree, transferring ownership of the
// returned handle to the caller. The child orders follow each node's members.
StyleValueFFI::StyleValueData const* CalculatedStyleValue::make_calculated_data_from_rust_root(StyleValueFFI::CalcNode const* rust_root, NumericType const& resolved_type, CalculationContext const& context)
{
    Vector<StyleValueFFI::RetainedNumericRangeByType> ranges;
    ranges.ensure_capacity(context.accepted_ranges_by_type.size());
    for (auto const& [value_type, range] : context.accepted_ranges_by_type)
        ranges.unchecked_append({ to_underlying(value_type), range.min, range.max });
    auto resolve_as_base = context.percentages_resolve_as.has_value()
        ? NumericType::base_type_from_value_type(*context.percentages_resolve_as)
        : OptionalNone {};
    return StyleValueFFI::rust_style_value_create_calculated(
        rust_root,
        resolved_type.to_ffi(),
        context.percentages_resolve_as.has_value(),
        context.percentages_resolve_as == ValueType::Number,
        resolve_as_base.has_value() ? to_underlying(*resolve_as_base) : 0,
        context.percentages_resolve_as.has_value() ? to_underlying(*context.percentages_resolve_as) : 0,
        context.resolve_numbers_as_integers,
        ranges.data(), ranges.size());
}

ValueComparingNonnullRefPtr<CalculatedStyleValue const> CalculatedStyleValue::create(CalcNodeRef root, NumericType resolved_type, CalculationContext context)
{
    return adopt_ref(*new (nothrow) CalculatedStyleValue(root.release(), move(resolved_type), move(context)));
}

NumericType CalculatedStyleValue::resolved_type() const
{
    return NumericType::from_ffi(m_value->calculated.resolved_type).release_value();
}

CalculationContext CalculatedStyleValue::calculation_context() const
{
    auto const& data = m_value->calculated;
    CalculationContext context;
    if (data.has_percentages_resolve_as)
        context.percentages_resolve_as = static_cast<ValueType>(data.percentages_resolve_as);
    context.resolve_numbers_as_integers = data.resolve_numbers_as_integers;
    for (size_t i = 0; i < data.accepted_ranges.length; ++i) {
        auto const& range = data.accepted_ranges.pointer[i];
        context.accepted_ranges_by_type.set(static_cast<ValueType>(range.value_type), NumericRange { range.min, range.max });
    }
    return context;
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

bool CalculatedStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;

    // NB: Structural equality runs entirely over the Rust value graph, including the style
    //     values retained by random() and non-math-function nodes.
    return StyleValueFFI::rust_calc_equals(m_value.operator->(), other.as_calculated().m_value.operator->());
}

bool CalculatedStyleValue::contains_percentage() const
{
    // The Rust mirror answers.
    return StyleValueFFI::rust_calc_node_contains_percentage(m_value->calculated.rust_calculation.node);
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-math-expression
static GC::Ptr<CSSNumericValue> reify_rust_calculation(StyleValueFFI::StyleValueData const* calculated_data)
{
    struct Reification {
        StyleValueFFI::FfiCalcReification description;
        ~Reification() { StyleValueFFI::rust_calc_reification_release(description.storage); }
    } reification { StyleValueFFI::rust_calc_describe_for_typed_om(calculated_data) };
    if (reification.description.node_count == 0)
        return nullptr;

    auto nodes = ReadonlySpan<StyleValueFFI::FfiCalcReificationNode> { reification.description.nodes, reification.description.node_count };
    auto child_indices = ReadonlySpan<size_t> { reification.description.children, reification.description.child_count };
    GC::RootVector<GC::Ref<CSSNumericValue>> reified_nodes;
    reified_nodes.ensure_capacity(nodes.size());
    for (auto const& node : nodes) {
        auto numeric_type = NumericType::from_ffi(node.numeric_type).release_value();
        VERIFY(node.child_start + node.child_count <= child_indices.size());
        auto children = child_indices.slice(node.child_start, node.child_count);
        auto child = [&](size_t index) -> GC::Ref<CSSNumericValue> {
            VERIFY(index < children.size());
            VERIFY(children[index] < reified_nodes.size());
            return reified_nodes[children[index]];
        };

        // NB: The reified objects get the types that their constructors would give them, computed from their
        //     reified children. The internal calculation's type may differ, for example by carrying a percent hint
        //     from the property that it belongs to.
        auto combined_type_of_children = [&](auto combine) {
            Optional<NumericType> type;
            for (auto index : children) {
                VERIFY(index < reified_nodes.size());
                auto const& child_type = reified_nodes[index]->type();
                type = type.has_value() ? combine(*type, child_type) : child_type;
                if (!type.has_value())
                    return numeric_type;
            }
            return type.value_or(numeric_type);
        };
        auto added_types_of_children = [&]() {
            return combined_type_of_children([](NumericType const& a, NumericType const& b) { return a.added_to(b); });
        };
        auto multiplied_types_of_children = [&]() {
            return combined_type_of_children([](NumericType const& a, NumericType const& b) { return a.multiplied_by(b); });
        };
        auto reify_children = [&]() {
            GC::RootVector<GC::Ref<CSSNumericValue>> result;
            result.ensure_capacity(children.size());
            for (auto index : children) {
                VERIFY(index < reified_nodes.size());
                result.append(reified_nodes[index]);
            }
            return CSSNumericArray::create(move(result));
        };

        switch (node.kind) {
        case 0:
            reified_nodes.append(CSSUnitValue::create(node.value, NumericLeaf { static_cast<NumericLeaf::Kind>(node.numeric_kind), node.value, node.unit }.typed_om_unit()));
            break;
        case 2:
            reified_nodes.append(CSSMathSum::create(added_types_of_children(), reify_children()));
            break;
        case 3:
            reified_nodes.append(CSSMathProduct::create(multiplied_types_of_children(), reify_children()));
            break;
        case 4:
            VERIFY(children.size() == 1);
            reified_nodes.append(CSSMathNegate::create(child(0)->type(), child(0)));
            break;
        case 5:
            VERIFY(children.size() == 1);
            reified_nodes.append(CSSMathInvert::create(child(0)->type().inverted(), child(0)));
            break;
        case 6:
            reified_nodes.append(CSSMathMin::create(added_types_of_children(), reify_children()));
            break;
        case 7:
            reified_nodes.append(CSSMathMax::create(added_types_of_children(), reify_children()));
            break;
        case 8:
            VERIFY(children.size() == 3);
            reified_nodes.append(CSSMathClamp::create(added_types_of_children(), child(0), child(1), child(2)));
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
    return reified_nodes.last();
}

GC::Ref<CSSStyleValue> CalculatedStyleValue::reify(Utf16FlyString const& associated_property) const
{
    // NB: This spec algorithm is incomplete and assumes we do not already have a calculation tree.
    //     Rust describes the existing tree in one batch instead.
    if (auto reified = reify_rust_calculation(m_value.operator->()))
        return *reified;
    // Some math functions are not reifiable yet. If we contain one, we have to fall back to CSSStyleValue.
    // https://github.com/w3c/css-houdini-drafts/issues/1090
    return default_reify(associated_property);
}

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
