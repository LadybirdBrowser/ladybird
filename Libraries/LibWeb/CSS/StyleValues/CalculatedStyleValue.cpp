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
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/CSS/StyleValues/AbstractNonMathCalcFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalcNodeRef.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RandomValueSharingStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>

namespace Web::CSS {

static ValueComparingNonnullRefPtr<StyleValue const> wrap_borrowed_style_value_data(void const* data)
{
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
        static_cast<StyleValueFFI::StyleValueData const*>(data)));
}

// Marshals a numeric type into its FFI mirror.
static StyleValueFFI::FfiNumericType to_ffi_numeric_type(Optional<NumericType> const& type)
{
    StyleValueFFI::FfiNumericType result {};
    result.valid = type.has_value();
    if (type.has_value()) {
        type->for_each_type_and_exponent([&](auto base_type, i32 exponent) {
            result.has_exponent[to_underlying(base_type)] = true;
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
    return from_ffi_numeric_type(m_value->calculated.resolved_type).release_value();
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

// The RoundingStrategy discriminants cross the boundary as round()'s strategy code; pin them.
static_assert(to_underlying(RoundingStrategy::Down) == 0);
static_assert(to_underlying(RoundingStrategy::Nearest) == 1);
static_assert(to_underlying(RoundingStrategy::ToZero) == 2);
static_assert(to_underlying(RoundingStrategy::Up) == 3);

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
        if (type.has_exponent[i])
            result.set_exponent(static_cast<NumericType::BaseType>(i), type.exponents[i]);
    }
    if (type.has_percent_hint)
        result.set_percent_hint(static_cast<NumericType::BaseType>(type.percent_hint));
    return result;
}

struct CalcResolutionSnapshot {
    CalcResolutionSnapshot(StyleValueFFI::CalcNode const* root, CalculationContext const& calculation_context, CalculationResolutionContext const& resolution_context)
    {
        if (resolution_context.length_resolution_context.has_value()) {
            length_resolution_context = to_ffi_length_resolution_context(*resolution_context.length_resolution_context);
            ffi_context.length_resolution_context = &length_resolution_context.value();
        }
        resolution_context.percentage_basis.visit(
            [](Empty const&) {},
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

        external_resolutions = StyleValueFFI::rust_calc_external_resolutions(root, ffi_context.basis_kind, ffi_context.basis_value, ffi_context.basis_unit);
        ffi_context.external_resolutions = external_resolutions.resolutions;
        ffi_context.external_resolution_count = external_resolutions.resolution_count;
        for (auto& resolution : Span<StyleValueFFI::FfiCalcExternalResolution> { external_resolutions.resolutions, external_resolutions.resolution_count }) {
            switch (resolution.kind) {
            case StyleValueFFI::FfiCalcExternalResolutionKind::NonMathFunction: {
                auto function = wrap_borrowed_style_value_data(resolution.source);
                auto resolved = static_cast<AbstractNonMathCalcFunctionStyleValue const&>(*function).resolve_to_calculation_node(calculation_context, resolution_context);
                if (resolved.has_value())
                    resolution.resolved_node = resolved->release();
                break;
            }
            case StyleValueFFI::FfiCalcExternalResolutionKind::Channel:
                if (resolution_context.relative_color.has_value()) {
                    if (auto value = resolution_context.relative_color->get(static_cast<ChannelKeyword>(resolution.unit_or_channel)); value.has_value()) {
                        resolution.has_number = true;
                        resolution.number = value.value();
                    }
                }
                break;
            case StyleValueFFI::FfiCalcExternalResolutionKind::RandomSharing: {
                auto sharing = wrap_borrowed_style_value_data(resolution.source);
                // When we are in the absolutization process we should absolutize the sharing options.
                if (resolution_context.length_resolution_context.has_value()) {
                    ComputationContext context { resolution_context.length_resolution_context.value(), resolution_context.abstract_element };
                    auto absolutized = sharing->as_random_value_sharing().absolutized(context);
                    resolution.resolved_style_value = StyleValueFFI::rust_style_value_retain(absolutized->rust_style_value_data());
                    resolution.has_number = true;
                    resolution.number = absolutized->as_random_value_sharing().random_base_value();
                } else if (resolution_context.abstract_element.has_value() || !resolution_context.percentage_basis.has<Empty>()) {
                    // NB: We don't want to resolve this before computation time even if it's possible.
                    resolution.has_number = true;
                    resolution.number = sharing->as_random_value_sharing().random_base_value();
                }
                break;
            }
            case StyleValueFFI::FfiCalcExternalResolutionKind::Length:
                if (resolution_context.length_resolution_context.has_value()) {
                    resolution.has_number = true;
                    resolution.number = Length { resolution.input_value, static_cast<LengthUnit>(resolution.unit_or_channel) }.to_px(*resolution_context.length_resolution_context).to_double();
                }
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }
    }

    ~CalcResolutionSnapshot() { StyleValueFFI::rust_calc_external_resolutions_release(external_resolutions.storage); }

    Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_resolution_context;
    StyleValueFFI::FfiCalcExternalResolutions external_resolutions {};
    StyleValueFFI::FfiCalcResolutionContext ffi_context {};
};

ValueComparingNonnullRefPtr<StyleValue const> CalculatedStyleValue::absolutized(ComputationContext const& computation_context) const
{
    // NB: Materialize the context once; rebuilding it per use is a HashMap construction each time.
    auto calculation_context = this->calculation_context();
    auto resolution_context = CalculationResolutionContext::from_computation_context(computation_context);

    CalcResolutionSnapshot resolution_snapshot { m_value->calculated.rust_calculation.node, calculation_context, resolution_context };

    auto result = StyleValueFFI::rust_calc_absolutize(m_value.operator->(), &resolution_snapshot.ffi_context);
    if (result.is_percentage)
        return PercentageStyleValue::create(Percentage { result.percentage_value });

    if (result.collapsed)
        return adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(result.collapsed));

    // The simplified root transfers straight into the new value's data; no
    // C++ tree is materialized.
    return adopt_ref(*new (nothrow) CalculatedStyleValue(result.simplified, resolved_type(), calculation_context));
}

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
    return resolve_value(calculation_context(), resolution_context, apply_censoring_and_clamping);
}

Optional<CalculatedStyleValue::ResolvedValue> CalculatedStyleValue::resolve_value(CalculationContext const& calculation_context, CalculationResolutionContext const& resolution_context, bool apply_censoring_and_clamping) const
{
    // The resolution runs in the Rust style computation core.
    CalcResolutionSnapshot resolution_snapshot { m_value->calculated.rust_calculation.node, calculation_context, resolution_context };

    auto rust_result = StyleValueFFI::rust_calc_resolve(m_value.operator->(), &resolution_snapshot.ffi_context, apply_censoring_and_clamping);
    if (!rust_result.resolved)
        return {};
    return ResolvedValue { rust_result.value, from_ffi_numeric_type(rust_result.numeric_type) };
}

Optional<Angle> CalculatedStyleValue::resolve_angle(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(calculation_context, context);

    if (result.has_value() && result->type.has_value() && result->type->matches_angle(calculation_context.percentages_resolve_as))
        return Angle::make_degrees(result->value);

    return {};
}

Optional<Flex> CalculatedStyleValue::resolve_flex(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(calculation_context, context);

    if (result.has_value() && result->type.has_value() && result->type->matches_flex(calculation_context.percentages_resolve_as))
        return Flex::make_fr(result->value);

    return {};
}

Optional<Length> CalculatedStyleValue::resolve_length(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(calculation_context, context);

    if (result.has_value() && result->type.has_value() && result->type->matches_length(calculation_context.percentages_resolve_as))
        return Length::make_px(result->value);

    return {};
}

Optional<double> CalculatedStyleValue::resolve_raw_length(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(calculation_context, context, false);

    if (result.has_value() && result->type.has_value() && result->type->matches_length(calculation_context.percentages_resolve_as))
        return result->value;

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
    auto result = resolve_value(calculation_context, context);

    if (result.has_value() && result->type.has_value() && result->type->matches_resolution(calculation_context.percentages_resolve_as))
        return Resolution::make_dots_per_pixel(result->value);

    return {};
}

Optional<Time> CalculatedStyleValue::resolve_time(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(calculation_context, context);

    if (result.has_value() && result->type.has_value() && result->type->matches_time(calculation_context.percentages_resolve_as))
        return Time::make_seconds(result->value);

    return {};
}

Optional<double> CalculatedStyleValue::resolve_number(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(calculation_context, context);

    if (result.has_value() && result->type.has_value() && result->type->matches_number(calculation_context.percentages_resolve_as))
        return result->value;

    return {};
}

Optional<i32> CalculatedStyleValue::resolve_integer(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(calculation_context, context);

    if (result.has_value() && result->type.has_value() && result->type->matches_number(calculation_context.percentages_resolve_as))
        return round_to_nearest_integer(result->value);

    return {};
}

RefPtr<StyleValue const> CalculatedStyleValue::resolve_as_style_value(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(calculation_context, context);
    if (!result.has_value() || !result->type.has_value())
        return {};

    if (result->type->matches_number(calculation_context.percentages_resolve_as)) {
        if (calculation_context.resolve_numbers_as_integers)
            return IntegerStyleValue::create(round_to_nearest_integer(result->value));
        return NumberStyleValue::create(result->value);
    }
    if (result->type->matches_angle(calculation_context.percentages_resolve_as))
        return AngleStyleValue::create(Angle::make_degrees(result->value));
    if (result->type->matches_flex(calculation_context.percentages_resolve_as))
        return FlexStyleValue::create(Flex::make_fr(result->value));
    if (result->type->matches_frequency(calculation_context.percentages_resolve_as))
        return FrequencyStyleValue::create(Frequency::make_hertz(result->value));
    if (result->type->matches_length(calculation_context.percentages_resolve_as))
        return LengthStyleValue::create(Length::make_px(result->value));
    if (result->type->matches_percentage())
        return PercentageStyleValue::create(Percentage { result->value });
    if (result->type->matches_resolution(calculation_context.percentages_resolve_as))
        return ResolutionStyleValue::create(Resolution::make_dots_per_pixel(result->value));
    if (result->type->matches_time(calculation_context.percentages_resolve_as))
        return TimeStyleValue::create(Time::make_seconds(result->value));

    return {};
}

bool CalculatedStyleValue::contains_percentage() const
{
    // The Rust mirror answers.
    return StyleValueFFI::rust_calc_node_contains_percentage(m_value->calculated.rust_calculation.node);
}

bool CalculatedStyleValue::is_fully_simplified() const
{
    return resolve_value({}).has_value();
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-math-expression
static GC::Ptr<CSSNumericValue> reify_rust_calculation(void const* calculated_data)
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
        auto numeric_type = from_ffi_numeric_type(node.numeric_type).release_value();
        VERIFY(node.child_start + node.child_count <= child_indices.size());
        auto children = child_indices.slice(node.child_start, node.child_count);
        auto child = [&](size_t index) -> GC::Ref<CSSNumericValue> {
            VERIFY(index < children.size());
            VERIFY(children[index] < reified_nodes.size());
            return reified_nodes[children[index]];
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
            switch (node.numeric_kind) {
            case 0:
                reified_nodes.append(CSSUnitValue::create(node.value, "number"_utf16_fly_string));
                break;
            case 1:
                reified_nodes.append(CSSUnitValue::create(node.value, Angle { node.value, static_cast<AngleUnit>(node.unit) }.unit_name()));
                break;
            case 2:
                reified_nodes.append(CSSUnitValue::create(node.value, Flex { node.value, static_cast<FlexUnit>(node.unit) }.unit_name()));
                break;
            case 3:
                reified_nodes.append(CSSUnitValue::create(node.value, Frequency { node.value, static_cast<FrequencyUnit>(node.unit) }.unit_name()));
                break;
            case 4:
                reified_nodes.append(CSSUnitValue::create(node.value, Length { node.value, static_cast<LengthUnit>(node.unit) }.unit_name()));
                break;
            case 5:
                reified_nodes.append(CSSUnitValue::create(node.value, "percent"_utf16_fly_string));
                break;
            case 6:
                reified_nodes.append(CSSUnitValue::create(node.value, Resolution { node.value, static_cast<ResolutionUnit>(node.unit) }.unit_name()));
                break;
            case 7:
                reified_nodes.append(CSSUnitValue::create(node.value, Time { node.value, static_cast<TimeUnit>(node.unit) }.unit_name()));
                break;
            default:
                VERIFY_NOT_REACHED();
            }
            break;
        case 2:
            reified_nodes.append(CSSMathSum::create(move(numeric_type), reify_children()));
            break;
        case 3:
            reified_nodes.append(CSSMathProduct::create(move(numeric_type), reify_children()));
            break;
        case 4:
            VERIFY(children.size() == 1);
            reified_nodes.append(CSSMathNegate::create(move(numeric_type), child(0)));
            break;
        case 5:
            VERIFY(children.size() == 1);
            reified_nodes.append(CSSMathInvert::create(move(numeric_type), child(0)));
            break;
        case 6:
            reified_nodes.append(CSSMathMin::create(move(numeric_type), reify_children()));
            break;
        case 7:
            reified_nodes.append(CSSMathMax::create(move(numeric_type), reify_children()));
            break;
        case 8:
            VERIFY(children.size() == 3);
            reified_nodes.append(CSSMathClamp::create(move(numeric_type), child(0), child(1), child(2)));
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
    return reified_nodes.last();
}

bool CalculatedStyleValue::contains_anchor_function() const
{
    return StyleValueFFI::rust_calc_contains_anchor(m_value.operator->());
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

CalcNodeRef CalcNodeRef::numeric(NumericValue const& value)
{
    return adopt(value.visit(
        [](Number const& number) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(0, number.value(), to_underlying(number.type())); },
        [](Angle const& angle) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(1, angle.raw_value(), to_underlying(angle.unit())); },
        [](Flex const& flex) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(2, flex.raw_value(), to_underlying(flex.unit())); },
        [](Frequency const& frequency) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(3, frequency.raw_value(), to_underlying(frequency.unit())); },
        [](Length const& length) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(4, length.raw_value(), to_underlying(length.unit())); },
        [](Percentage const& percentage) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(5, percentage.value(), 0); },
        [](Resolution const& resolution) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(6, resolution.raw_value(), to_underlying(resolution.unit())); },
        [](Time const& time) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(7, time.raw_value(), to_underlying(time.unit())); }));
}

Optional<CalcNodeRef> CalcNodeRef::from_keyword(Keyword keyword)
{
    switch (keyword) {
    case Keyword::E:
        // https://drafts.csswg.org/css-values-4/#valdef-calc-e
        return numeric(Number { Number::Type::Number, AK::E<double> });
    case Keyword::Pi:
        // https://drafts.csswg.org/css-values-4/#valdef-calc-pi
        return numeric(Number { Number::Type::Number, AK::Pi<double> });
    case Keyword::Infinity:
        // https://drafts.csswg.org/css-values-4/#valdef-calc-infinity
        return numeric(Number { Number::Type::Number, AK::Infinity<double> });
    case Keyword::NegativeInfinity:
        // https://drafts.csswg.org/css-values-4/#valdef-calc--infinity
        return numeric(Number { Number::Type::Number, -AK::Infinity<double> });
    case Keyword::Nan:
        // https://drafts.csswg.org/css-values-4/#valdef-calc-nan
        return numeric(Number { Number::Type::Number, AK::NaN<double> });
    default:
        return {};
    }
}

CalcNodeRef CalcNodeRef::channel_keyword(ChannelKeyword channel)
{
    return adopt(StyleValueFFI::rust_calc_node_create_channel_keyword(to_underlying(channel)));
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
CalcNodeRef CalcNodeRef::hypot(Vector<CalcNodeRef> children) { return make_variadic_node(4, move(children)); }

static CalcNodeRef make_unary_node(u8 kind, CalcNodeRef child)
{
    return CalcNodeRef::adopt(StyleValueFFI::rust_calc_node_create_unary(kind, child.release()));
}

CalcNodeRef CalcNodeRef::negate(CalcNodeRef child) { return make_unary_node(0, move(child)); }
CalcNodeRef CalcNodeRef::invert(CalcNodeRef child) { return make_unary_node(1, move(child)); }
CalcNodeRef CalcNodeRef::abs(CalcNodeRef child) { return make_unary_node(2, move(child)); }
CalcNodeRef CalcNodeRef::sign(CalcNodeRef child) { return make_unary_node(3, move(child)); }
CalcNodeRef CalcNodeRef::sin(CalcNodeRef child) { return make_unary_node(4, move(child)); }
CalcNodeRef CalcNodeRef::cos(CalcNodeRef child) { return make_unary_node(5, move(child)); }
CalcNodeRef CalcNodeRef::tan(CalcNodeRef child) { return make_unary_node(6, move(child)); }
CalcNodeRef CalcNodeRef::asin(CalcNodeRef child) { return make_unary_node(7, move(child)); }
CalcNodeRef CalcNodeRef::acos(CalcNodeRef child) { return make_unary_node(8, move(child)); }
CalcNodeRef CalcNodeRef::atan(CalcNodeRef child) { return make_unary_node(9, move(child)); }
CalcNodeRef CalcNodeRef::sqrt(CalcNodeRef child) { return make_unary_node(10, move(child)); }
CalcNodeRef CalcNodeRef::exp(CalcNodeRef child) { return make_unary_node(11, move(child)); }

static CalcNodeRef make_binary_node(u8 kind, CalcNodeRef first, CalcNodeRef second)
{
    auto const* first_handle = first.release();
    return CalcNodeRef::adopt(StyleValueFFI::rust_calc_node_create_binary(kind, first_handle, second.release()));
}

CalcNodeRef CalcNodeRef::atan2(CalcNodeRef y, CalcNodeRef x) { return make_binary_node(0, move(y), move(x)); }
CalcNodeRef CalcNodeRef::pow(CalcNodeRef base, CalcNodeRef exponent) { return make_binary_node(1, move(base), move(exponent)); }
CalcNodeRef CalcNodeRef::log(CalcNodeRef value, CalcNodeRef base) { return make_binary_node(2, move(value), move(base)); }
CalcNodeRef CalcNodeRef::mod(CalcNodeRef value, CalcNodeRef modulus) { return make_binary_node(3, move(value), move(modulus)); }
CalcNodeRef CalcNodeRef::rem(CalcNodeRef value, CalcNodeRef divisor) { return make_binary_node(4, move(value), move(divisor)); }

CalcNodeRef CalcNodeRef::clamp(CalcNodeRef minimum, CalcNodeRef center, CalcNodeRef maximum)
{
    auto const* minimum_handle = minimum.release();
    auto const* center_handle = center.release();
    return adopt(StyleValueFFI::rust_calc_node_create_clamp(minimum_handle, center_handle, maximum.release()));
}

CalcNodeRef CalcNodeRef::progress(bool no_clamp, CalcNodeRef value, CalcNodeRef start, CalcNodeRef end)
{
    auto const* value_handle = value.release();
    auto const* start_handle = start.release();
    return adopt(StyleValueFFI::rust_calc_node_create_progress(no_clamp, value_handle, start_handle, end.release()));
}

CalcNodeRef CalcNodeRef::round(RoundingStrategy strategy, CalcNodeRef value, CalcNodeRef interval)
{
    auto const* value_handle = value.release();
    return adopt(StyleValueFFI::rust_calc_node_create_round(to_underlying(strategy), value_handle, interval.release()));
}

CalcNodeRef CalcNodeRef::random(StyleValue const& random_value_sharing, CalcNodeRef minimum, CalcNodeRef maximum, Optional<CalcNodeRef> step)
{
    auto const* minimum_handle = minimum.release();
    auto const* maximum_handle = maximum.release();
    return adopt(StyleValueFFI::rust_calc_node_create_random(
        minimum_handle, maximum_handle,
        step.has_value() ? step->release() : nullptr,
        StyleValueFFI::rust_style_value_retain(random_value_sharing.rust_style_value_data())));
}

CalcNodeRef CalcNodeRef::non_math_function(StyleValue const& function, Optional<NumericType> const& numeric_type)
{
    auto ffi_numeric_type = to_ffi_numeric_type(numeric_type);
    return adopt(StyleValueFFI::rust_calc_node_create_non_math_function(
        StyleValueFFI::rust_style_value_retain(function.rust_style_value_data()), &ffi_numeric_type));
}

CalcNodeRef CalcNodeRef::from_style_value(StyleValue const& style_value)
{
    switch (style_value.type()) {
    case StyleValue::Type::Angle:
        return numeric(style_value.as_angle().angle());
    case StyleValue::Type::Frequency:
        return numeric(style_value.as_frequency().frequency());
    case StyleValue::Type::Integer:
        return numeric(Number { Number::Type::Number, static_cast<double>(style_value.as_integer().integer()) });
    case StyleValue::Type::Length:
        return numeric(style_value.as_length().length());
    case StyleValue::Type::Number:
        return numeric(Number { Number::Type::Number, style_value.as_number().number() });
    case StyleValue::Type::Percentage:
        return numeric(style_value.as_percentage().percentage());
    case StyleValue::Type::Time:
        return numeric(style_value.as_time().time());
    case StyleValue::Type::Calculated:
        return retain(style_value.as_calculated().rust_calculation_root());
    default:
        VERIFY_NOT_REACHED();
    }
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

// https://drafts.csswg.org/css-values-4/#calc-simplification
CalcNodeRef simplify_a_calculation_tree(CalcNodeRef const& root, CalculationContext const& context, CalculationResolutionContext const& resolution_context)
{
    CalcResolutionSnapshot resolution_snapshot { root.node(), context, resolution_context };

    auto resolve_as_base = context.percentages_resolve_as.has_value()
        ? NumericType::base_type_from_value_type(*context.percentages_resolve_as)
        : OptionalNone {};

    return CalcNodeRef::adopt(StyleValueFFI::rust_calc_simplify_tree(
        root.node(),
        &resolution_snapshot.ffi_context,
        context.percentages_resolve_as.has_value(),
        context.percentages_resolve_as == ValueType::Number,
        resolve_as_base.has_value() ? to_underlying(*resolve_as_base) : 0));
}

}
