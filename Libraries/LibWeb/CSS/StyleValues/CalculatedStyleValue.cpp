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
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
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

// Marshals a numeric type into its FFI mirror.
static StyleValueFFI::FfiNumericType to_ffi_numeric_type(Optional<NumericType> const& type)
{
    StyleValueFFI::FfiNumericType result {};
    result.valid = type.has_value();
    if (type.has_value()) {
        type->for_each_type_and_exponent([&](auto base_type, i32 exponent) {
            result.has_exponent_bits |= 1u << to_underlying(base_type);
            result.exponents[to_underlying(base_type)] = exponent;
        });
        if (auto hint = type->percent_hint(); hint.has_value()) {
            result.has_percent_hint = true;
            result.percent_hint = to_underlying(hint.value());
        }
    }
    return result;
}

static Optional<NumericType> from_ffi_numeric_type(StyleValueFFI::FfiNumericType const&);

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
        to_ffi_numeric_type(resolved_type),
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
    return from_ffi_numeric_type(m_value->calculated._0->resolved_type).release_value();
}

CalculationContext CalculatedStyleValue::calculation_context() const
{
    auto const& data = *m_value->calculated._0;
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
    return for_property(property.id());
}

CalculationContext CalculationContext::for_property(PropertyID property_id)
{
    // FIXME: Handle registered custom properties, which may limit which types they accept.
    return {
        .percentages_resolve_as = property_resolves_percentages_relative_to(property_id),
        .resolve_numbers_as_integers = property_accepts_type(property_id, ValueType::Integer),
        .accepted_ranges_by_type = property_accepted_ranges_by_value_type(property_id),
    };
}

// The Number::Type discriminants cross the boundary in the numeric leaf's unit slot; pin them.
static_assert(to_underlying(Number::Type::Number) == 0);
static_assert(to_underlying(Number::Type::IntegerWithExplicitSign) == 1);
static_assert(to_underlying(Number::Type::Integer) == 2);

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

static Optional<NumericType> from_ffi_numeric_type(StyleValueFFI::FfiNumericType const& type)
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

struct CalcResolutionSnapshot {
    explicit CalcResolutionSnapshot(CalculationResolutionContext const& resolution_context)
        : element_facts { resolution_context.length_resolution_context.has_value() ? &*resolution_context.length_resolution_context : nullptr, resolution_context.abstract_element }
        , ffi_element_facts(element_facts.to_ffi())
    {
        ffi_context.element_facts = &ffi_element_facts;
        if (resolution_context.length_resolution_context.has_value()) {
            length_resolution_context = to_ffi_length_resolution_context(*resolution_context.length_resolution_context);
            ffi_context.length_resolution_context = &length_resolution_context.value();
        }
        resolution_context.percentage_basis.visit(
            [](Empty const&) { },
            [&](Angle const& angle) {
                ffi_context.basis_kind = 1;
                ffi_context.basis_value = angle.raw_value();
                ffi_context.basis_unit = to_underlying(angle.unit());
            },
            [&](Frequency const& frequency) {
                ffi_context.basis_kind = 2;
                ffi_context.basis_value = frequency.raw_value();
                ffi_context.basis_unit = to_underlying(frequency.unit());
            },
            [&](Length const& length) {
                ffi_context.basis_kind = 3;
                ffi_context.basis_value = length.raw_value();
                ffi_context.basis_unit = to_underlying(length.unit());
            },
            [&](Time const& time) {
                ffi_context.basis_kind = 4;
                ffi_context.basis_value = time.raw_value();
                ffi_context.basis_unit = to_underlying(time.unit());
            });
    }

    CalcResolutionSnapshot(CalcResolutionSnapshot const&) = delete;

    ElementFactsForRust element_facts;
    StyleValueFFI::FfiElementFacts ffi_element_facts;
    Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_resolution_context;
    StyleValueFFI::FfiCalcResolutionContext ffi_context {};
};

bool CalculatedStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;

    // NB: Structural equality runs entirely over the Rust value graph, including the style
    //     values retained by random() and non-math-function nodes.
    return StyleValueFFI::rust_calc_equals(m_value.operator->(), other.as_calculated().m_value.operator->());
}

// https://drafts.csswg.org/css-values-4/#calc-computed-value
Optional<CalculatedStyleValue::ResolvedValue> CalculatedStyleValue::resolve_value(CalculationResolutionContext const& resolution_context, bool apply_censoring_and_clamping) const
{
    // The resolution runs in the Rust style computation core.
    CalcResolutionSnapshot resolution_snapshot { resolution_context };

    auto rust_result = StyleValueFFI::rust_calc_resolve(m_value.operator->(), &resolution_snapshot.ffi_context, apply_censoring_and_clamping);
    if (!rust_result.resolved)
        return {};
    return ResolvedValue { rust_result.value, from_ffi_numeric_type(rust_result.numeric_type) };
}

Optional<Angle> CalculatedStyleValue::resolve_angle(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_angle(calculation_context.percentages_resolve_as))
        return Angle::make_degrees(result->value);

    return {};
}

Optional<Length> CalculatedStyleValue::resolve_length(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_length(calculation_context.percentages_resolve_as))
        return Length::make_px(result->value);

    return {};
}

Optional<Percentage> CalculatedStyleValue::resolve_percentage(CalculationResolutionContext const& context) const
{
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_percentage())
        return Percentage { result->value };

    return {};
}

Optional<Resolution> CalculatedStyleValue::resolve_resolution(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_resolution(calculation_context.percentages_resolve_as))
        return Resolution::make_dots_per_pixel(result->value);

    return {};
}

Optional<Time> CalculatedStyleValue::resolve_time(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_time(calculation_context.percentages_resolve_as))
        return Time::make_seconds(result->value);

    return {};
}

Optional<double> CalculatedStyleValue::resolve_number(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_number(calculation_context.percentages_resolve_as))
        return result->value;

    return {};
}

Optional<i32> CalculatedStyleValue::resolve_integer(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_number(calculation_context.percentages_resolve_as))
        return round_to_nearest_integer(result->value);

    return {};
}

bool CalculatedStyleValue::contains_percentage() const
{
    // The Rust mirror answers.
    return StyleValueFFI::rust_calc_node_contains_percentage(m_value->calculated._0->rust_calculation.node);
}

Optional<NumericType> CalcNodeRef::determine_type(CalculationContext const& context) const
{
    auto resolve_as_base = context.percentages_resolve_as.has_value()
        ? NumericType::base_type_from_value_type(*context.percentages_resolve_as)
        : OptionalNone {};
    return from_ffi_numeric_type(StyleValueFFI::rust_calc_node_determine_type(
        m_node,
        context.percentages_resolve_as.has_value(),
        context.percentages_resolve_as == ValueType::Number,
        resolve_as_base.has_value() ? to_underlying(*resolve_as_base) : 0));
}

}
