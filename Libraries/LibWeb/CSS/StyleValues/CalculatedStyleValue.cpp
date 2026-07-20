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
    // The Rust allocation takes ownership of one strong reference to the calculation node.
    calculation->ref();
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
        to_rust_calc_node(*calculation),
        calculation.ptr(),
        resolved_type_bytes.data(), resolved_type_bytes.size(),
        context.percentages_resolve_as.has_value(),
        context.percentages_resolve_as == ValueType::Number,
        resolve_as_base.has_value() ? to_underlying(*resolve_as_base) : 0,
        context.percentages_resolve_as.has_value() ? to_underlying(*context.percentages_resolve_as) : 0,
        context.resolve_numbers_as_integers,
        ranges.data(), ranges.size());
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

template<typename T>
static NonnullRefPtr<CalculationNode const> simplify_children_vector(T const& original, CalculationContext const& context, CalculationResolutionContext const& resolution_context)
{
    Vector<NonnullRefPtr<CalculationNode const>> simplified_children;
    simplified_children.ensure_capacity(original.children().size());

    bool any_changed = false;
    for (auto const& child : original.children()) {
        auto simplified = simplify_a_calculation_tree(child, context, resolution_context);
        if (simplified != child)
            any_changed = true;
        simplified_children.append(move(simplified));
    }

    if (any_changed)
        return T::create(move(simplified_children));
    return original;
}

template<typename T>
static NonnullRefPtr<CalculationNode const> simplify_child(T const& original, NonnullRefPtr<CalculationNode const> const& child, CalculationContext const& context, CalculationResolutionContext const& resolution_context)
{
    auto simplified = simplify_a_calculation_tree(child, context, resolution_context);
    if (simplified != child)
        return T::create(move(simplified));
    return original;
}

template<typename T>
static NonnullRefPtr<CalculationNode const> simplify_2_children(T const& original, NonnullRefPtr<CalculationNode const> const& child_1, NonnullRefPtr<CalculationNode const> const& child_2, CalculationContext const& context, CalculationResolutionContext const& resolution_context)
{
    auto simplified_1 = simplify_a_calculation_tree(child_1, context, resolution_context);
    auto simplified_2 = simplify_a_calculation_tree(child_2, context, resolution_context);
    if (simplified_1 != child_1 || simplified_2 != child_2)
        return T::create(move(simplified_1), move(simplified_2));
    return original;
}

static GC::Ptr<CSSNumericArray> reify_children(JS::Realm& realm, ReadonlySpan<NonnullRefPtr<CalculationNode const>> children)
{
    GC::RootVector<GC::Ref<CSSNumericValue>> reified_children;
    for (auto const& child : children) {
        auto reified_child = child->reify(realm);
        if (!reified_child)
            return nullptr;
        reified_children.append(reified_child.as_nonnull());
    }
    return CSSNumericArray::create(realm, move(reified_children));
}

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
    case StyleValue::Type::Calculated:
        return style_value->as_calculated().calculation();
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

StringView CalculationNode::name() const
{
    switch (m_type) {
    case Type::Min:
        return "min"sv;
    case Type::Max:
        return "max"sv;
    case Type::Clamp:
        return "clamp"sv;
    case Type::Abs:
        return "abs"sv;
    case Type::Sign:
        return "sign"sv;
    case Type::Sin:
        return "sin"sv;
    case Type::Cos:
        return "cos"sv;
    case Type::Tan:
        return "tan"sv;
    case Type::Asin:
        return "asin"sv;
    case Type::Acos:
        return "acos"sv;
    case Type::Atan:
        return "atan"sv;
    case Type::Atan2:
        return "atan2"sv;
    case Type::Pow:
        return "pow"sv;
    case Type::Progress:
        return "progress"sv;
    case Type::Sqrt:
        return "sqrt"sv;
    case Type::Hypot:
        return "hypot"sv;
    case Type::Log:
        return "log"sv;
    case Type::Exp:
        return "exp"sv;
    case Type::Random:
        return "random"sv;
    case Type::Round:
        return "round"sv;
    case Type::Mod:
        return "mod"sv;
    case Type::Rem:
        return "rem"sv;
    case Type::Numeric:
    case Type::ChannelKeyword:
    case Type::Sum:
    case Type::Product:
    case Type::Negate:
    case Type::Invert:
    case Type::NonMathFunction:
        return "calc"sv;
    }
    VERIFY_NOT_REACHED();
}

static NumericType numeric_type_from_calculated_style_value(CalculatedStyleValue::CalculationResult::Value const& value, CalculationContext const& context)
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

bool ChannelKeywordCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_channel == static_cast<ChannelKeywordCalculationNode const&>(other).m_channel;
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

void NumericCalculationNode::serialize_value(StringBuilder& builder) const
{
    m_value.visit([&](auto& value) { value.serialize(builder); });
}

String NumericCalculationNode::value_to_string() const
{
    StringBuilder builder;
    serialize_value(builder);
    return builder.to_string_without_validation();
}

bool NumericCalculationNode::contains_percentage() const
{
    return m_value.has<Percentage>();
}

bool NumericCalculationNode::is_in_canonical_unit() const
{
    return m_value.visit(
        [](Angle const& angle) { return angle.unit() == AngleUnit::Deg; },
        [](Flex const& flex) { return flex.unit() == FlexUnit::Fr; },
        [](Frequency const& frequency) { return frequency.unit() == FrequencyUnit::Hz; },
        [](Length const& length) { return length.unit() == LengthUnit::Px; },
        [](Number const&) { return true; },
        [](Percentage const&) { return true; },
        [](Resolution const& resolution) { return resolution.unit() == ResolutionUnit::Dppx; },
        [](Time const& time) { return time.unit() == TimeUnit::S; });
}

static Optional<CalculatedStyleValue::CalculationResult> try_get_value_with_canonical_unit(CalculationNode const& child, CalculationContext const& context, CalculationResolutionContext const& resolution_context)
{
    if (child.type() != CalculationNode::Type::Numeric)
        return {};
    auto const& numeric_child = as<NumericCalculationNode>(child);

    // Can't run with non-canonical units or unresolved percentages.
    // We've already attempted to resolve both in with_simplified_children().
    if (!numeric_child.is_in_canonical_unit()
        || (numeric_child.value().has<Percentage>() && context.percentages_resolve_as.has_value()))
        return {};

    // Can't run if a child has an invalid type.
    if (!numeric_child.numeric_type().has_value())
        return {};

    return CalculatedStyleValue::CalculationResult::from_value(numeric_child.value(), resolution_context, numeric_child.numeric_type());
}

static Optional<double> try_get_number(CalculationNode const& child)
{
    if (child.type() != CalculationNode::Type::Numeric)
        return {};
    auto const* maybe_number = as<NumericCalculationNode>(child).value().get_pointer<Number>();
    if (!maybe_number)
        return {};
    return maybe_number->value();
}

Optional<NonFiniteValue> NumericCalculationNode::infinite_or_nan_value() const
{
    auto raw_value = m_value.visit(
        [](Number const& number) { return number.value(); },
        [](Percentage const& percentage) { return percentage.as_fraction(); },
        [](auto const& dimension) { return dimension.raw_value(); });

    if (isnan(raw_value))
        return NonFiniteValue::NaN;
    if (!isfinite(raw_value)) {
        if (raw_value < 0)
            return NonFiniteValue::NegativeInfinity;
        return NonFiniteValue::Infinity;
    }

    return {};
}

bool NumericCalculationNode::is_negative() const
{
    return m_value.visit(
        [&](Number const& number) { return number.value() < 0; },
        [](Percentage const& percentage) { return percentage.value() < 0; },
        [](auto const& dimension) { return dimension.raw_value() < 0; });
}

NonnullRefPtr<NumericCalculationNode const> NumericCalculationNode::negated(CalculationContext const& context) const
{
    return value().visit(
        [&](Percentage const& percentage) {
            return create(Percentage(-percentage.value()), context);
        },
        [&](Number const& number) {
            return create(Number(number.type(), -number.value()), context);
        },
        [&]<typename T>(T const& value) {
            return create(T(-value.raw_value(), value.unit()), context);
        });
}

void NumericCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}NUMERIC({})\n", "", indent, m_value.visit([](auto& it) { return it.to_string(); }));
}

bool NumericCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value == static_cast<NumericCalculationNode const&>(other).m_value;
}

GC::Ptr<CSSNumericValue> NumericCalculationNode::reify(JS::Realm& realm) const
{
    return m_value.visit(
        [&realm](Number const& number) { return CSSUnitValue::create(realm, number.value(), "number"_utf16_fly_string); },
        [&realm](Percentage const& percentage) { return CSSUnitValue::create(realm, percentage.value(), "percent"_utf16_fly_string); },
        [&realm](auto const& dimension) { return CSSUnitValue::create(realm, dimension.raw_value(), dimension.unit_name()); });
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

bool SumCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }
    return false;
}

NonnullRefPtr<CalculationNode const> SumCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_children_vector(*this, context, resolution_context);
}

void SumCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SUM:\n", "", indent);
    for (auto const& item : m_values)
        item->dump(builder, indent + 2);
}

bool SumCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    if (m_values.size() != static_cast<SumCalculationNode const&>(other).m_values.size())
        return false;
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (!m_values[i]->equals(*static_cast<SumCalculationNode const&>(other).m_values[i]))
            return false;
    }
    return true;
}

GC::Ptr<CSSNumericValue> SumCalculationNode::reify(JS::Realm& realm) const
{
    auto reified_children = reify_children(realm, m_values);
    if (!reified_children)
        return nullptr;
    return CSSMathSum::create(realm, numeric_type().value(), reified_children.as_nonnull());
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

bool ProductCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }
    return false;
}

NonnullRefPtr<CalculationNode const> ProductCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_children_vector(*this, context, resolution_context);
}

void ProductCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}PRODUCT:\n", "", indent);
    for (auto const& item : m_values)
        item->dump(builder, indent + 2);
}

bool ProductCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    if (m_values.size() != static_cast<ProductCalculationNode const&>(other).m_values.size())
        return false;
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (!m_values[i]->equals(*static_cast<ProductCalculationNode const&>(other).m_values[i]))
            return false;
    }
    return true;
}

GC::Ptr<CSSNumericValue> ProductCalculationNode::reify(JS::Realm& realm) const
{
    auto reified_children = reify_children(realm, m_values);
    if (!reified_children)
        return nullptr;
    return CSSMathProduct::create(realm, numeric_type().value(), reified_children.as_nonnull());
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

bool ProgressCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage()
        || m_start_value->contains_percentage()
        || m_end_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> ProgressCalculationNode::with_simplified_children(CalculationContext const& calculation_context, CalculationResolutionContext const& calculation_resolution_context) const
{
    auto simplified_value = simplify_a_calculation_tree(m_value, calculation_context, calculation_resolution_context);
    auto simplified_start_value = simplify_a_calculation_tree(m_start_value, calculation_context, calculation_resolution_context);
    auto simplified_end_value = simplify_a_calculation_tree(m_end_value, calculation_context, calculation_resolution_context);

    if (simplified_value == m_value && simplified_start_value == m_start_value && simplified_end_value == m_end_value)
        return *this;

    return create(m_no_clamp, move(simplified_value), move(simplified_start_value), move(simplified_end_value));
}

Optional<CalculatedStyleValue::CalculationResult> ProgressCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    auto maybe_value = try_get_value_with_canonical_unit(*m_value, context, resolution_context);
    auto maybe_start_value = try_get_value_with_canonical_unit(*m_start_value, context, resolution_context);
    auto maybe_end_value = try_get_value_with_canonical_unit(*m_end_value, context, resolution_context);

    if (!maybe_value.has_value() || !maybe_start_value.has_value() || !maybe_end_value.has_value())
        return {};

    // https://drafts.csswg.org/css-values-5/#calculate-a-progress-function
    // If the progress start value and progress end value are different values
    if (maybe_start_value != maybe_end_value) {
        // (progress value - progress start value) / (progress end value - progress start value), clamped to the [0,1] range if no-clamp is not specified.
        auto progress = (maybe_value->value() - maybe_start_value->value()) / (maybe_end_value->value() - maybe_start_value->value());

        if (!m_no_clamp)
            progress = clamp(progress, 0.0, 1.0);

        return CalculatedStyleValue::CalculationResult { progress, numeric_type() };
    }

    // If the progress start value and progress end value are the same value
    {
        // 0 if no-clamp is not specified.
        if (!m_no_clamp)
            return CalculatedStyleValue::CalculationResult { 0.0, numeric_type() };

        // Otherwise, 0, -∞, or +∞, depending on whether progress value is equal to, less than, or greater than the shared value.
        if (maybe_value->value() == maybe_start_value->value())
            return CalculatedStyleValue::CalculationResult { 0.0, numeric_type() };
        if (maybe_value->value() < maybe_start_value->value())
            return CalculatedStyleValue::CalculationResult { -AK::Infinity<double>, numeric_type() };

        return CalculatedStyleValue::CalculationResult { AK::Infinity<double>, numeric_type() };
    }
}

void ProgressCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}PROGRESS (no-clamp: {})\n", "", indent, m_no_clamp);
    m_value->dump(builder, indent + 2);
    m_start_value->dump(builder, indent + 2);
    m_end_value->dump(builder, indent + 2);
}

bool ProgressCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;

    if (type() != other.type())
        return false;

    auto const& other_progress = static_cast<ProgressCalculationNode const&>(other);

    return m_no_clamp == other_progress.m_no_clamp
        && m_value->equals(other_progress.m_value)
        && m_start_value->equals(other_progress.m_start_value)
        && m_end_value->equals(other_progress.m_end_value);
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

bool NegateCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> NegateCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

void NegateCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}NEGATE:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool NegateCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<NegateCalculationNode const&>(other).m_value);
}

GC::Ptr<CSSNumericValue> NegateCalculationNode::reify(JS::Realm& realm) const
{
    auto reified_value = m_value->reify(realm);
    if (!reified_value)
        return nullptr;
    return CSSMathNegate::create(realm, numeric_type().value(), reified_value.as_nonnull());
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

bool InvertCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> InvertCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

void InvertCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}INVERT:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool InvertCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<InvertCalculationNode const&>(other).m_value);
}

GC::Ptr<CSSNumericValue> InvertCalculationNode::reify(JS::Realm& realm) const
{
    auto reified_value = m_value->reify(realm);
    if (!reified_value)
        return nullptr;
    return CSSMathInvert::create(realm, numeric_type().value(), reified_value.as_nonnull());
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

bool MinCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }

    return false;
}

NonnullRefPtr<CalculationNode const> MinCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_children_vector(*this, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-min
enum class MinOrMax {
    Min,
    Max,
};
static Optional<CalculatedStyleValue::CalculationResult> run_min_or_max_operation_if_possible(Vector<NonnullRefPtr<CalculationNode const>> const& children, CalculationContext const& context, CalculationResolutionContext const& resolution_context, MinOrMax min_or_max)
{
    // The min() or max() functions contain one or more comma-separated calculations, and represent the smallest
    // (most negative) or largest (most positive) of them, respectively.
    Optional<CalculatedStyleValue::CalculationResult> result;
    for (auto const& child : children) {
        auto child_value = try_get_value_with_canonical_unit(child, context, resolution_context);
        if (!child_value.has_value())
            return {};

        if (!result.has_value()) {
            result = child_value.release_value();
        } else {
            auto consistent_type = result->type()->consistent_type(child_value->type().value());
            if (!consistent_type.has_value())
                return {};

            // https://drafts.csswg.org/css-values-4/#calc-ieee
            // Any operation with at least one NaN argument produces NaN.
            if (isnan(child_value->value()) || isnan(result->value())) {
                result = CalculatedStyleValue::CalculationResult { AK::NaN<double>, consistent_type };
                continue;
            }

            if (min_or_max == MinOrMax::Min) {
                if (child_value->value() < result->value()) {
                    result = CalculatedStyleValue::CalculationResult { child_value->value(), consistent_type };
                } else {
                    result = CalculatedStyleValue::CalculationResult { result->value(), consistent_type };
                }
            } else {
                if (child_value->value() > result->value()) {
                    result = CalculatedStyleValue::CalculationResult { child_value->value(), consistent_type };
                } else {
                    result = CalculatedStyleValue::CalculationResult { result->value(), consistent_type };
                }
            }
        }
    }
    return result;
}

// https://drafts.csswg.org/css-values-4/#funcdef-min
Optional<CalculatedStyleValue::CalculationResult> MinCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return run_min_or_max_operation_if_possible(m_values, context, resolution_context, MinOrMax::Min);
}

void MinCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}MIN:\n", "", indent);
    for (auto const& value : m_values)
        value->dump(builder, indent + 2);
}

bool MinCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    if (m_values.size() != static_cast<MinCalculationNode const&>(other).m_values.size())
        return false;
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (!m_values[i]->equals(*static_cast<MinCalculationNode const&>(other).m_values[i]))
            return false;
    }
    return true;
}

GC::Ptr<CSSNumericValue> MinCalculationNode::reify(JS::Realm& realm) const
{
    auto reified_children = reify_children(realm, m_values);
    if (!reified_children)
        return nullptr;
    return CSSMathMin::create(realm, numeric_type().value(), reified_children.as_nonnull());
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

bool MaxCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }

    return false;
}

NonnullRefPtr<CalculationNode const> MaxCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_children_vector(*this, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-max
Optional<CalculatedStyleValue::CalculationResult> MaxCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return run_min_or_max_operation_if_possible(m_values, context, resolution_context, MinOrMax::Max);
}

void MaxCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}MAX:\n", "", indent);
    for (auto const& value : m_values)
        value->dump(builder, indent + 2);
}

bool MaxCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    if (m_values.size() != static_cast<MaxCalculationNode const&>(other).m_values.size())
        return false;
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (!m_values[i]->equals(*static_cast<MaxCalculationNode const&>(other).m_values[i]))
            return false;
    }
    return true;
}

GC::Ptr<CSSNumericValue> MaxCalculationNode::reify(JS::Realm& realm) const
{
    auto reified_children = reify_children(realm, m_values);
    if (!reified_children)
        return nullptr;
    return CSSMathMax::create(realm, numeric_type().value(), reified_children.as_nonnull());
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

bool ClampCalculationNode::contains_percentage() const
{
    return m_min_value->contains_percentage() || m_center_value->contains_percentage() || m_max_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> ClampCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    auto simplified_min = simplify_a_calculation_tree(m_min_value, context, resolution_context);
    auto simplified_center = simplify_a_calculation_tree(m_center_value, context, resolution_context);
    auto simplified_max = simplify_a_calculation_tree(m_max_value, context, resolution_context);
    if (simplified_min != m_min_value || simplified_center != m_center_value || simplified_max != m_max_value)
        return create(move(simplified_min), move(simplified_center), move(simplified_max));
    return *this;
}

// https://drafts.csswg.org/css-values-4/#funcdef-clamp
Optional<CalculatedStyleValue::CalculationResult> ClampCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    // The clamp() function takes three calculations — a minimum value, a central value, and a maximum value — and
    // represents its central calculation, clamped according to its min and max calculations, favoring the min
    // calculation if it conflicts with the max. (That is, given clamp(MIN, VAL, MAX), it represents exactly the
    // same value as max(MIN, min(VAL, MAX))).
    //
    // Either the min or max calculations (or even both) can instead be the keyword none, which indicates the value
    // is not clamped from that side. (That is, clamp(MIN, VAL, none) is equivalent to max(MIN, VAL), clamp(none,
    // VAL, MAX) is equivalent to min(VAL, MAX), and clamp(none, VAL, none) is equivalent to just calc(VAL).)
    //
    // For all three functions, the argument calculations can resolve to any <number>, <dimension>, or <percentage>,
    // but must have a consistent type or else the function is invalid; the result’s type will be the consistent type.

    auto min_result = try_get_value_with_canonical_unit(m_min_value, context, resolution_context);
    if (!min_result.has_value())
        return {};

    auto center_result = try_get_value_with_canonical_unit(m_center_value, context, resolution_context);
    if (!center_result.has_value())
        return {};

    auto max_result = try_get_value_with_canonical_unit(m_max_value, context, resolution_context);
    if (!max_result.has_value())
        return {};

    auto consistent_type = min_result->type()->consistent_type(center_result->type().value()).map([&](auto&& it) { return it.consistent_type(max_result->type().value()); });
    if (!consistent_type.has_value())
        return {};

    // https://drafts.csswg.org/css-values-4/#calc-ieee
    // Any operation with at least one NaN argument produces NaN.
    if (isnan(min_result->value()) || isnan(center_result->value()) || isnan(max_result->value()))
        return CalculatedStyleValue::CalculationResult { AK::NaN<double>, consistent_type.release_value() };

    auto chosen_value = max(min_result->value(), min(center_result->value(), max_result->value()));
    return CalculatedStyleValue::CalculationResult { chosen_value, consistent_type.release_value() };
}

void ClampCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}CLAMP:\n", "", indent);
    m_min_value->dump(builder, indent + 2);
    m_center_value->dump(builder, indent + 2);
    m_max_value->dump(builder, indent + 2);
}

bool ClampCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_min_value->equals(*static_cast<ClampCalculationNode const&>(other).m_min_value)
        && m_center_value->equals(*static_cast<ClampCalculationNode const&>(other).m_center_value)
        && m_max_value->equals(*static_cast<ClampCalculationNode const&>(other).m_max_value);
}

GC::Ptr<CSSNumericValue> ClampCalculationNode::reify(JS::Realm& realm) const
{
    auto lower = m_min_value->reify(realm);
    auto value = m_center_value->reify(realm);
    auto upper = m_max_value->reify(realm);
    if (!lower || !value || !upper)
        return nullptr;

    return CSSMathClamp::create(realm, numeric_type().value(), lower.as_nonnull(), value.as_nonnull(), upper.as_nonnull());
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

bool AbsCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> AbsCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-abs
Optional<CalculatedStyleValue::CalculationResult> AbsCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    // The abs(A) function contains one calculation A, and returns the absolute value of A, as the same type as the input:
    // if A’s numeric value is positive or 0⁺, just A again; otherwise -1 * A.
    auto child_value = try_get_value_with_canonical_unit(m_value, context, resolution_context);
    if (!child_value.has_value())
        return {};
    return CalculatedStyleValue::CalculationResult { fabs(child_value->value()), child_value->type() };
}

void AbsCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ABS:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool AbsCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<AbsCalculationNode const&>(other).m_value);
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

bool SignCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> SignCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-sign
Optional<CalculatedStyleValue::CalculationResult> SignCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    // The sign(A) function contains one calculation A, and returns -1 if A’s numeric value is negative,
    // +1 if A’s numeric value is positive, 0⁺ if A’s numeric value is 0⁺, and 0⁻ if A’s numeric value is 0⁻.
    // The return type is a <number>, made consistent with the input calculation’s type.

    if (m_value->type() != CalculationNode::Type::Numeric)
        return {};
    auto const& numeric_child = as<NumericCalculationNode>(*m_value);
    double raw_value = numeric_child.value().visit(
        [](Number const& number) { return number.value(); },
        [](Percentage const& percentage) { return percentage.as_fraction(); },
        [](auto const& dimension) { return dimension.raw_value(); });

    auto return_type = NumericType {}.made_consistent_with(numeric_child.numeric_type().value_or({}));

    // https://drafts.csswg.org/css-values-4/#calc-ieee
    // Any operation with at least one NaN argument produces NaN.
    if (isnan(raw_value))
        return CalculatedStyleValue::CalculationResult { AK::NaN<double>, return_type };

    double sign = 0;
    if (raw_value < 0) {
        sign = -1;
    } else if (raw_value > 0) {
        sign = 1;
    } else {
        FloatExtractor<double> const extractor { .d = raw_value };
        sign = extractor.sign ? -0.0 : 0.0;
    }

    return CalculatedStyleValue::CalculationResult { sign, return_type };
}

void SignCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SIGN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool SignCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<SignCalculationNode const&>(other).m_value);
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

bool SinCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> SinCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

enum class SinCosOrTan {
    Sin,
    Cos,
    Tan,
};
static Optional<CalculatedStyleValue::CalculationResult> run_sin_cos_or_tan_operation_if_possible(CalculationNode const& child, SinCosOrTan trig_function)
{
    // The sin(A), cos(A), and tan(A) functions all contain a single calculation which must resolve to either a <number>
    // or an <angle>, and compute their corresponding function by interpreting the result of their argument as radians.
    // (That is, sin(45deg), sin(.125turn), and sin(3.14159 / 4) all represent the same value, approximately .707.) They
    // all represent a <number>, with the return type made consistent with the input calculation’s type. sin() and cos()
    // will always return a number between −1 and 1, while tan() can return any number between −∞ and +∞.
    // (See § 10.9 Type Checking for details on how math functions handle ∞.)

    if (child.type() != CalculationNode::Type::Numeric)
        return {};
    auto const& numeric_child = as<NumericCalculationNode>(child);

    auto radians = numeric_child.value().visit(
        [](Angle const& angle) { return angle.to_radians(); },
        [](Number const& number) { return number.value(); },
        [](auto const&) -> double { VERIFY_NOT_REACHED(); });

    double result = 0;
    switch (trig_function) {
    case SinCosOrTan::Sin:
        result = sin(radians);
        break;
    case SinCosOrTan::Cos:
        result = cos(radians);
        break;
    case SinCosOrTan::Tan:
        result = tan(radians);
        break;
    }

    return CalculatedStyleValue::CalculationResult { result, NumericType {}.made_consistent_with(child.numeric_type().value()) };
}

// https://drafts.csswg.org/css-values-4/#funcdef-sin
Optional<CalculatedStyleValue::CalculationResult> SinCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    return run_sin_cos_or_tan_operation_if_possible(m_value, SinCosOrTan::Sin);
}

void SinCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SIN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool SinCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<SinCalculationNode const&>(other).m_value);
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

bool CosCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> CosCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-cos
Optional<CalculatedStyleValue::CalculationResult> CosCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    return run_sin_cos_or_tan_operation_if_possible(m_value, SinCosOrTan::Cos);
}

void CosCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}COS:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool CosCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<CosCalculationNode const&>(other).m_value);
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

bool TanCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> TanCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-tan
Optional<CalculatedStyleValue::CalculationResult> TanCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    return run_sin_cos_or_tan_operation_if_possible(m_value, SinCosOrTan::Tan);
}

void TanCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}TAN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool TanCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<TanCalculationNode const&>(other).m_value);
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

bool AsinCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> AsinCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

enum class AsinAcosOrAtan {
    Asin,
    Acos,
    Atan,
};
static Optional<CalculatedStyleValue::CalculationResult> run_asin_acos_or_atan_operation_if_possible(CalculationNode const& child, AsinAcosOrAtan trig_function)
{
    // The asin(A), acos(A), and atan(A) functions are the "arc" or "inverse" trigonometric functions, representing
    // the inverse function to their corresponding "normal" trig functions. All of them contain a single calculation
    // which must resolve to a <number>, and compute their corresponding function, interpreting their result as a
    // number of radians, representing an <angle> with the return type made consistent with the input calculation’s
    // type. The angle returned by asin() must be normalized to the range [-90deg, 90deg]; the angle returned by acos()
    // to the range [0deg, 180deg]; and the angle returned by atan() to the range [-90deg, 90deg].

    auto maybe_number = try_get_number(child);
    if (!maybe_number.has_value())
        return {};
    auto number = maybe_number.release_value();

    auto normalize_angle = [](double radians, double min_degrees, double max_degrees) -> double {
        auto degrees = AK::to_degrees(radians);
        while (degrees < min_degrees)
            degrees += 360;
        while (degrees > max_degrees)
            degrees -= 360;
        return degrees;
    };

    double result = 0;
    switch (trig_function) {
    case AsinAcosOrAtan::Asin:
        result = normalize_angle(asin(number), -90, 90);
        break;
    case AsinAcosOrAtan::Acos:
        result = normalize_angle(acos(number), 0, 180);
        break;
    case AsinAcosOrAtan::Atan:
        result = normalize_angle(atan(number), -90, 90);
        break;
    }

    return CalculatedStyleValue::CalculationResult { result, NumericType { NumericType::BaseType::Angle, 1 }.made_consistent_with(child.numeric_type().value()) };
}

// https://drafts.csswg.org/css-values-4/#funcdef-asin
Optional<CalculatedStyleValue::CalculationResult> AsinCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    return run_asin_acos_or_atan_operation_if_possible(m_value, AsinAcosOrAtan::Asin);
}

void AsinCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ASIN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool AsinCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<AsinCalculationNode const&>(other).m_value);
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

bool AcosCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> AcosCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-acos
Optional<CalculatedStyleValue::CalculationResult> AcosCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    return run_asin_acos_or_atan_operation_if_possible(m_value, AsinAcosOrAtan::Acos);
}

void AcosCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ACOS:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool AcosCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<AcosCalculationNode const&>(other).m_value);
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

bool AtanCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> AtanCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-atan
Optional<CalculatedStyleValue::CalculationResult> AtanCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    return run_asin_acos_or_atan_operation_if_possible(m_value, AsinAcosOrAtan::Atan);
}

void AtanCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ATAN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool AtanCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<AtanCalculationNode const&>(other).m_value);
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

bool Atan2CalculationNode::contains_percentage() const
{
    return m_y->contains_percentage() || m_x->contains_percentage();
}

NonnullRefPtr<CalculationNode const> Atan2CalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_2_children(*this, m_y, m_x, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-atan2
Optional<CalculatedStyleValue::CalculationResult> Atan2CalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    // The atan2(A, B) function contains two comma-separated calculations, A and B. A and B can resolve to any <number>,
    // <dimension>, or <percentage>, but must have a consistent type or else the function is invalid. The function
    // returns the <angle> between the positive X-axis and the point (B,A), with the return type made consistent with the
    // input calculation’s type. The returned angle must be normalized to the interval (-180deg, 180deg] (that is,
    // greater than -180deg, and less than or equal to 180deg).
    auto x_value = try_get_value_with_canonical_unit(m_x, context, resolution_context);
    if (!x_value.has_value())
        return {};
    auto y_value = try_get_value_with_canonical_unit(m_y, context, resolution_context);
    if (!y_value.has_value())
        return {};

    auto input_consistent_type = x_value->type()->consistent_type(y_value->type().value());
    if (!input_consistent_type.has_value())
        return {};

    auto degrees = AK::to_degrees(atan2(y_value->value(), x_value->value()));
    while (degrees <= -180)
        degrees += 360;
    while (degrees > 180)
        degrees -= 360;

    return CalculatedStyleValue::CalculationResult { degrees, NumericType { NumericType::BaseType::Angle, 1 }.made_consistent_with(*input_consistent_type) };
}

void Atan2CalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ATAN2:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

bool Atan2CalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_x->equals(*static_cast<Atan2CalculationNode const&>(other).m_x)
        && m_y->equals(*static_cast<Atan2CalculationNode const&>(other).m_y);
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

NonnullRefPtr<CalculationNode const> PowCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_2_children(*this, m_x, m_y, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-pow
Optional<CalculatedStyleValue::CalculationResult> PowCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    // The pow(A, B) function contains two comma-separated calculations A and B, both of which must resolve to <number>s,
    // and returns the result of raising A to the power of B, returning the value as a <number>. The input calculations
    // must have a consistent type or else the function is invalid; the result’s type will be the consistent type.
    auto a = try_get_number(m_x);
    auto b = try_get_number(m_y);
    if (!a.has_value() || !b.has_value())
        return {};

    auto consistent_type = m_x->numeric_type()->consistent_type(m_y->numeric_type().value());
    if (!consistent_type.has_value())
        return {};

    return CalculatedStyleValue::CalculationResult { pow(*a, *b), consistent_type };
}

void PowCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}POW:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

bool PowCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_x->equals(*static_cast<PowCalculationNode const&>(other).m_x)
        && m_y->equals(*static_cast<PowCalculationNode const&>(other).m_y);
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

NonnullRefPtr<CalculationNode const> SqrtCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-sqrt
Optional<CalculatedStyleValue::CalculationResult> SqrtCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    // The sqrt(A) function contains a single calculation which must resolve to a <number>, and returns the square root
    // of the value as a <number>, with the return type made consistent with the input calculation’s type.
    // (sqrt(X) and pow(X, .5) are basically equivalent, differing only in some error-handling; sqrt() is a common enough
    // function that it is provided as a convenience.)
    auto number = try_get_number(m_value);
    if (!number.has_value())
        return {};

    auto consistent_type = NumericType {}.made_consistent_with(m_value->numeric_type().value());
    if (!consistent_type.has_value())
        return {};

    return CalculatedStyleValue::CalculationResult { sqrt(*number), consistent_type };
}

void SqrtCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SQRT:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool SqrtCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<SqrtCalculationNode const&>(other).m_value);
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

bool HypotCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }

    return false;
}

NonnullRefPtr<CalculationNode const> HypotCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_children_vector(*this, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-hypot
Optional<CalculatedStyleValue::CalculationResult> HypotCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    // The hypot(A, …) function contains one or more comma-separated calculations, and returns the length of an
    // N-dimensional vector with components equal to each of the calculations. (That is, the square root of the sum of
    // the squares of its arguments.) The argument calculations can resolve to any <number>, <dimension>, or
    // <percentage>, but must have a consistent type or else the function is invalid; the result’s type will be the
    // consistent type.

    Optional<NumericType> consistent_type;
    double value = 0;

    for (auto const& child : m_values) {
        auto canonical_child = try_get_value_with_canonical_unit(child, context, resolution_context);
        if (!canonical_child.has_value())
            return {};

        if (!consistent_type.has_value())
            consistent_type = canonical_child->type();
        else
            consistent_type = consistent_type->consistent_type(canonical_child->type().value());

        if (!consistent_type.has_value())
            return {};

        value += canonical_child->value() * canonical_child->value();
    }

    if (!consistent_type.has_value())
        return {};

    return CalculatedStyleValue::CalculationResult { sqrt(value), consistent_type };
}

void HypotCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}HYPOT:\n", "", indent);
    for (auto const& value : m_values)
        value->dump(builder, indent + 2);
}

bool HypotCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (!m_values[i]->equals(*static_cast<HypotCalculationNode const&>(other).m_values[i]))
            return false;
    }
    return true;
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

NonnullRefPtr<CalculationNode const> LogCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_2_children(*this, m_x, m_y, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-log
Optional<CalculatedStyleValue::CalculationResult> LogCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    // The log(A, B?) function contains one or two calculations (representing the value to be logarithmed, and the
    // base of the logarithm, defaulting to e), which must resolve to <number>s, and returns the logarithm base B of
    // the value A, as a <number> with the return type made consistent with the input calculation’s type.

    auto number = try_get_number(m_x);
    auto base = try_get_number(m_y);
    if (!number.has_value() || !base.has_value())
        return {};

    auto consistent_type = NumericType {}.made_consistent_with(m_x->numeric_type().value());
    if (!consistent_type.has_value())
        return {};

    return CalculatedStyleValue::CalculationResult { log(*number) / log(*base), consistent_type };
}

void LogCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}LOG:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

bool LogCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_x->equals(*static_cast<LogCalculationNode const&>(other).m_x)
        && m_y->equals(*static_cast<LogCalculationNode const&>(other).m_y);
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

NonnullRefPtr<CalculationNode const> ExpCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-exp
Optional<CalculatedStyleValue::CalculationResult> ExpCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    // The exp(A) function contains one calculation which must resolve to a <number>, and returns the same value as
    // pow(e, A) as a <number> with the return type made consistent with the input calculation’s type.

    auto number = try_get_number(m_value);
    if (!number.has_value())
        return {};

    auto consistent_type = NumericType {}.made_consistent_with(m_value->numeric_type().value());
    if (!consistent_type.has_value())
        return {};

    return CalculatedStyleValue::CalculationResult { exp(*number), consistent_type };
}

void ExpCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}EXP:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool ExpCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<ExpCalculationNode const&>(other).m_value);
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

bool RoundCalculationNode::contains_percentage() const
{
    return m_x->contains_percentage() || m_y->contains_percentage();
}

NonnullRefPtr<CalculationNode const> RoundCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    auto simplified_x = simplify_a_calculation_tree(m_x, context, resolution_context);
    auto simplified_y = simplify_a_calculation_tree(m_y, context, resolution_context);
    if (simplified_x != m_x || simplified_y != m_y)
        return create(m_strategy, move(simplified_x), move(simplified_y));
    return *this;
}

// https://drafts.csswg.org/css-values-4/#funcdef-round
Optional<CalculatedStyleValue::CalculationResult> RoundCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    // The round(<rounding-strategy>?, A, B?) function contains an optional rounding strategy, and two calculations A
    // and B, and returns the value of A, rounded according to the rounding strategy, to the nearest integer multiple of
    // B either above or below A. The argument calculations can resolve to any <number>, <dimension>, or <percentage>,
    // but must have a consistent type or else the function is invalid; the result’s type will be the consistent type.

    auto maybe_a = try_get_value_with_canonical_unit(m_x, context, resolution_context);
    auto maybe_b = try_get_value_with_canonical_unit(m_y, context, resolution_context);
    if (!maybe_a.has_value() || !maybe_b.has_value())
        return {};

    auto consistent_type = maybe_a->type()->made_consistent_with(maybe_b->type().value());
    if (!consistent_type.has_value())
        return {};

    auto a = maybe_a->value();
    auto b = maybe_b->value();

    // https://drafts.csswg.org/css-values-4/#round-infinities
    // In round(A, B), if B is 0, the result is NaN. If A and B are both infinite, the result is NaN.
    if (b == 0 || (isinf(a) && isinf(b)))
        return CalculatedStyleValue::CalculationResult { AK::NaN<double>, consistent_type };

    // If A is infinite but B is finite, the result is the same infinity.
    if (isinf(a) && isfinite(b))
        return CalculatedStyleValue::CalculationResult { a, consistent_type };

    // If A is finite but B is infinite, the result depends on the <rounding-strategy> and the sign of A:
    if (isfinite(a) && isinf(b)) {
        FloatExtractor<double> const extractor { .d = a };

        switch (m_strategy) {
        // nearest, to-zero:
        case RoundingStrategy::Nearest:
        case RoundingStrategy::ToZero: {
            // If A is positive or 0⁺, return 0⁺. Otherwise, return 0⁻.
            return CalculatedStyleValue::CalculationResult { !extractor.sign ? 0.0 : -0.0, consistent_type };
        }
        // up:
        case RoundingStrategy::Up: {
            double result;
            if (a > 0) {
                // If A is positive(not zero), return +∞.
                result = AK::Infinity<double>;
            } else {
                // If A is 0⁺, return 0⁺. Otherwise, return 0⁻.
                result = !extractor.sign ? 0.0 : -0.0;
            }

            return CalculatedStyleValue::CalculationResult { result, consistent_type };
        }
        // down:
        case RoundingStrategy::Down: {
            double result;
            if (a < 0) {
                // If A is negative (not zero), return −∞.
                result = -AK::Infinity<double>;
            } else {
                // If A is 0⁻, return 0⁻. Otherwise, return 0⁺.
                result = extractor.sign ? -0.0 : 0.0;
            }

            return CalculatedStyleValue::CalculationResult { result, consistent_type };
        }
        }
    }

    // If A is exactly equal to an integer multiple of B, round() resolves to A exactly (preserving whether A is 0⁻ or
    // 0⁺, if relevant).
    if (fmod(a, b) == 0)
        return maybe_a.release_value();

    // Otherwise, there are two integer multiples of B that are potentially "closest" to A, lower B which is closer to
    // −∞ and upper B which is closer to +∞. The following <rounding-strategy>s dictate how to choose between them:

    // FIXME: If lower B would be zero, it is specifically equal to 0⁺;
    //        if upper B would be zero, it is specifically equal to 0⁻.
    auto get_lower_b = [&]() {
        return floor(a / b) * b;
    };
    auto get_upper_b = [&]() {
        return ceil(a / b) * b;
    };

    double rounded = 0;
    switch (m_strategy) {
    // -> nearest
    case RoundingStrategy::Nearest: {
        // Choose whichever of lower B and upper B that has the smallest absolute difference from A.
        // If both have an equal difference (A is exactly between the two values), choose upper B.
        auto lower_b = get_lower_b();
        auto upper_b = get_upper_b();
        auto lower_diff = fabs(lower_b - a);
        auto upper_diff = fabs(upper_b - a);
        rounded = upper_diff <= lower_diff ? upper_b : lower_b;
        break;
    }
    // -> up
    case RoundingStrategy::Up:
        // Choose upper B.
        rounded = get_upper_b();
        break;
    // -> down
    case RoundingStrategy::Down:
        // Choose lower B.
        rounded = get_lower_b();
        break;
    // -> to-zero
    case RoundingStrategy::ToZero: {
        // Choose whichever of lower B and upper B that has the smallest absolute difference from 0.
        auto lower_b = get_lower_b();
        auto upper_b = get_upper_b();
        rounded = fabs(upper_b) < fabs(lower_b) ? upper_b : lower_b;
        break;
    }
    }

    return CalculatedStyleValue::CalculationResult { rounded, consistent_type };
}

void RoundCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ROUND: {}\n", "", indent, CSS::to_string(m_strategy));
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

bool RoundCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_strategy == static_cast<RoundCalculationNode const&>(other).m_strategy
        && m_x->equals(*static_cast<RoundCalculationNode const&>(other).m_x)
        && m_y->equals(*static_cast<RoundCalculationNode const&>(other).m_y);
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

bool ModCalculationNode::contains_percentage() const
{
    return m_x->contains_percentage() || m_y->contains_percentage();
}

NonnullRefPtr<CalculationNode const> ModCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_2_children(*this, m_x, m_y, context, resolution_context);
}

enum class ModOrRem {
    Mod,
    Rem,
};
// https://drafts.csswg.org/css-values-4/#funcdef-mod
static Optional<CalculatedStyleValue::CalculationResult> run_mod_or_rem_operation_if_possible(CalculationNode const& numerator, CalculationNode const& denominator, CalculationContext const& context, CalculationResolutionContext const& resolution_context, ModOrRem mod_or_rem)
{
    // The modulus functions mod(A, B) and rem(A, B) similarly contain two calculations A and B, and return the
    // difference between A and the nearest integer multiple of B either above or below A. The argument calculations
    // can resolve to any <number>, <dimension>, or <percentage>, but must have the same type, or else the function
    // is invalid; the result will have the same type as the arguments.
    auto numerator_value = try_get_value_with_canonical_unit(numerator, context, resolution_context);
    auto denominator_value = try_get_value_with_canonical_unit(denominator, context, resolution_context);
    if (!numerator_value.has_value() || !denominator_value.has_value())
        return {};

    if (numerator_value->type() != denominator_value->type())
        return {};

    // The two functions are very similar, and in fact return identical results if both arguments are positive or both
    // are negative: the value of the function is equal to the value of A shifted by the integer multiple of B that
    // brings the value between zero and B. (Specifically, the range includes zero and excludes B.More specifically,
    // if B is positive the range starts at 0⁺, and if B is negative it starts at 0⁻.)
    //
    // Their behavior diverges if the A value and the B step are on opposite sides of zero: mod() (short for “modulus”)
    // continues to choose the integer multiple of B that puts the value between zero and B, as above (guaranteeing
    // that the result will either be zero or share the sign of B, not A), while rem() (short for "remainder") chooses
    // the integer multiple of B that puts the value between zero and -B, avoiding changing the sign of the value.

    double result = 0;
    if (mod_or_rem == ModOrRem::Mod) {
        auto quotient = floor(numerator_value->value() / denominator_value->value());
        result = numerator_value->value() - (denominator_value->value() * quotient);
    } else {
        result = fmod(numerator_value->value(), denominator_value->value());
    }

    return CalculatedStyleValue::CalculationResult { result, numerator_value->type() };
}

// https://drafts.csswg.org/css-values-4/#funcdef-mod
Optional<CalculatedStyleValue::CalculationResult> ModCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return run_mod_or_rem_operation_if_possible(m_x, m_y, context, resolution_context, ModOrRem::Mod);
}

void ModCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}MOD:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

bool ModCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_x->equals(*static_cast<ModCalculationNode const&>(other).m_x)
        && m_y->equals(*static_cast<ModCalculationNode const&>(other).m_y);
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

bool RandomCalculationNode::contains_percentage() const
{
    return m_minimum->contains_percentage() || m_maximum->contains_percentage() || (m_step && m_step->contains_percentage());
}

NonnullRefPtr<CalculationNode const> RandomCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    ValueComparingRefPtr<RandomValueSharingStyleValue const> simplified_random_value_sharing;

    // When we are in the absolutization process we should absolutize m_random_value_sharing
    if (resolution_context.length_resolution_context.has_value()) {
        ComputationContext computation_context {
            .length_resolution_context = resolution_context.length_resolution_context.value(),
            .abstract_element = resolution_context.abstract_element
        };

        simplified_random_value_sharing = m_random_value_sharing->absolutized(computation_context)->as_random_value_sharing();
    } else {
        simplified_random_value_sharing = m_random_value_sharing;
    }

    ValueComparingNonnullRefPtr<CalculationNode const> simplified_minimum = simplify_a_calculation_tree(m_minimum, context, resolution_context);
    ValueComparingNonnullRefPtr<CalculationNode const> simplified_maximum = simplify_a_calculation_tree(m_maximum, context, resolution_context);

    ValueComparingRefPtr<CalculationNode const> simplified_step;
    if (m_step)
        simplified_step = simplify_a_calculation_tree(*m_step, context, resolution_context);

    if (simplified_random_value_sharing == m_random_value_sharing && simplified_minimum == m_minimum && simplified_maximum == m_maximum && simplified_step == m_step)
        return *this;

    return RandomCalculationNode::create(simplified_random_value_sharing.release_nonnull(), move(simplified_minimum), move(simplified_maximum), move(simplified_step));
}

// https://drafts.csswg.org/css-values-5/#random-evaluation
Optional<CalculatedStyleValue::CalculationResult> RandomCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    // NB: We don't want to resolve this before computation time even if it's possible
    if (!resolution_context.abstract_element.has_value() && !resolution_context.length_resolution_context.has_value() && resolution_context.percentage_basis.has<Empty>())
        return {};

    auto random_base_value = m_random_value_sharing->random_base_value();

    auto minimum = try_get_value_with_canonical_unit(m_minimum, context, resolution_context);
    auto maximum = try_get_value_with_canonical_unit(m_maximum, context, resolution_context);

    if (!minimum.has_value() || !maximum.has_value())
        return {};

    auto minimum_value = minimum->value();
    auto maximum_value = maximum->value();
    double step_value = 0;

    if (m_step) {
        auto step = try_get_value_with_canonical_unit(*m_step, context, resolution_context);

        if (!step.has_value())
            return {};

        step_value = step->value();
    }

    // https://drafts.csswg.org/css-values-5/#random-infinities
    // If the maximum value is less than the minimum value, it behaves as if it’s equal to the minimum value.
    if (maximum_value < minimum_value)
        maximum_value = minimum_value;

    // https://drafts.csswg.org/css-values-5/#random-infinities
    // In random(A, B), if A is infinite, the result is infinite.
    if (isinf(minimum_value))
        return CalculatedStyleValue::CalculationResult { AK::Infinity<double>, numeric_type() };

    // If A is finite, but the difference between A and B is either infinite or large enough to be treated as infinite
    // in the user agent, the result is NaN.
    if (isinf(maximum_value))
        return CalculatedStyleValue::CalculationResult { AK::NaN<double>, numeric_type() };

    // If C is infinite, the result is A.
    if (isinf(step_value))
        return CalculatedStyleValue::CalculationResult { minimum_value, numeric_type() };

    // Note: As usual for math functions, if any argument calculation is NaN, the result is NaN.
    if (isnan(minimum_value) || isnan(maximum_value) || isnan(step_value))
        return CalculatedStyleValue::CalculationResult { AK::NaN<double>, numeric_type() };

    // If C is negative, zero, or positive but close enough to zero that the range for the step multiplier (the N
    // mentioned in § 9.3 Evaluating Random Values) would be infinite in the user agent, the step must be ignored. (The
    // function is treated as if only A and B were provided.)
    auto has_step = step_value > AK::NumericLimits<float>::epsilon() * 1000;

    // Given a random function with a random base value R, the value of the function is:
    // - for a random() function with min and max, but no step
    if (!has_step) {
        // Return min + R * (max - min)
        return CalculatedStyleValue::CalculationResult {
            minimum_value + (random_base_value * (maximum_value - minimum_value)),
            numeric_type()
        };
    }

    // for a random() function with min, max, and step
    // Let epsilon be step / 1000, or the smallest representable value greater than zero in the numeric type being used if epsilon would round to zero.
    auto epsilon = step_value / 1000;

    // Let N be the largest integer such that min + N * step is less than or equal to max.
    auto n = floor((maximum_value - minimum_value) / step_value);

    // If N produces a value that is not within epsilon of max, but N+1 would produce a value within epsilon of max, set N to N+1.
    if (abs(maximum_value - (n * step_value + minimum_value)) > epsilon && abs(maximum_value - ((n + 1) * step_value + minimum_value)) < epsilon)
        n = n + 1;

    // Let step index be a random integer less than N+1, given R.
    auto step_index = floor((n + 1) * random_base_value);

    // Let value be min + step index * step.
    auto value = minimum_value + (step_index * step_value);

    // If step index is N and value is within epsilon of max, return max.
    if (step_index == n && abs(maximum_value - value) < epsilon)
        return CalculatedStyleValue::CalculationResult { maximum_value, numeric_type() };

    // Otherwise, return value.
    return CalculatedStyleValue::CalculationResult { value, numeric_type() };
}

void RandomCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}RANDOM:\n", "", indent);
    builder.appendff("{}\n", m_random_value_sharing->to_string(SerializationMode::Normal));
    m_minimum->dump(builder, indent + 2);
    m_maximum->dump(builder, indent + 2);
    if (m_step)
        m_step->dump(builder, indent + 2);
}

bool RandomCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;

    if (type() != other.type())
        return false;

    auto const& other_random = as<RandomCalculationNode>(other);

    return m_random_value_sharing == other_random.m_random_value_sharing
        && m_minimum == other_random.m_minimum
        && m_maximum == other_random.m_maximum
        && m_step == other_random.m_step;
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

bool RemCalculationNode::contains_percentage() const
{
    return m_x->contains_percentage() || m_y->contains_percentage();
}

NonnullRefPtr<CalculationNode const> RemCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_2_children(*this, m_x, m_y, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-mod
Optional<CalculatedStyleValue::CalculationResult> RemCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return run_mod_or_rem_operation_if_possible(m_x, m_y, context, resolution_context, ModOrRem::Rem);
}

void RemCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}REM:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

bool RemCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_x->equals(*static_cast<RemCalculationNode const&>(other).m_x)
        && m_y->equals(*static_cast<RemCalculationNode const&>(other).m_y);
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

bool NonMathFunctionCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;

    if (type() != other.type())
        return false;

    return static_cast<NonMathFunctionCalculationNode const&>(other).function() == m_function;
}

CalculatedStyleValue::CalculationResult CalculatedStyleValue::CalculationResult::from_value(Value const& value, CalculationResolutionContext const& context, Optional<NumericType> numeric_type)
{
    auto number = value.visit(
        [](Number const& number) { return number.value(); },
        [](Angle const& angle) { return angle.to_degrees(); },
        [](Flex const& flex) { return flex.to_fr(); },
        [](Frequency const& frequency) { return frequency.to_hertz(); },
        [&context](Length const& length) {
            // Handle some common cases first, so we can resolve more without a context
            if (length.is_absolute())
                return length.absolute_length_to_px_without_rounding();

            // If we don't have a context, we cant resolve the length, so return NAN
            if (!context.length_resolution_context.has_value()) {
                dbgln("Failed to resolve length `{}`, likely due to calc() being used with relative units and a property not taking it into account", length.to_string());
                return AK::NaN<double>;
            }

            return length.to_px_without_rounding(context.length_resolution_context.value());
        },
        [](Resolution const& resolution) { return resolution.to_dots_per_pixel(); },
        [](Time const& time) { return time.to_seconds(); },
        [](Percentage const& percentage) { return percentage.value(); });

    return CalculationResult { number, move(numeric_type) };
}

void CalculatedStyleValue::CalculationResult::add(CalculationResult const& other)
{
    m_value = m_value + other.m_value;
    m_type = m_type.has_value() && other.m_type.has_value() ? m_type->added_to(*other.m_type) : OptionalNone {};
}

void CalculatedStyleValue::CalculationResult::subtract(CalculationResult const& other)
{
    m_value = m_value - other.m_value;
    m_type = m_type.has_value() && other.m_type.has_value() ? m_type->added_to(*other.m_type) : OptionalNone {};
}

void CalculatedStyleValue::CalculationResult::multiply_by(CalculationResult const& other)
{
    m_value = m_value * other.m_value;
    m_type = m_type.has_value() && other.m_type.has_value() ? m_type->multiplied_by(*other.m_type) : OptionalNone {};
}

void CalculatedStyleValue::CalculationResult::divide_by(CalculationResult const& other)
{
    auto other_copy = other;
    other_copy.invert();
    m_value = m_value * other_copy.m_value;
    m_type = m_type.has_value() && other.m_type.has_value() ? m_type->multiplied_by(*other.m_type) : OptionalNone {};
}

void CalculatedStyleValue::CalculationResult::negate()
{
    m_value = 0 - m_value;
}

void CalculatedStyleValue::CalculationResult::invert()
{
    // FIXME: Correctly handle division by zero.
    m_value = 1.0 / m_value;
    if (m_type.has_value())
        m_type = m_type->inverted();
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
            if (!resolved)
                return nullptr;
            return to_rust_calc_node(*resolved);
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

    auto simplified_calculation_tree = cpp_calc_tree_from_rust(result.simplified, calculation_context);
    StyleValueFFI::rust_calc_node_release(result.simplified);
    return CalculatedStyleValue::create(simplified_calculation_tree, resolved_type(), calculation_context);
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
    // The Rust mirror answers; the C++ node recursion remains for the
    // simplification internals until evaluation moves over.
    return StyleValueFFI::rust_calc_node_contains_percentage(m_value->calculated.rust_calculation.node);
}

bool CalculatedStyleValue::is_fully_simplified() const
{
    return resolve_value({}).has_value();
}

String CalculatedStyleValue::dump() const
{
    StringBuilder builder;
    calculation()->dump(builder, 0);
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

struct NumericChildAndIndex {
    NonnullRefPtr<NumericCalculationNode const> child;
    size_t index;
};
// https://drafts.csswg.org/css-values-4/#calc-simplification
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
