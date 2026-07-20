/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CalculatedStyleValue.h"
#include <AK/QuickSort.h>
#include <AK/TypeCasts.h>
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

// Builds the Rust mirror of a calculation tree, transferring ownership of the
// returned handle to the caller. The child orders follow each node's members.
StyleValueFFI::StyleValueData* CalculatedStyleValue::make_calculated_data_from_rust_root(StyleValueFFI::CalcNode const* rust_root, NumericType const& resolved_type, CalculationContext const& context)
{
    static_assert(IsTriviallyCopyable<NumericType>);
    auto resolved_type_bytes = bit_cast<Array<u8, sizeof(NumericType)>>(resolved_type);
    Vector<StyleValueFFI::RetainedNumericRangeByType> ranges;
    ranges.ensure_capacity(context.accepted_ranges_by_type.size());
    for (auto const& [value_type, range] : context.accepted_ranges_by_type)
        ranges.unchecked_append({ to_underlying(value_type), range.min, range.max });
    auto resolve_as_base = context.percentages_resolve_as.has_value()
        ? NumericType::base_type_from_value_type(*context.percentages_resolve_as)
        : OptionalNone {};
    return StyleValueFFI::rust_style_value_create_calculated(
        rust_root,
        resolved_type_bytes.data(), resolved_type_bytes.size(),
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
    auto const& blob = m_value->calculated.resolved_type;
    Array<u8, sizeof(NumericType)> bytes;
    VERIFY(blob.length == bytes.size());
    __builtin_memcpy(bytes.data(), blob.pointer, bytes.size());
    return bit_cast<NumericType>(bytes);
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

// https://drafts.csswg.org/css-values-4/#funcdef-min
void CalculatedStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    // The serialization structure lives in the Rust style computation core; every formatted
    // byte still comes from the value serializers below. Trees containing random() keep the
    // C++ path, like resolution does.
    struct SerializationCallbackContext {
        StringBuilder& builder;
        SerializationMode mode;
    } callback_context { builder, mode };

    StyleValueFFI::FfiCalcSerializationCallbacks const callbacks {
        .context = &callback_context,
        .append_literal = [](void* context, u8 const* bytes, size_t length) {
            auto& callback_context = *static_cast<SerializationCallbackContext*>(context);
            callback_context.builder.append(StringView { bytes, length }); },
        .append_numeric_leaf = [](void* context, u8 kind, double value, u8 unit, bool) {
            auto& callback_context = *static_cast<SerializationCallbackContext*>(context);
            switch (kind) {
            case 0:
                Number { static_cast<Number::Type>(unit), value }.serialize(callback_context.builder, callback_context.mode);
                return;
            case 1:
                Angle { value, static_cast<AngleUnit>(unit) }.serialize(callback_context.builder, callback_context.mode);
                return;
            case 2:
                Flex { value, static_cast<FlexUnit>(unit) }.serialize(callback_context.builder, callback_context.mode);
                return;
            case 3:
                Frequency { value, static_cast<FrequencyUnit>(unit) }.serialize(callback_context.builder, callback_context.mode);
                return;
            case 4:
                Length { value, static_cast<LengthUnit>(unit) }.serialize(callback_context.builder, callback_context.mode);
                return;
            case 5:
                Percentage { value }.serialize(callback_context.builder, callback_context.mode);
                return;
            case 6:
                Resolution { value, static_cast<ResolutionUnit>(unit) }.serialize(callback_context.builder, callback_context.mode);
                return;
            case 7:
                Time { value, static_cast<TimeUnit>(unit) }.serialize(callback_context.builder, callback_context.mode);
                return;
            }
            VERIFY_NOT_REACHED(); },
        .append_style_value = [](void* context, void const* shell) -> bool {
            auto& callback_context = *static_cast<SerializationCallbackContext*>(context);
            auto start_length = callback_context.builder.length();
            static_cast<StyleValue const*>(shell)->serialize(callback_context.builder, callback_context.mode);
            return callback_context.builder.length() > start_length; },
        .append_channel_name = [](void* context, u8 channel) {
            auto& callback_context = *static_cast<SerializationCallbackContext*>(context);
            callback_context.builder.append(CSS::to_string(static_cast<ChannelKeyword>(channel))); },
    };
    StyleValueFFI::rust_calc_serialize(m_value.operator->(), &callbacks, mode == SerializationMode::ResolvedValue);
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

// The callback seams shared by resolution and absolutization: the C++ leaf
// I/O the Rust calc core calls back into while simplifying a tree.
struct CalcResolveCallbackContext {
    CalculationContext const& calculation_context;
    CalculationResolutionContext const& resolution_context;
};

static StyleValueFFI::FfiCalcResolutionContext make_calc_ffi_resolution_context(CalcResolveCallbackContext& callback_context, Optional<ComputedValuesFFI::FfiLengthResolutionContext>& length_context_storage)
{
    StyleValueFFI::FfiCalcResolutionContext ffi_context {
        .basis_kind = 0,
        .basis_value = 0,
        .basis_unit = 0,
        .length_resolution_context = nullptr,
        .callback_context = &callback_context,
        .resolve_non_math_function = [](void* context, void const* shell) -> StyleValueFFI::CalcNode const* {
            auto& callback_context = *static_cast<CalcResolveCallbackContext*>(context);
            auto resolved = static_cast<AbstractNonMathCalcFunctionStyleValue const*>(shell)->resolve_to_calculation_node(callback_context.calculation_context, callback_context.resolution_context);
            if (!resolved.has_value())
                return nullptr;
            return resolved->release();
        },
        .resolve_channel_keyword = [](void* context, u8 channel, double* out_value) -> bool {
            auto& callback_context = *static_cast<CalcResolveCallbackContext*>(context);
            if (!callback_context.resolution_context.relative_color.has_value())
                return false;
            auto resolved = callback_context.resolution_context.relative_color->get(static_cast<ChannelKeyword>(channel));
            if (!resolved.has_value())
                return false;
            *out_value = resolved.value();
            return true;
        },
        .random_base_value = [](void* context, void const* sharing, double* out_value) -> bool {
            auto& callback_context = *static_cast<CalcResolveCallbackContext*>(context);
            // NB: We don't want to resolve this before computation time even if it's possible.
            auto const& resolution_context = callback_context.resolution_context;
            if (!resolution_context.abstract_element.has_value() && !resolution_context.length_resolution_context.has_value() && resolution_context.percentage_basis.has<Empty>())
                return false;
            *out_value = static_cast<RandomValueSharingStyleValue const*>(sharing)->random_base_value();
            return true;
        },
        .absolutize_random_sharing = [](void* context, void const* sharing) -> void const* {
            auto& callback_context = *static_cast<CalcResolveCallbackContext*>(context);
            auto const& resolution_context = callback_context.resolution_context;
            // When we are in the absolutization process we should absolutize the sharing options.
            if (!resolution_context.length_resolution_context.has_value())
                return nullptr;
            ComputationContext computation_context {
                .length_resolution_context = resolution_context.length_resolution_context.value(),
                .abstract_element = resolution_context.abstract_element
            };
            auto absolutized = static_cast<RandomValueSharingStyleValue const*>(sharing)->absolutized(computation_context);
            return retain_style_value_for_rust(absolutized.ptr());
        },
        .resolve_length = [](void* context, double value, u8 unit, double* out_px) -> bool {
            auto& callback_context = *static_cast<CalcResolveCallbackContext*>(context);
            auto const& resolution_context = callback_context.resolution_context;
            if (!resolution_context.length_resolution_context.has_value())
                return false;
            *out_px = Length { value, static_cast<LengthUnit>(unit) }.to_px(*resolution_context.length_resolution_context).to_double();
            return true;
        },
    };
    auto const& resolution_context = callback_context.resolution_context;
    if (resolution_context.length_resolution_context.has_value()) {
        length_context_storage = to_ffi_length_resolution_context(*resolution_context.length_resolution_context);
        ffi_context.length_resolution_context = &length_context_storage.value();
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

    return ffi_context;
}

ValueComparingNonnullRefPtr<StyleValue const> CalculatedStyleValue::absolutized(ComputationContext const& computation_context) const
{
    // NB: Materialize the context once; rebuilding it per use is a HashMap construction each time.
    auto calculation_context = this->calculation_context();
    auto resolution_context = CalculationResolutionContext::from_computation_context(computation_context);

    CalcResolveCallbackContext callback_context { calculation_context, resolution_context };
    Optional<ComputedValuesFFI::FfiLengthResolutionContext> ffi_length_resolution_context;
    auto ffi_context = make_calc_ffi_resolution_context(callback_context, ffi_length_resolution_context);

    auto result = StyleValueFFI::rust_calc_absolutize(m_value.operator->(), &ffi_context);
    if (result.is_percentage)
        return PercentageStyleValue::create(Percentage { result.percentage_value });

    // The simplified root transfers straight into the new value's data; no
    // C++ tree is materialized.
    return adopt_ref(*new (nothrow) CalculatedStyleValue(result.simplified, resolved_type(), calculation_context));
}

bool CalculatedStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;

    // Structural equality runs over the Rust trees; the style values carried by
    // random() and non-math-function nodes compare through their own equals.
    return StyleValueFFI::rust_calc_equals(
        m_value.operator->(), other.as_calculated().m_value.operator->(), nullptr,
        [](void*, void const* a, void const* b) -> bool {
            return static_cast<StyleValue const*>(a)->equals(*static_cast<StyleValue const*>(b));
        });
}

// https://drafts.csswg.org/css-values-4/#calc-computed-value
Optional<CalculatedStyleValue::ResolvedValue> CalculatedStyleValue::resolve_value(CalculationResolutionContext const& resolution_context, bool apply_censoring_and_clamping) const
{
    return resolve_value(calculation_context(), resolution_context, apply_censoring_and_clamping);
}

Optional<CalculatedStyleValue::ResolvedValue> CalculatedStyleValue::resolve_value(CalculationContext const& calculation_context, CalculationResolutionContext const& resolution_context, bool apply_censoring_and_clamping) const
{
    // The resolution runs in the Rust style computation core.
    CalcResolveCallbackContext callback_context { calculation_context, resolution_context };
    Optional<ComputedValuesFFI::FfiLengthResolutionContext> ffi_length_resolution_context;
    auto ffi_context = make_calc_ffi_resolution_context(callback_context, ffi_length_resolution_context);

    auto rust_result = StyleValueFFI::rust_calc_resolve(m_value.operator->(), &ffi_context, apply_censoring_and_clamping);
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

Optional<Frequency> CalculatedStyleValue::resolve_frequency(CalculationResolutionContext const& context) const
{
    auto calculation_context = this->calculation_context();
    auto result = resolve_value(calculation_context, context);

    if (result.has_value() && result->type.has_value() && result->type->matches_frequency(calculation_context.percentages_resolve_as))
        return Frequency::make_hertz(result->value);

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

// Reifies one node of the Rust calculation tree into its typed-om object.
static GC::Ptr<CSSNumericValue> reify_rust_calc_node(JS::Realm& realm, void const* calculated_data, StyleValueFFI::CalcNode const* node)
{
    auto numeric_type_of = [&]() {
        return from_ffi_numeric_type(StyleValueFFI::rust_calc_node_numeric_type(calculated_data, node)).value();
    };
    auto children_of = [&]() {
        Vector<StyleValueFFI::CalcNode const*> children;
        auto count = StyleValueFFI::rust_calc_node_children(node, nullptr, 0);
        children.resize(count);
        StyleValueFFI::rust_calc_node_children(node, children.data(), children.size());
        return children;
    };
    auto reify_children_of = [&]() -> GC::Ptr<CSSNumericArray> {
        GC::RootVector<GC::Ref<CSSNumericValue>> reified_children;
        for (auto const* child : children_of()) {
            auto reified_child = reify_rust_calc_node(realm, calculated_data, child);
            if (!reified_child)
                return nullptr;
            reified_children.append(reified_child.as_nonnull());
        }
        return CSSNumericArray::create(realm, move(reified_children));
    };

    switch (StyleValueFFI::rust_calc_node_kind(node)) {
    case 0: {
        // A numeric leaf reifies as a unit value in its own unit.
        u8 kind = 0;
        double value = 0;
        u8 unit = 0;
        StyleValueFFI::rust_calc_node_numeric_leaf(node, &kind, &value, &unit);
        switch (kind) {
        case 0:
            return CSSUnitValue::create(realm, value, "number"_utf16_fly_string);
        case 1:
            return CSSUnitValue::create(realm, value, Angle { value, static_cast<AngleUnit>(unit) }.unit_name());
        case 2:
            return CSSUnitValue::create(realm, value, Flex { value, static_cast<FlexUnit>(unit) }.unit_name());
        case 3:
            return CSSUnitValue::create(realm, value, Frequency { value, static_cast<FrequencyUnit>(unit) }.unit_name());
        case 4:
            return CSSUnitValue::create(realm, value, Length { value, static_cast<LengthUnit>(unit) }.unit_name());
        case 5:
            return CSSUnitValue::create(realm, value, "percent"_utf16_fly_string);
        case 6:
            return CSSUnitValue::create(realm, value, Resolution { value, static_cast<ResolutionUnit>(unit) }.unit_name());
        case 7:
            return CSSUnitValue::create(realm, value, Time { value, static_cast<TimeUnit>(unit) }.unit_name());
        }
        VERIFY_NOT_REACHED();
    }
    case 2: {
        auto reified_children = reify_children_of();
        if (!reified_children)
            return nullptr;
        return CSSMathSum::create(realm, numeric_type_of(), reified_children.as_nonnull());
    }
    case 3: {
        auto reified_children = reify_children_of();
        if (!reified_children)
            return nullptr;
        return CSSMathProduct::create(realm, numeric_type_of(), reified_children.as_nonnull());
    }
    case 4:
    case 5: {
        auto children = children_of();
        VERIFY(children.size() == 1);
        auto reified_child = reify_rust_calc_node(realm, calculated_data, children[0]);
        if (!reified_child)
            return nullptr;
        if (StyleValueFFI::rust_calc_node_kind(node) == 4)
            return CSSMathNegate::create(realm, numeric_type_of(), reified_child.as_nonnull());
        return CSSMathInvert::create(realm, numeric_type_of(), reified_child.as_nonnull());
    }
    case 6: {
        auto reified_children = reify_children_of();
        if (!reified_children)
            return nullptr;
        return CSSMathMin::create(realm, numeric_type_of(), reified_children.as_nonnull());
    }
    case 7: {
        auto reified_children = reify_children_of();
        if (!reified_children)
            return nullptr;
        return CSSMathMax::create(realm, numeric_type_of(), reified_children.as_nonnull());
    }
    case 8: {
        auto children = children_of();
        VERIFY(children.size() == 3);
        auto lower = reify_rust_calc_node(realm, calculated_data, children[0]);
        auto value = reify_rust_calc_node(realm, calculated_data, children[1]);
        auto upper = reify_rust_calc_node(realm, calculated_data, children[2]);
        if (!lower || !value || !upper)
            return nullptr;
        return CSSMathClamp::create(realm, numeric_type_of(), lower.as_nonnull(), value.as_nonnull(), upper.as_nonnull());
    }
    default:
        // Some math functions are not reifiable yet.
        // https://github.com/w3c/css-houdini-drafts/issues/1090
        return nullptr;
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-math-expression
static bool rust_calc_node_contains_anchor(StyleValueFFI::CalcNode const* node)
{
    if (StyleValueFFI::rust_calc_node_kind(node) == 28
        && static_cast<StyleValue const*>(StyleValueFFI::rust_calc_node_style_value(node))->is_anchor())
        return true;
    Vector<StyleValueFFI::CalcNode const*> children;
    auto count = StyleValueFFI::rust_calc_node_children(node, nullptr, 0);
    children.resize(count);
    StyleValueFFI::rust_calc_node_children(node, children.data(), children.size());
    for (auto const* child : children) {
        if (rust_calc_node_contains_anchor(child))
            return true;
    }
    return false;
}

bool CalculatedStyleValue::contains_anchor_function() const
{
    return rust_calc_node_contains_anchor(rust_calculation_root());
}

GC::Ref<CSSStyleValue> CalculatedStyleValue::reify(JS::Realm& realm, Utf16FlyString const& associated_property) const
{
    // NB: This spec algorithm isn't really implementable here - it's incomplete, and assumes we don't already have a
    //     calculation tree. So we have a per-node method instead, walking the Rust tree.
    if (auto reified = reify_rust_calc_node(realm, m_value.operator->(), m_value->calculated.rust_calculation.node))
        return *reified;
    // Some math functions are not reifiable yet. If we contain one, we have to fall back to CSSStyleValue.
    // https://github.com/w3c/css-houdini-drafts/issues/1090
    return default_reify(realm, associated_property);
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
        retain_style_value_for_rust(&random_value_sharing)));
}

CalcNodeRef CalcNodeRef::non_math_function(StyleValue const& function, Optional<NumericType> const& numeric_type)
{
    auto ffi_numeric_type = to_ffi_numeric_type(numeric_type);
    return adopt(StyleValueFFI::rust_calc_node_create_non_math_function(
        retain_style_value_for_rust(&function), &ffi_numeric_type));
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
    CalcResolveCallbackContext callback_context { context, resolution_context };
    Optional<ComputedValuesFFI::FfiLengthResolutionContext> ffi_length_resolution_context;
    auto ffi_context = make_calc_ffi_resolution_context(callback_context, ffi_length_resolution_context);

    auto resolve_as_base = context.percentages_resolve_as.has_value()
        ? NumericType::base_type_from_value_type(*context.percentages_resolve_as)
        : OptionalNone {};

    return CalcNodeRef::adopt(StyleValueFFI::rust_calc_simplify_tree(
        root.node(),
        &ffi_context,
        context.percentages_resolve_as.has_value(),
        context.percentages_resolve_as == ValueType::Number,
        resolve_as_base.has_value() ? to_underlying(*resolve_as_base) : 0));
}

}
