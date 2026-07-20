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
static StyleValueFFI::CalcNode const* to_rust_calc_node(CalculationNode const& node)
{
    auto children_of = [](CalculationNode const& node) {
        Vector<StyleValueFFI::CalcNode const*> handles;
        auto children = node.children();
        handles.ensure_capacity(children.size());
        for (auto const& child : children)
            handles.unchecked_append(to_rust_calc_node(*child));
        return handles;
    };
    auto variadic = [&](u8 kind) {
        auto children = children_of(node);
        return StyleValueFFI::rust_calc_node_create_variadic(kind, children.data(), children.size());
    };
    auto unary = [&](u8 kind) {
        auto children = children_of(node);
        VERIFY(children.size() == 1);
        return StyleValueFFI::rust_calc_node_create_unary(kind, children[0]);
    };
    auto binary = [&](u8 kind) {
        auto children = children_of(node);
        VERIFY(children.size() == 2);
        return StyleValueFFI::rust_calc_node_create_binary(kind, children[0], children[1]);
    };

    switch (node.type()) {
    case CalculationNode::Type::Numeric:
        return static_cast<NumericCalculationNode const&>(node).value().visit(
            [](Number const& number) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(0, number.value(), to_underlying(number.type())); },
            [](Angle const& angle) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(1, angle.raw_value(), to_underlying(angle.unit())); },
            [](Flex const& flex) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(2, flex.raw_value(), to_underlying(flex.unit())); },
            [](Frequency const& frequency) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(3, frequency.raw_value(), to_underlying(frequency.unit())); },
            [](Length const& length) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(4, length.raw_value(), to_underlying(length.unit())); },
            [](Percentage const& percentage) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(5, percentage.value(), 0); },
            [](Resolution const& resolution) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(6, resolution.raw_value(), to_underlying(resolution.unit())); },
            [](Time const& time) { return StyleValueFFI::rust_calc_node_create_numeric_dimension(7, time.raw_value(), to_underlying(time.unit())); });
    case CalculationNode::Type::ChannelKeyword:
        return StyleValueFFI::rust_calc_node_create_channel_keyword(to_underlying(static_cast<ChannelKeywordCalculationNode const&>(node).channel()));
    case CalculationNode::Type::Sum:
        return variadic(0);
    case CalculationNode::Type::Product:
        return variadic(1);
    case CalculationNode::Type::Min:
        return variadic(2);
    case CalculationNode::Type::Max:
        return variadic(3);
    case CalculationNode::Type::Hypot:
        return variadic(4);
    case CalculationNode::Type::Negate:
        return unary(0);
    case CalculationNode::Type::Invert:
        return unary(1);
    case CalculationNode::Type::Abs:
        return unary(2);
    case CalculationNode::Type::Sign:
        return unary(3);
    case CalculationNode::Type::Sin:
        return unary(4);
    case CalculationNode::Type::Cos:
        return unary(5);
    case CalculationNode::Type::Tan:
        return unary(6);
    case CalculationNode::Type::Asin:
        return unary(7);
    case CalculationNode::Type::Acos:
        return unary(8);
    case CalculationNode::Type::Atan:
        return unary(9);
    case CalculationNode::Type::Sqrt:
        return unary(10);
    case CalculationNode::Type::Exp:
        return unary(11);
    // NB: Atan2's children are ordered y then x, matching its members.
    case CalculationNode::Type::Atan2:
        return binary(0);
    case CalculationNode::Type::Pow:
        return binary(1);
    case CalculationNode::Type::Log:
        return binary(2);
    case CalculationNode::Type::Mod:
        return binary(3);
    case CalculationNode::Type::Rem:
        return binary(4);
    case CalculationNode::Type::Clamp: {
        auto children = children_of(node);
        VERIFY(children.size() == 3);
        return StyleValueFFI::rust_calc_node_create_clamp(children[0], children[1], children[2]);
    }
    case CalculationNode::Type::Progress: {
        auto children = children_of(node);
        VERIFY(children.size() == 3);
        return StyleValueFFI::rust_calc_node_create_progress(
            static_cast<ProgressCalculationNode const&>(node).no_clamp(), children[0], children[1], children[2]);
    }
    case CalculationNode::Type::Round: {
        auto const& round = static_cast<RoundCalculationNode const&>(node);
        auto children = children_of(node);
        VERIFY(children.size() == 2);
        return StyleValueFFI::rust_calc_node_create_round(to_underlying(round.rounding_strategy()), children[0], children[1]);
    }
    case CalculationNode::Type::Random: {
        auto const& random = static_cast<RandomCalculationNode const&>(node);
        return StyleValueFFI::rust_calc_node_create_random(
            to_rust_calc_node(random.minimum()),
            to_rust_calc_node(random.maximum()),
            random.step() ? to_rust_calc_node(*random.step()) : nullptr,
            retain_style_value_for_rust(&random.random_value_sharing()));
    }
    case CalculationNode::Type::NonMathFunction: {
        auto const& non_math_function = static_cast<NonMathFunctionCalculationNode const&>(node);
        auto ffi_numeric_type = to_ffi_numeric_type(non_math_function.numeric_type());
        return StyleValueFFI::rust_calc_node_create_non_math_function(
            retain_style_value_for_rust(non_math_function.function().ptr()), &ffi_numeric_type);
    }
    }
    VERIFY_NOT_REACHED();
}

StyleValueFFI::StyleValueData* CalculatedStyleValue::make_calculated_data(NonnullRefPtr<CalculationNode const> const& calculation, NumericType const& resolved_type, CalculationContext const& context)
{
    return make_calculated_data_from_rust_root(to_rust_calc_node(*calculation), resolved_type, context);
}

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

static Optional<NumericType> add_the_types(Vector<NonnullRefPtr<CalculationNode const>> const& nodes)
{
    Optional<NumericType> left_type;
    for (auto const& value : nodes) {
        auto right_type = value->numeric_type();
        if (!right_type.has_value())
            return {};

        if (left_type.has_value()) {
            left_type = left_type->added_to(right_type.value());
        } else {
            left_type = right_type;
        }

        if (!left_type.has_value())
            return {};
    }

    return left_type;
}

static Optional<NumericType> add_the_types(CalculationNode const& a, CalculationNode const& b)
{
    auto a_type = a.numeric_type();
    auto b_type = b.numeric_type();
    if (!a_type.has_value() || !b_type.has_value())
        return {};
    return a_type->added_to(*b_type);
}

static Optional<NumericType> add_the_types(CalculationNode const& a, CalculationNode const& b, CalculationNode const& c)
{
    auto a_type = a.numeric_type();
    auto b_type = b.numeric_type();
    auto c_type = c.numeric_type();
    if (!a_type.has_value() || !b_type.has_value() || !c_type.has_value())
        return {};

    auto a_and_b_type = a_type->added_to(*b_type);
    if (!a_and_b_type.has_value())
        return {};

    return a_and_b_type->added_to(*c_type);
}

static Optional<NumericType> multiply_the_types(Vector<NonnullRefPtr<CalculationNode const>> const& nodes)
{
    // At a * sub-expression, multiply the types of the left and right arguments.
    // The sub-expression’s type is the returned result.
    Optional<NumericType> left_type;
    for (auto const& value : nodes) {
        auto right_type = value->numeric_type();
        if (!right_type.has_value())
            return {};

        if (left_type.has_value()) {
            left_type = left_type->multiplied_by(right_type.value());
        } else {
            left_type = right_type;
        }

        if (!left_type.has_value())
            return {};
    }

    return left_type;
}

static NonnullRefPtr<CalculationNode const> cpp_calc_tree_from_rust(StyleValueFFI::CalcNode const* node, CalculationContext const& context);

NonnullRefPtr<CalculationNode const> CalculationNode::from_style_value(NonnullRefPtr<StyleValue const> const& style_value, CalculationContext const& calculation_context)
{
    switch (style_value->type()) {
    case StyleValue::Type::Angle:
        return NumericCalculationNode::create(style_value->as_angle().angle(), calculation_context);
    case StyleValue::Type::Frequency:
        return NumericCalculationNode::create(style_value->as_frequency().frequency(), calculation_context);
    case StyleValue::Type::Integer:
        return NumericCalculationNode::create(Number { Number::Type::Number, static_cast<double>(style_value->as_integer().integer()) }, calculation_context);
    case StyleValue::Type::Length:
        return NumericCalculationNode::create(style_value->as_length().length(), calculation_context);
    case StyleValue::Type::Number:
        return NumericCalculationNode::create(Number { Number::Type::Number, style_value->as_number().number() }, calculation_context);
    case StyleValue::Type::Percentage:
        return NumericCalculationNode::create(style_value->as_percentage().percentage(), calculation_context);
    case StyleValue::Type::Time:
        return NumericCalculationNode::create(style_value->as_time().time(), calculation_context);
    case StyleValue::Type::Calculated: {
        // NB: Materialize from the Rust tree under the value's own context, which is what
        //     its nodes were typed under when it was created.
        auto const& calculated = style_value->as_calculated();
        return cpp_calc_tree_from_rust(calculated.rust_calculation_root(), calculated.calculation_context());
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

CalculationNode::CalculationNode(Type type, Optional<NumericType> numeric_type)
    : m_type(type)
    , m_numeric_type(move(numeric_type))
{
}

CalculationNode::~CalculationNode() = default;

static NumericType numeric_type_from_calculated_style_value(CalculatedStyleValue::NumericValue const& value, CalculationContext const& context)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // Anything else is a terminal value, whose type is determined based on its CSS type.
    // (Unless otherwise specified, the type’s associated percent hint is null.)
    return value.visit(
        [](Number const&) {
            // -> <number>
            // -> <integer>
            //    the type is «[ ]» (empty map)
            return NumericType {};
        },
        [](Length const&) {
            // -> <length>
            //    the type is «[ "length" → 1 ]»
            return NumericType { NumericType::BaseType::Length, 1 };
        },
        [](Angle const&) {
            // -> <angle>
            //    the type is «[ "angle" → 1 ]»
            return NumericType { NumericType::BaseType::Angle, 1 };
        },
        [](Time const&) {
            // -> <time>
            //    the type is «[ "time" → 1 ]»
            return NumericType { NumericType::BaseType::Time, 1 };
        },
        [](Frequency const&) {
            // -> <frequency>
            //    the type is «[ "frequency" → 1 ]»
            return NumericType { NumericType::BaseType::Frequency, 1 };
        },
        [](Resolution const&) {
            // -> <resolution>
            //    the type is «[ "resolution" → 1 ]»
            return NumericType { NumericType::BaseType::Resolution, 1 };
        },
        [](Flex const&) {
            // -> <flex>
            //    the type is «[ "flex" → 1 ]»
            return NumericType { NumericType::BaseType::Flex, 1 };
        },
        // NOTE: <calc-constant> is a separate node type. (FIXME: Should it be?)
        [&context](Percentage const&) {
            // -> <percentage>
            //    If, in the context in which the math function containing this calculation is placed,
            //    <percentage>s are resolved relative to another type of value (such as in width,
            //    where <percentage> is resolved against a <length>), and that other type is not <number>,
            //    the type is determined as the other type, but with a percent hint set to that other type.
            if (context.percentages_resolve_as.has_value() && context.percentages_resolve_as != ValueType::Number && context.percentages_resolve_as != ValueType::Percentage) {
                auto base_type = NumericType::base_type_from_value_type(*context.percentages_resolve_as);
                VERIFY(base_type.has_value());
                auto result = NumericType { base_type.value(), 1 };
                result.set_percent_hint(base_type);
                return result;
            }

            //    Otherwise, the type is «[ "percent" → 1 ]», with a percent hint of "percent".
            auto result = NumericType { NumericType::BaseType::Percent, 1 };
            // FIXME: Setting the percent hint to "percent" causes us to fail tests.
            // result.set_percent_hint(NumericType::BaseType::Percent);
            return result;
        });
}

NonnullRefPtr<NumericCalculationNode const> NumericCalculationNode::create(NumericValue value, CalculationContext const& context)
{
    auto numeric_type = numeric_type_from_calculated_style_value(value, context);
    return adopt_ref(*new (nothrow) NumericCalculationNode(move(value), numeric_type));
}

NonnullRefPtr<ChannelKeywordCalculationNode const> ChannelKeywordCalculationNode::create(ChannelKeyword channel, CalculationContext const&)
{
    return adopt_ref(*new (nothrow) ChannelKeywordCalculationNode(channel));
}

ChannelKeywordCalculationNode::ChannelKeywordCalculationNode(ChannelKeyword channel)
    : CalculationNode(Type::ChannelKeyword, NumericType {})
    , m_channel(channel)
{
}

ChannelKeywordCalculationNode::~ChannelKeywordCalculationNode() = default;

void ChannelKeywordCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}CHANNEL-KEYWORD({})\n", "", indent, to_string(m_channel));
}

RefPtr<NumericCalculationNode const> NumericCalculationNode::from_keyword(Keyword keyword, CalculationContext const& context)
{
    switch (keyword) {
    case Keyword::E:
        // https://drafts.csswg.org/css-values-4/#valdef-calc-e
        return create(Number { Number::Type::Number, AK::E<double> }, context);
    case Keyword::Pi:
        // https://drafts.csswg.org/css-values-4/#valdef-calc-pi
        return create(Number { Number::Type::Number, AK::Pi<double> }, context);
    case Keyword::Infinity:
        // https://drafts.csswg.org/css-values-4/#valdef-calc-infinity
        return create(Number { Number::Type::Number, AK::Infinity<double> }, context);
    case Keyword::NegativeInfinity:
        // https://drafts.csswg.org/css-values-4/#valdef-calc--infinity
        return create(Number { Number::Type::Number, -AK::Infinity<double> }, context);
    case Keyword::Nan:
        // https://drafts.csswg.org/css-values-4/#valdef-calc-nan
        return create(Number { Number::Type::Number, AK::NaN<double> }, context);
    default:
        return nullptr;
    }
}

NumericCalculationNode::NumericCalculationNode(NumericValue value, NumericType numeric_type)
    : CalculationNode(Type::Numeric, move(numeric_type))
    , m_value(move(value))
{
}

NumericCalculationNode::~NumericCalculationNode() = default;

void NumericCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}NUMERIC({})\n", "", indent, m_value.visit([](auto& it) { return it.to_string(); }));
}

NonnullRefPtr<SumCalculationNode const> SumCalculationNode::create(Vector<NonnullRefPtr<CalculationNode const>> values)
{
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // At a + or - sub-expression, attempt to add the types of the left and right arguments.
    // If this returns failure, the entire calculation’s type is failure.
    // Otherwise, the sub-expression’s type is the returned type.
    auto numeric_type = add_the_types(values);
    return adopt_ref(*new (nothrow) SumCalculationNode(move(values), move(numeric_type)));
}

SumCalculationNode::SumCalculationNode(Vector<NonnullRefPtr<CalculationNode const>> values, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Sum, move(numeric_type))
    , m_values(move(values))
{
    VERIFY(!m_values.is_empty());
}

SumCalculationNode::~SumCalculationNode() = default;

void SumCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SUM:\n", "", indent);
    for (auto const& item : m_values)
        item->dump(builder, indent + 2);
}

NonnullRefPtr<ProductCalculationNode const> ProductCalculationNode::create(Vector<NonnullRefPtr<CalculationNode const>> values)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // At a * sub-expression, multiply the types of the left and right arguments.
    // The sub-expression’s type is the returned result.
    auto numeric_type = multiply_the_types(values);
    return adopt_ref(*new (nothrow) ProductCalculationNode(move(values), move(numeric_type)));
}

ProductCalculationNode::ProductCalculationNode(Vector<NonnullRefPtr<CalculationNode const>> values, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Product, move(numeric_type))
    , m_values(move(values))
{
    VERIFY(!m_values.is_empty());
}

ProductCalculationNode::~ProductCalculationNode() = default;

void ProductCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}PRODUCT:\n", "", indent);
    for (auto const& item : m_values)
        item->dump(builder, indent + 2);
}

NonnullRefPtr<ProgressCalculationNode const> ProgressCalculationNode::create(bool no_clamp, NonnullRefPtr<CalculationNode const> value, NonnullRefPtr<CalculationNode const> start_value, NonnullRefPtr<CalculationNode const> end_value)
{
    // https://drafts.csswg.org/css-values-5/#progress
    // The result of progress() is a <number> made consistent with the consistent type of its arguments
    auto numeric_type = NumericType {}.made_consistent_with(add_the_types({ value, start_value, end_value }).value()).value();

    return adopt_ref(*new (nothrow) ProgressCalculationNode(no_clamp, move(value), move(start_value), move(end_value), numeric_type));
}

ProgressCalculationNode::ProgressCalculationNode(bool no_clamp, NonnullRefPtr<CalculationNode const> value, NonnullRefPtr<CalculationNode const> start_value, NonnullRefPtr<CalculationNode const> end_value, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Progress, numeric_type)
    , m_no_clamp(no_clamp)
    , m_value(move(value))
    , m_start_value(move(start_value))
    , m_end_value(move(end_value))
{
}

ProgressCalculationNode::~ProgressCalculationNode() = default;

void ProgressCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}PROGRESS (no-clamp: {})\n", "", indent, m_no_clamp);
    m_value->dump(builder, indent + 2);
    m_start_value->dump(builder, indent + 2);
    m_end_value->dump(builder, indent + 2);
}

NonnullRefPtr<NegateCalculationNode const> NegateCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) NegateCalculationNode(move(value)));
}

NegateCalculationNode::NegateCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // NOTE: `- foo` doesn't change the type
    : CalculationNode(Type::Negate, value->numeric_type())
    , m_value(move(value))
{
}

NegateCalculationNode::~NegateCalculationNode() = default;

void NegateCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}NEGATE:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

NonnullRefPtr<InvertCalculationNode const> InvertCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // At a / sub-expression, let left type be the result of finding the types of its left argument,
    // and right type be the result of finding the types of its right argument and then inverting it.
    // The sub-expression’s type is the result of multiplying the left type and right type.
    // NOTE: An InvertCalculationNode only represents the right argument here, and the multiplication
    //       is handled in the parent ProductCalculationNode.
    auto numeric_type = value->numeric_type().map([](auto& it) { return it.inverted(); });
    return adopt_ref(*new (nothrow) InvertCalculationNode(move(value), move(numeric_type)));
}

InvertCalculationNode::InvertCalculationNode(NonnullRefPtr<CalculationNode const> value, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Invert, move(numeric_type))
    , m_value(move(value))
{
}

InvertCalculationNode::~InvertCalculationNode() = default;

void InvertCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}INVERT:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

NonnullRefPtr<MinCalculationNode const> MinCalculationNode::create(Vector<NonnullRefPtr<CalculationNode const>> values)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(values);
    return adopt_ref(*new (nothrow) MinCalculationNode(move(values), move(numeric_type)));
}

MinCalculationNode::MinCalculationNode(Vector<NonnullRefPtr<CalculationNode const>> values, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Min, move(numeric_type))
    , m_values(move(values))
{
}

MinCalculationNode::~MinCalculationNode() = default;

// https://drafts.csswg.org/css-values-4/#funcdef-min
void MinCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}MIN:\n", "", indent);
    for (auto const& value : m_values)
        value->dump(builder, indent + 2);
}

NonnullRefPtr<MaxCalculationNode const> MaxCalculationNode::create(Vector<NonnullRefPtr<CalculationNode const>> values)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(values);
    return adopt_ref(*new (nothrow) MaxCalculationNode(move(values), move(numeric_type)));
}

MaxCalculationNode::MaxCalculationNode(Vector<NonnullRefPtr<CalculationNode const>> values, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Max, move(numeric_type))
    , m_values(move(values))
{
}

MaxCalculationNode::~MaxCalculationNode() = default;

void MaxCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}MAX:\n", "", indent);
    for (auto const& value : m_values)
        value->dump(builder, indent + 2);
}

NonnullRefPtr<ClampCalculationNode const> ClampCalculationNode::create(NonnullRefPtr<CalculationNode const> min, NonnullRefPtr<CalculationNode const> center, NonnullRefPtr<CalculationNode const> max)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(*min, *center, *max);
    return adopt_ref(*new (nothrow) ClampCalculationNode(move(min), move(center), move(max), move(numeric_type)));
}

ClampCalculationNode::ClampCalculationNode(NonnullRefPtr<CalculationNode const> min, NonnullRefPtr<CalculationNode const> center, NonnullRefPtr<CalculationNode const> max, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Clamp, move(numeric_type))
    , m_min_value(move(min))
    , m_center_value(move(center))
    , m_max_value(move(max))
{
}

ClampCalculationNode::~ClampCalculationNode() = default;

void ClampCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}CLAMP:\n", "", indent);
    m_min_value->dump(builder, indent + 2);
    m_center_value->dump(builder, indent + 2);
    m_max_value->dump(builder, indent + 2);
}

NonnullRefPtr<AbsCalculationNode const> AbsCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) AbsCalculationNode(move(value)));
}

AbsCalculationNode::AbsCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // The type of its contained calculation.
    : CalculationNode(Type::Abs, value->numeric_type())
    , m_value(move(value))
{
}

AbsCalculationNode::~AbsCalculationNode() = default;

void AbsCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ABS:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

NonnullRefPtr<SignCalculationNode const> SignCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) SignCalculationNode(move(value)));
}

SignCalculationNode::SignCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Sign, NumericType {})
    , m_value(move(value))
{
}

SignCalculationNode::~SignCalculationNode() = default;

void SignCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SIGN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

NonnullRefPtr<SinCalculationNode const> SinCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) SinCalculationNode(move(value)));
}

SinCalculationNode::SinCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // «[ ]» (empty map).
    : CalculationNode(Type::Sin, NumericType {})
    , m_value(move(value))
{
}

SinCalculationNode::~SinCalculationNode() = default;

void SinCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SIN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

NonnullRefPtr<CosCalculationNode const> CosCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) CosCalculationNode(move(value)));
}

CosCalculationNode::CosCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Cos, NumericType {})
    , m_value(move(value))
{
}

CosCalculationNode::~CosCalculationNode() = default;

void CosCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}COS:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

NonnullRefPtr<TanCalculationNode const> TanCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) TanCalculationNode(move(value)));
}

TanCalculationNode::TanCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Tan, NumericType {})
    , m_value(move(value))
{
}

TanCalculationNode::~TanCalculationNode() = default;

void TanCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}TAN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

NonnullRefPtr<AsinCalculationNode const> AsinCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) AsinCalculationNode(move(value)));
}

AsinCalculationNode::AsinCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ "angle" → 1 ]».
    : CalculationNode(Type::Asin, NumericType { NumericType::BaseType::Angle, 1 })
    , m_value(move(value))
{
}

AsinCalculationNode::~AsinCalculationNode() = default;

void AsinCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ASIN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

NonnullRefPtr<AcosCalculationNode const> AcosCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) AcosCalculationNode(move(value)));
}

AcosCalculationNode::AcosCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ "angle" → 1 ]».
    : CalculationNode(Type::Acos, NumericType { NumericType::BaseType::Angle, 1 })
    , m_value(move(value))
{
}

AcosCalculationNode::~AcosCalculationNode() = default;

void AcosCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ACOS:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

NonnullRefPtr<AtanCalculationNode const> AtanCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) AtanCalculationNode(move(value)));
}

AtanCalculationNode::AtanCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ "angle" → 1 ]».
    : CalculationNode(Type::Atan, NumericType { NumericType::BaseType::Angle, 1 })
    , m_value(move(value))
{
}

AtanCalculationNode::~AtanCalculationNode() = default;

void AtanCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ATAN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

NonnullRefPtr<Atan2CalculationNode const> Atan2CalculationNode::create(NonnullRefPtr<CalculationNode const> y, NonnullRefPtr<CalculationNode const> x)
{
    return adopt_ref(*new (nothrow) Atan2CalculationNode(move(y), move(x)));
}

Atan2CalculationNode::Atan2CalculationNode(NonnullRefPtr<CalculationNode const> y, NonnullRefPtr<CalculationNode const> x)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ "angle" → 1 ]».
    : CalculationNode(Type::Atan2, NumericType { NumericType::BaseType::Angle, 1 })
    , m_y(move(y))
    , m_x(move(x))
{
}

Atan2CalculationNode::~Atan2CalculationNode() = default;

void Atan2CalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ATAN2:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

NonnullRefPtr<PowCalculationNode const> PowCalculationNode::create(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
{
    return adopt_ref(*new (nothrow) PowCalculationNode(move(x), move(y)));
}

PowCalculationNode::PowCalculationNode(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Pow, NumericType {})
    , m_x(move(x))
    , m_y(move(y))
{
}

PowCalculationNode::~PowCalculationNode() = default;

void PowCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}POW:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

NonnullRefPtr<SqrtCalculationNode const> SqrtCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) SqrtCalculationNode(move(value)));
}

SqrtCalculationNode::SqrtCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Sqrt, NumericType {})
    , m_value(move(value))
{
}

SqrtCalculationNode::~SqrtCalculationNode() = default;

void SqrtCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SQRT:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

NonnullRefPtr<HypotCalculationNode const> HypotCalculationNode::create(Vector<NonnullRefPtr<CalculationNode const>> values)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(values);
    return adopt_ref(*new (nothrow) HypotCalculationNode(move(values), move(numeric_type)));
}

HypotCalculationNode::HypotCalculationNode(Vector<NonnullRefPtr<CalculationNode const>> values, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Hypot, move(numeric_type))
    , m_values(move(values))
{
}

HypotCalculationNode::~HypotCalculationNode() = default;

void HypotCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}HYPOT:\n", "", indent);
    for (auto const& value : m_values)
        value->dump(builder, indent + 2);
}

NonnullRefPtr<LogCalculationNode const> LogCalculationNode::create(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
{
    return adopt_ref(*new (nothrow) LogCalculationNode(move(x), move(y)));
}

LogCalculationNode::LogCalculationNode(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Log, NumericType {})
    , m_x(move(x))
    , m_y(move(y))
{
}

LogCalculationNode::~LogCalculationNode() = default;

void LogCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}LOG:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

NonnullRefPtr<ExpCalculationNode const> ExpCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) ExpCalculationNode(move(value)));
}

ExpCalculationNode::ExpCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Exp, NumericType {})
    , m_value(move(value))
{
}

ExpCalculationNode::~ExpCalculationNode() = default;

void ExpCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}EXP:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

NonnullRefPtr<RoundCalculationNode const> RoundCalculationNode::create(RoundingStrategy strategy, NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
{
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(*x, *y);
    return adopt_ref(*new (nothrow) RoundCalculationNode(strategy, move(x), move(y), move(numeric_type)));
}

RoundCalculationNode::RoundCalculationNode(RoundingStrategy mode, NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Round, move(numeric_type))
    , m_strategy(mode)
    , m_x(move(x))
    , m_y(move(y))
{
}

RoundCalculationNode::~RoundCalculationNode() = default;

void RoundCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ROUND: {}\n", "", indent, CSS::to_string(m_strategy));
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

NonnullRefPtr<ModCalculationNode const> ModCalculationNode::create(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
{
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(*x, *y);
    return adopt_ref(*new (nothrow) ModCalculationNode(move(x), move(y), move(numeric_type)));
}

ModCalculationNode::ModCalculationNode(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Mod, move(numeric_type))
    , m_x(move(x))
    , m_y(move(y))
{
}

ModCalculationNode::~ModCalculationNode() = default;

void ModCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}MOD:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

NonnullRefPtr<RandomCalculationNode const> RandomCalculationNode::create(NonnullRefPtr<RandomValueSharingStyleValue const> random_value_sharing, NonnullRefPtr<CalculationNode const> minimum, NonnullRefPtr<CalculationNode const> maximum, RefPtr<CalculationNode const> step)
{
    Optional<NumericType> numeric_type;

    if (step)
        numeric_type = add_the_types(*minimum, *maximum, *step);
    else
        numeric_type = add_the_types(*minimum, *maximum);

    return adopt_ref(*new (nothrow) RandomCalculationNode(move(random_value_sharing), move(minimum), move(maximum), move(step), move(numeric_type)));
}

RandomCalculationNode::RandomCalculationNode(NonnullRefPtr<RandomValueSharingStyleValue const> random_value_sharing, NonnullRefPtr<CalculationNode const> minimum, NonnullRefPtr<CalculationNode const> maximum, RefPtr<CalculationNode const> step, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Random, move(numeric_type))
    , m_random_value_sharing(move(random_value_sharing))
    , m_minimum(move(minimum))
    , m_maximum(move(maximum))
    , m_step(move(step))
{
}

RandomCalculationNode::~RandomCalculationNode() = default;

void RandomCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}RANDOM:\n", "", indent);
    builder.appendff("{}\n", m_random_value_sharing->to_string(SerializationMode::Normal));
    m_minimum->dump(builder, indent + 2);
    m_maximum->dump(builder, indent + 2);
    if (m_step)
        m_step->dump(builder, indent + 2);
}

NonnullRefPtr<RemCalculationNode const> RemCalculationNode::create(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
{
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(*x, *y);
    return adopt_ref(*new (nothrow) RemCalculationNode(move(x), move(y), move(numeric_type)));
}

RemCalculationNode::RemCalculationNode(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Rem, move(numeric_type))
    , m_x(move(x))
    , m_y(move(y))
{
}

RemCalculationNode::~RemCalculationNode() = default;

void RemCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}REM:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

NonnullRefPtr<NonMathFunctionCalculationNode const> NonMathFunctionCalculationNode::create(AbstractNonMathCalcFunctionStyleValue const& function, NumericType numeric_type)
{
    return adopt_ref(*new (nothrow) NonMathFunctionCalculationNode(move(function), move(numeric_type)));
}

NonMathFunctionCalculationNode::NonMathFunctionCalculationNode(AbstractNonMathCalcFunctionStyleValue const& function, NumericType numeric_type)
    : CalculationNode(Type::NonMathFunction, numeric_type)
    , m_function(function)
{
}

NonMathFunctionCalculationNode::~NonMathFunctionCalculationNode() = default;

void NonMathFunctionCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}NON-MATH FUNCTION: {}", "", indent, m_function->to_string(SerializationMode::Normal));
}

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

// Rebuilds a C++ calculation tree from a Rust calculation node. Absolutized
// values must still carry a C++ tree for math-function composition through
// CalculationNode::from_style_value until the calculation storage drops it.
static NonnullRefPtr<CalculationNode const> cpp_calc_tree_from_rust(StyleValueFFI::CalcNode const* node, CalculationContext const& context)
{
    auto children_of = [&]() {
        Vector<StyleValueFFI::CalcNode const*> rust_children;
        auto count = StyleValueFFI::rust_calc_node_children(node, nullptr, 0);
        rust_children.resize(count);
        StyleValueFFI::rust_calc_node_children(node, rust_children.data(), rust_children.size());
        Vector<NonnullRefPtr<CalculationNode const>> children;
        children.ensure_capacity(count);
        for (auto const* child : rust_children)
            children.unchecked_append(cpp_calc_tree_from_rust(child, context));
        return children;
    };
    switch (StyleValueFFI::rust_calc_node_kind(node)) {
    case 0: {
        u8 kind = 0;
        double value = 0;
        u8 unit = 0;
        StyleValueFFI::rust_calc_node_numeric_leaf(node, &kind, &value, &unit);
        switch (kind) {
        case 0:
            return NumericCalculationNode::create(Number { static_cast<Number::Type>(unit), value }, context);
        case 1:
            return NumericCalculationNode::create(Angle { value, static_cast<AngleUnit>(unit) }, context);
        case 2:
            return NumericCalculationNode::create(Flex { value, static_cast<FlexUnit>(unit) }, context);
        case 3:
            return NumericCalculationNode::create(Frequency { value, static_cast<FrequencyUnit>(unit) }, context);
        case 4:
            return NumericCalculationNode::create(Length { value, static_cast<LengthUnit>(unit) }, context);
        case 5:
            return NumericCalculationNode::create(Percentage { value }, context);
        case 6:
            return NumericCalculationNode::create(Resolution { value, static_cast<ResolutionUnit>(unit) }, context);
        case 7:
            return NumericCalculationNode::create(Time { value, static_cast<TimeUnit>(unit) }, context);
        }
        VERIFY_NOT_REACHED();
    }
    case 1: {
        u8 channel = 0;
        VERIFY(StyleValueFFI::rust_calc_node_channel_keyword(node, &channel));
        return ChannelKeywordCalculationNode::create(static_cast<ChannelKeyword>(channel), context);
    }
    case 2:
        return SumCalculationNode::create(children_of());
    case 3:
        return ProductCalculationNode::create(children_of());
    case 4:
        return NegateCalculationNode::create(children_of()[0]);
    case 5:
        return InvertCalculationNode::create(children_of()[0]);
    case 6:
        return MinCalculationNode::create(children_of());
    case 7:
        return MaxCalculationNode::create(children_of());
    case 8: {
        auto children = children_of();
        return ClampCalculationNode::create(children[0], children[1], children[2]);
    }
    case 9: {
        auto children = children_of();
        return ProgressCalculationNode::create(StyleValueFFI::rust_calc_node_progress_no_clamp(node), children[0], children[1], children[2]);
    }
    case 10:
        return AbsCalculationNode::create(children_of()[0]);
    case 11:
        return SignCalculationNode::create(children_of()[0]);
    case 12:
        return SinCalculationNode::create(children_of()[0]);
    case 13:
        return CosCalculationNode::create(children_of()[0]);
    case 14:
        return TanCalculationNode::create(children_of()[0]);
    case 15:
        return AsinCalculationNode::create(children_of()[0]);
    case 16:
        return AcosCalculationNode::create(children_of()[0]);
    case 17:
        return AtanCalculationNode::create(children_of()[0]);
    case 18: {
        // NB: Atan2's children are ordered y then x, matching its members.
        auto children = children_of();
        return Atan2CalculationNode::create(children[0], children[1]);
    }
    case 19: {
        auto children = children_of();
        return PowCalculationNode::create(children[0], children[1]);
    }
    case 20:
        return SqrtCalculationNode::create(children_of()[0]);
    case 21:
        return HypotCalculationNode::create(children_of());
    case 22: {
        auto children = children_of();
        return LogCalculationNode::create(children[0], children[1]);
    }
    case 23:
        return ExpCalculationNode::create(children_of()[0]);
    case 24: {
        auto children = children_of();
        return RoundCalculationNode::create(static_cast<RoundingStrategy>(StyleValueFFI::rust_calc_node_round_strategy(node)), children[0], children[1]);
    }
    case 25: {
        auto children = children_of();
        return ModCalculationNode::create(children[0], children[1]);
    }
    case 26: {
        auto children = children_of();
        return RemCalculationNode::create(children[0], children[1]);
    }
    case 27: {
        auto children = children_of();
        auto const& sharing = static_cast<StyleValue const*>(StyleValueFFI::rust_calc_node_style_value(node))->as_random_value_sharing();
        return RandomCalculationNode::create(sharing, children[0], children[1], children.size() == 3 ? RefPtr<CalculationNode const> { children[2] } : nullptr);
    }
    case 28: {
        auto const& function = *static_cast<AbstractNonMathCalcFunctionStyleValue const*>(StyleValueFFI::rust_calc_node_style_value(node));
        auto numeric_type = from_ffi_numeric_type(StyleValueFFI::rust_calc_node_non_math_function_type(node));
        return NonMathFunctionCalculationNode::create(function, numeric_type.value());
    }
    }
    VERIFY_NOT_REACHED();
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

String CalculatedStyleValue::dump() const
{
    StringBuilder builder;
    cpp_calc_tree_from_rust(rust_calculation_root(), calculation_context())->dump(builder, 0);
    return builder.to_string_without_validation();
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

// https://drafts.csswg.org/css-values-4/#calc-simplification
NonnullRefPtr<CalculationNode const> simplify_a_calculation_tree(CalculationNode const& original_root, CalculationContext const& context, CalculationResolutionContext const& resolution_context)
{
    // The algorithm runs in the Rust calc core; this entry simplifies trees that are not (yet)
    // owned by a calculated style value, so it converts the tree, simplifies, and materializes
    // the result back into C++ nodes.
    CalcResolveCallbackContext callback_context { context, resolution_context };
    Optional<ComputedValuesFFI::FfiLengthResolutionContext> ffi_length_resolution_context;
    auto ffi_context = make_calc_ffi_resolution_context(callback_context, ffi_length_resolution_context);

    auto resolve_as_base = context.percentages_resolve_as.has_value()
        ? NumericType::base_type_from_value_type(*context.percentages_resolve_as)
        : OptionalNone {};

    auto const* rust_root = to_rust_calc_node(original_root);
    auto const* simplified = StyleValueFFI::rust_calc_simplify_tree(
        rust_root,
        &ffi_context,
        context.percentages_resolve_as.has_value(),
        context.percentages_resolve_as == ValueType::Number,
        resolve_as_base.has_value() ? to_underlying(*resolve_as_base) : 0);
    auto result = cpp_calc_tree_from_rust(simplified, context);
    StyleValueFFI::rust_calc_node_release(simplified);
    StyleValueFFI::rust_calc_node_release(rust_root);
    return result;
}

}
