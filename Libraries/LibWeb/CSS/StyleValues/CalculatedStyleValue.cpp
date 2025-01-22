/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CalculatedStyleValue.h"
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/PropertyID.h>

namespace Web::CSS {

static Optional<CSSNumericType> add_the_types(Vector<NonnullOwnPtr<CalculationNode>> const& nodes)
{
    Optional<CSSNumericType> left_type;
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

static Optional<CSSNumericType> add_the_types(CalculationNode const& a, CalculationNode const& b)
{
    auto a_type = a.numeric_type();
    auto b_type = b.numeric_type();
    if (!a_type.has_value() || !b_type.has_value())
        return {};
    return a_type->added_to(*b_type);
}

static Optional<CSSNumericType> add_the_types(CalculationNode const& a, CalculationNode const& b, CalculationNode const& c)
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

static Optional<CSSNumericType> multiply_the_types(Vector<NonnullOwnPtr<CalculationNode>> const& nodes)
{
    // At a * sub-expression, multiply the types of the left and right arguments.
    // The sub-expression’s type is the returned result.
    Optional<CSSNumericType> left_type;
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

Optional<CalculationNode::ConstantType> CalculationNode::constant_type_from_string(StringView string)
{
    if (string.equals_ignoring_ascii_case("e"sv))
        return CalculationNode::ConstantType::E;

    if (string.equals_ignoring_ascii_case("pi"sv))
        return CalculationNode::ConstantType::Pi;

    if (string.equals_ignoring_ascii_case("infinity"sv))
        return CalculationNode::ConstantType::Infinity;

    if (string.equals_ignoring_ascii_case("-infinity"sv))
        return CalculationNode::ConstantType::MinusInfinity;

    if (string.equals_ignoring_ascii_case("NaN"sv))
        return CalculationNode::ConstantType::NaN;

    return {};
}

CalculationNode::CalculationNode(Type type, Optional<CSSNumericType> numeric_type)
    : m_type(type)
    , m_numeric_type(move(numeric_type))
{
}

CalculationNode::~CalculationNode() = default;

static CSSNumericType numeric_type_from_calculated_style_value(CalculatedStyleValue::CalculationResult::Value const& value, CalculationContext const& context)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // Anything else is a terminal value, whose type is determined based on its CSS type.
    // (Unless otherwise specified, the type’s associated percent hint is null.)
    return value.visit(
        [](Number const&) {
            // -> <number>
            // -> <integer>
            //    the type is «[ ]» (empty map)
            return CSSNumericType {};
        },
        [](Length const&) {
            // -> <length>
            //    the type is «[ "length" → 1 ]»
            return CSSNumericType { CSSNumericType::BaseType::Length, 1 };
        },
        [](Angle const&) {
            // -> <angle>
            //    the type is «[ "angle" → 1 ]»
            return CSSNumericType { CSSNumericType::BaseType::Angle, 1 };
        },
        [](Time const&) {
            // -> <time>
            //    the type is «[ "time" → 1 ]»
            return CSSNumericType { CSSNumericType::BaseType::Time, 1 };
        },
        [](Frequency const&) {
            // -> <frequency>
            //    the type is «[ "frequency" → 1 ]»
            return CSSNumericType { CSSNumericType::BaseType::Frequency, 1 };
        },
        [](Resolution const&) {
            // -> <resolution>
            //    the type is «[ "resolution" → 1 ]»
            return CSSNumericType { CSSNumericType::BaseType::Resolution, 1 };
        },
        [](Flex const&) {
            // -> <flex>
            //    the type is «[ "flex" → 1 ]»
            return CSSNumericType { CSSNumericType::BaseType::Flex, 1 };
        },
        // NOTE: <calc-constant> is a separate node type. (FIXME: Should it be?)
        [&context](Percentage const&) {
            // -> <percentage>
            //    If, in the context in which the math function containing this calculation is placed,
            //    <percentage>s are resolved relative to another type of value (such as in width,
            //    where <percentage> is resolved against a <length>), and that other type is not <number>,
            //    the type is determined as the other type, but with a percent hint set to that other type.
            if (context.percentages_resolve_as.has_value() && context.percentages_resolve_as != ValueType::Number && context.percentages_resolve_as != ValueType::Percentage) {
                auto base_type = CSSNumericType::base_type_from_value_type(*context.percentages_resolve_as);
                VERIFY(base_type.has_value());
                auto result = CSSNumericType { base_type.value(), 1 };
                result.set_percent_hint(base_type);
                return result;
            }

            //    Otherwise, the type is «[ "percent" → 1 ]», with a percent hint of "percent".
            auto result = CSSNumericType { CSSNumericType::BaseType::Percent, 1 };
            // FIXME: Setting the percent hint to "percent" causes us to fail tests.
            // result.set_percent_hint(CSSNumericType::BaseType::Percent);
            return result;
        });
}

NonnullOwnPtr<NumericCalculationNode> NumericCalculationNode::create(NumericValue value, CalculationContext const& context)
{
    auto numeric_type = numeric_type_from_calculated_style_value(value, context);
    return adopt_own(*new (nothrow) NumericCalculationNode(move(value), numeric_type));
}

NumericCalculationNode::NumericCalculationNode(NumericValue value, CSSNumericType numeric_type)
    : CalculationNode(Type::Numeric, move(numeric_type))
    , m_value(move(value))
{
}

NumericCalculationNode::~NumericCalculationNode() = default;

String NumericCalculationNode::to_string() const
{
    return m_value.visit([](auto& value) { return value.to_string(); });
}

bool NumericCalculationNode::contains_percentage() const
{
    return m_value.has<Percentage>();
}

CalculatedStyleValue::CalculationResult NumericCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    if (m_value.has<Percentage>()) {
        // NOTE: Depending on whether percentage_basis is set, the caller of resolve() is expecting a raw percentage or
        //       resolved type.
        return context.percentage_basis.visit(
            [&](Empty const&) {
                VERIFY(numeric_type_from_calculated_style_value(m_value, {}) == numeric_type());
                return CalculatedStyleValue::CalculationResult::from_value(m_value, context, numeric_type());
            },
            [&](auto const& value) {
                auto const calculated_value = value.percentage_of(m_value.get<Percentage>());
                return CalculatedStyleValue::CalculationResult::from_value(calculated_value, context, numeric_type_from_calculated_style_value(calculated_value, {}));
            });
    }

    return CalculatedStyleValue::CalculationResult::from_value(m_value, context, numeric_type());
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

NonnullOwnPtr<SumCalculationNode> SumCalculationNode::create(Vector<NonnullOwnPtr<CalculationNode>> values)
{
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // At a + or - sub-expression, attempt to add the types of the left and right arguments.
    // If this returns failure, the entire calculation’s type is failure.
    // Otherwise, the sub-expression’s type is the returned type.
    auto numeric_type = add_the_types(values);
    return adopt_own(*new (nothrow) SumCalculationNode(move(values), move(numeric_type)));
}

SumCalculationNode::SumCalculationNode(Vector<NonnullOwnPtr<CalculationNode>> values, Optional<CSSNumericType> numeric_type)
    : CalculationNode(Type::Sum, move(numeric_type))
    , m_values(move(values))
{
    VERIFY(!m_values.is_empty());
}

SumCalculationNode::~SumCalculationNode() = default;

String SumCalculationNode::to_string() const
{
    bool first = true;
    StringBuilder builder;
    for (auto& value : m_values) {
        if (!first)
            builder.append(" + "sv);
        builder.append(value->to_string());
        first = false;
    }
    return MUST(builder.to_string());
}

bool SumCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }
    return false;
}

CalculatedStyleValue::CalculationResult SumCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    Optional<CalculatedStyleValue::CalculationResult> total;

    for (auto& additional_product : m_values) {
        auto additional_value = additional_product->resolve(context);
        if (!total.has_value()) {
            total = additional_value;
            continue;
        }
        total->add(additional_value);
    }

    return total.value();
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

NonnullOwnPtr<ProductCalculationNode> ProductCalculationNode::create(Vector<NonnullOwnPtr<CalculationNode>> values)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // At a * sub-expression, multiply the types of the left and right arguments.
    // The sub-expression’s type is the returned result.
    auto numeric_type = multiply_the_types(values);
    return adopt_own(*new (nothrow) ProductCalculationNode(move(values), move(numeric_type)));
}

ProductCalculationNode::ProductCalculationNode(Vector<NonnullOwnPtr<CalculationNode>> values, Optional<CSSNumericType> numeric_type)
    : CalculationNode(Type::Product, move(numeric_type))
    , m_values(move(values))
{
    VERIFY(!m_values.is_empty());
}

ProductCalculationNode::~ProductCalculationNode() = default;

String ProductCalculationNode::to_string() const
{
    bool first = true;
    StringBuilder builder;
    for (auto& value : m_values) {
        if (!first)
            builder.append(" * "sv);
        builder.append(value->to_string());
        first = false;
    }
    return MUST(builder.to_string());
}

bool ProductCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }
    return false;
}

CalculatedStyleValue::CalculationResult ProductCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    Optional<CalculatedStyleValue::CalculationResult> total;

    for (auto& additional_product : m_values) {
        auto additional_value = additional_product->resolve(context);
        if (!total.has_value()) {
            total = additional_value;
            continue;
        }
        total->multiply_by(additional_value);
    }

    return total.value();
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

NonnullOwnPtr<NegateCalculationNode> NegateCalculationNode::create(NonnullOwnPtr<CalculationNode> value)
{
    return adopt_own(*new (nothrow) NegateCalculationNode(move(value)));
}

NegateCalculationNode::NegateCalculationNode(NonnullOwnPtr<CalculationNode> value)
    // NOTE: `- foo` doesn't change the type
    : CalculationNode(Type::Negate, value->numeric_type())
    , m_value(move(value))
{
}

NegateCalculationNode::~NegateCalculationNode() = default;

String NegateCalculationNode::to_string() const
{
    return MUST(String::formatted("(0 - {})", m_value->to_string()));
}

bool NegateCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CalculatedStyleValue::CalculationResult NegateCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto child_value = m_value->resolve(context);
    child_value.negate();
    return child_value;
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

NonnullOwnPtr<InvertCalculationNode> InvertCalculationNode::create(NonnullOwnPtr<CalculationNode> value)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // At a / sub-expression, let left type be the result of finding the types of its left argument,
    // and right type be the result of finding the types of its right argument and then inverting it.
    // The sub-expression’s type is the result of multiplying the left type and right type.
    // NOTE: An InvertCalculationNode only represents the right argument here, and the multiplication
    //       is handled in the parent ProductCalculationNode.
    auto numeric_type = value->numeric_type().map([](auto& it) { return it.inverted(); });
    return adopt_own(*new (nothrow) InvertCalculationNode(move(value), move(numeric_type)));
}

InvertCalculationNode::InvertCalculationNode(NonnullOwnPtr<CalculationNode> value, Optional<CSSNumericType> numeric_type)
    : CalculationNode(Type::Invert, move(numeric_type))
    , m_value(move(value))
{
}

InvertCalculationNode::~InvertCalculationNode() = default;

String InvertCalculationNode::to_string() const
{
    return MUST(String::formatted("(1 / {})", m_value->to_string()));
}

bool InvertCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CalculatedStyleValue::CalculationResult InvertCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto child_value = m_value->resolve(context);
    child_value.invert();
    return child_value;
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

NonnullOwnPtr<MinCalculationNode> MinCalculationNode::create(Vector<NonnullOwnPtr<CalculationNode>> values)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(values);
    return adopt_own(*new (nothrow) MinCalculationNode(move(values), move(numeric_type)));
}

MinCalculationNode::MinCalculationNode(Vector<NonnullOwnPtr<CalculationNode>> values, Optional<CSSNumericType> numeric_type)
    : CalculationNode(Type::Min, move(numeric_type))
    , m_values(move(values))
{
}

MinCalculationNode::~MinCalculationNode() = default;

String MinCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("min("sv);
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (i != 0)
            builder.append(", "sv);
        builder.append(m_values[i]->to_string());
    }
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool MinCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }

    return false;
}

CalculatedStyleValue::CalculationResult MinCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    CalculatedStyleValue::CalculationResult smallest_node = m_values.first()->resolve(context);
    auto smallest_value = smallest_node.value();

    for (size_t i = 1; i < m_values.size(); i++) {
        auto child_resolved = m_values[i]->resolve(context);
        auto child_value = child_resolved.value();

        if (child_value < smallest_value) {
            smallest_value = child_value;
            smallest_node = child_resolved;
        }
    }

    return smallest_node;
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

NonnullOwnPtr<MaxCalculationNode> MaxCalculationNode::create(Vector<NonnullOwnPtr<CalculationNode>> values)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(values);
    return adopt_own(*new (nothrow) MaxCalculationNode(move(values), move(numeric_type)));
}

MaxCalculationNode::MaxCalculationNode(Vector<NonnullOwnPtr<CalculationNode>> values, Optional<CSSNumericType> numeric_type)
    : CalculationNode(Type::Max, move(numeric_type))
    , m_values(move(values))
{
}

MaxCalculationNode::~MaxCalculationNode() = default;

String MaxCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("max("sv);
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (i != 0)
            builder.append(", "sv);
        builder.append(m_values[i]->to_string());
    }
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool MaxCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }

    return false;
}

CalculatedStyleValue::CalculationResult MaxCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    CalculatedStyleValue::CalculationResult largest_node = m_values.first()->resolve(context);
    auto largest_value = largest_node.value();

    for (size_t i = 1; i < m_values.size(); i++) {
        auto child_resolved = m_values[i]->resolve(context);
        auto child_value = child_resolved.value();

        if (child_value > largest_value) {
            largest_value = child_value;
            largest_node = child_resolved;
        }
    }

    return largest_node;
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

NonnullOwnPtr<ClampCalculationNode> ClampCalculationNode::create(NonnullOwnPtr<CalculationNode> min, NonnullOwnPtr<CalculationNode> center, NonnullOwnPtr<CalculationNode> max)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(*min, *center, *max);
    return adopt_own(*new (nothrow) ClampCalculationNode(move(min), move(center), move(max), move(numeric_type)));
}

ClampCalculationNode::ClampCalculationNode(NonnullOwnPtr<CalculationNode> min, NonnullOwnPtr<CalculationNode> center, NonnullOwnPtr<CalculationNode> max, Optional<CSSNumericType> numeric_type)
    : CalculationNode(Type::Clamp, move(numeric_type))
    , m_min_value(move(min))
    , m_center_value(move(center))
    , m_max_value(move(max))
{
}

ClampCalculationNode::~ClampCalculationNode() = default;

String ClampCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("clamp("sv);
    builder.append(m_min_value->to_string());
    builder.append(", "sv);
    builder.append(m_center_value->to_string());
    builder.append(", "sv);
    builder.append(m_max_value->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool ClampCalculationNode::contains_percentage() const
{
    return m_min_value->contains_percentage() || m_center_value->contains_percentage() || m_max_value->contains_percentage();
}

CalculatedStyleValue::CalculationResult ClampCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto min_node = m_min_value->resolve(context);
    auto center_node = m_center_value->resolve(context);
    auto max_node = m_max_value->resolve(context);

    auto min_value = min_node.value();
    auto center_value = center_node.value();
    auto max_value = max_node.value();

    // NOTE: The value should be returned as "max(MIN, min(VAL, MAX))"
    auto chosen_value = max(min_value, min(center_value, max_value));
    if (chosen_value == min_value)
        return min_node;
    if (chosen_value == center_value)
        return center_node;
    if (chosen_value == max_value)
        return max_node;

    VERIFY_NOT_REACHED();
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

NonnullOwnPtr<AbsCalculationNode> AbsCalculationNode::create(NonnullOwnPtr<CalculationNode> value)
{
    return adopt_own(*new (nothrow) AbsCalculationNode(move(value)));
}

AbsCalculationNode::AbsCalculationNode(NonnullOwnPtr<CalculationNode> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // The type of its contained calculation.
    : CalculationNode(Type::Abs, value->numeric_type())
    , m_value(move(value))
{
}

AbsCalculationNode::~AbsCalculationNode() = default;

String AbsCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("abs("sv);
    builder.append(m_value->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool AbsCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CalculatedStyleValue::CalculationResult AbsCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_value->resolve(context);
    if (node_a.value() < 0)
        node_a.negate();
    return node_a;
}

void AbsCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ABS: {}\n", "", indent, to_string());
}

bool AbsCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<AbsCalculationNode const&>(other).m_value);
}

NonnullOwnPtr<SignCalculationNode> SignCalculationNode::create(NonnullOwnPtr<CalculationNode> value)
{
    return adopt_own(*new (nothrow) SignCalculationNode(move(value)));
}

SignCalculationNode::SignCalculationNode(NonnullOwnPtr<CalculationNode> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Sign, CSSNumericType {})
    , m_value(move(value))
{
}

SignCalculationNode::~SignCalculationNode() = default;

String SignCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("sign("sv);
    builder.append(m_value->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool SignCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CalculatedStyleValue::CalculationResult SignCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_value->resolve(context);
    auto node_a_value = node_a.value();

    if (node_a_value < 0)
        return { -1, CSSNumericType {} };

    if (node_a_value > 0)
        return { 1, CSSNumericType {} };

    return { 0, CSSNumericType {} };
}

void SignCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SIGN: {}\n", "", indent, to_string());
}

bool SignCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<SignCalculationNode const&>(other).m_value);
}

NonnullOwnPtr<ConstantCalculationNode> ConstantCalculationNode::create(ConstantType constant)
{
    return adopt_own(*new (nothrow) ConstantCalculationNode(constant));
}

ConstantCalculationNode::ConstantCalculationNode(ConstantType constant)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // Anything else is a terminal value, whose type is determined based on its CSS type:
    // -> <calc-constant>
    //    the type is «[ ]» (empty map)
    : CalculationNode(Type::Constant, CSSNumericType {})
    , m_constant(constant)
{
}

ConstantCalculationNode::~ConstantCalculationNode() = default;

String ConstantCalculationNode::to_string() const
{
    switch (m_constant) {
    case CalculationNode::ConstantType::E:
        return "e"_string;
    case CalculationNode::ConstantType::Pi:
        return "pi"_string;
    case CalculationNode::ConstantType::Infinity:
        return "infinity"_string;
    case CalculationNode::ConstantType::MinusInfinity:
        return "-infinity"_string;
    case CalculationNode::ConstantType::NaN:
        return "NaN"_string;
    }

    VERIFY_NOT_REACHED();
}

CalculatedStyleValue::CalculationResult ConstantCalculationNode::resolve(CalculationResolutionContext const&) const
{
    switch (m_constant) {
    case ConstantType::E:
        return { AK::E<double>, CSSNumericType {} };
    case ConstantType::Pi:
        return { AK::Pi<double>, CSSNumericType {} };
    // FIXME: We need to keep track of Infinity and NaN across all nodes, since they require special handling.
    case ConstantType::Infinity:
        return { NumericLimits<double>::max(), CSSNumericType {} };
    case ConstantType::MinusInfinity:
        return { NumericLimits<double>::lowest(), CSSNumericType {} };
    case ConstantType::NaN:
        return { AK::NaN<double>, CSSNumericType {} };
    }

    VERIFY_NOT_REACHED();
}

void ConstantCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}CONSTANT: {}\n", "", indent, to_string());
}

bool ConstantCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_constant == static_cast<ConstantCalculationNode const&>(other).m_constant;
}

NonnullOwnPtr<SinCalculationNode> SinCalculationNode::create(NonnullOwnPtr<CalculationNode> value)
{
    return adopt_own(*new (nothrow) SinCalculationNode(move(value)));
}

SinCalculationNode::SinCalculationNode(NonnullOwnPtr<CalculationNode> value)
    // «[ ]» (empty map).
    : CalculationNode(Type::Sin, CSSNumericType {})
    , m_value(move(value))
{
}

SinCalculationNode::~SinCalculationNode() = default;

String SinCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("sin("sv);
    builder.append(m_value->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool SinCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CalculatedStyleValue::CalculationResult SinCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_value->resolve(context);
    auto node_a_value = AK::to_radians(node_a.value());
    auto result = sin(node_a_value);

    return { result, CSSNumericType {} };
}

void SinCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SIN: {}\n", "", indent, to_string());
}

bool SinCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<SinCalculationNode const&>(other).m_value);
}

NonnullOwnPtr<CosCalculationNode> CosCalculationNode::create(NonnullOwnPtr<CalculationNode> value)
{
    return adopt_own(*new (nothrow) CosCalculationNode(move(value)));
}

CosCalculationNode::CosCalculationNode(NonnullOwnPtr<CalculationNode> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Cos, CSSNumericType {})
    , m_value(move(value))
{
}

CosCalculationNode::~CosCalculationNode() = default;

String CosCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("cos("sv);
    builder.append(m_value->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool CosCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CalculatedStyleValue::CalculationResult CosCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_value->resolve(context);
    auto node_a_value = AK::to_radians(node_a.value());
    auto result = cos(node_a_value);

    return { result, CSSNumericType {} };
}

void CosCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}COS: {}\n", "", indent, to_string());
}

bool CosCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<CosCalculationNode const&>(other).m_value);
}

NonnullOwnPtr<TanCalculationNode> TanCalculationNode::create(NonnullOwnPtr<CalculationNode> value)
{
    return adopt_own(*new (nothrow) TanCalculationNode(move(value)));
}

TanCalculationNode::TanCalculationNode(NonnullOwnPtr<CalculationNode> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Tan, CSSNumericType {})
    , m_value(move(value))
{
}

TanCalculationNode::~TanCalculationNode() = default;

String TanCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("tan("sv);
    builder.append(m_value->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool TanCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CalculatedStyleValue::CalculationResult TanCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_value->resolve(context);
    auto node_a_value = AK::to_radians(node_a.value());
    auto result = tan(node_a_value);

    return { result, CSSNumericType {} };
}

void TanCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}TAN: {}\n", "", indent, to_string());
}

bool TanCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<TanCalculationNode const&>(other).m_value);
}

NonnullOwnPtr<AsinCalculationNode> AsinCalculationNode::create(NonnullOwnPtr<CalculationNode> value)
{
    return adopt_own(*new (nothrow) AsinCalculationNode(move(value)));
}

AsinCalculationNode::AsinCalculationNode(NonnullOwnPtr<CalculationNode> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ "angle" → 1 ]».
    : CalculationNode(Type::Asin, CSSNumericType { CSSNumericType::BaseType::Angle, 1 })
    , m_value(move(value))
{
}

AsinCalculationNode::~AsinCalculationNode() = default;

String AsinCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("asin("sv);
    builder.append(m_value->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool AsinCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CalculatedStyleValue::CalculationResult AsinCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_value->resolve(context);
    auto result = AK::to_degrees(asin(node_a.value()));
    return { result, CSSNumericType { CSSNumericType::BaseType::Angle, 1 } };
}

void AsinCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ASIN: {}\n", "", indent, to_string());
}

bool AsinCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<AsinCalculationNode const&>(other).m_value);
}

NonnullOwnPtr<AcosCalculationNode> AcosCalculationNode::create(NonnullOwnPtr<CalculationNode> value)
{
    return adopt_own(*new (nothrow) AcosCalculationNode(move(value)));
}

AcosCalculationNode::AcosCalculationNode(NonnullOwnPtr<CalculationNode> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ "angle" → 1 ]».
    : CalculationNode(Type::Acos, CSSNumericType { CSSNumericType::BaseType::Angle, 1 })
    , m_value(move(value))
{
}

AcosCalculationNode::~AcosCalculationNode() = default;

String AcosCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("acos("sv);
    builder.append(m_value->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool AcosCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CalculatedStyleValue::CalculationResult AcosCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_value->resolve(context);
    auto result = AK::to_degrees(acos(node_a.value()));
    return { result, CSSNumericType { CSSNumericType::BaseType::Angle, 1 } };
}

void AcosCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ACOS: {}\n", "", indent, to_string());
}

bool AcosCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<AcosCalculationNode const&>(other).m_value);
}

NonnullOwnPtr<AtanCalculationNode> AtanCalculationNode::create(NonnullOwnPtr<CalculationNode> value)
{
    return adopt_own(*new (nothrow) AtanCalculationNode(move(value)));
}

AtanCalculationNode::AtanCalculationNode(NonnullOwnPtr<CalculationNode> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ "angle" → 1 ]».
    : CalculationNode(Type::Atan, CSSNumericType { CSSNumericType::BaseType::Angle, 1 })
    , m_value(move(value))
{
}

AtanCalculationNode::~AtanCalculationNode() = default;

String AtanCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("atan("sv);
    builder.append(m_value->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool AtanCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CalculatedStyleValue::CalculationResult AtanCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_value->resolve(context);
    auto result = AK::to_degrees(atan(node_a.value()));
    return { result, CSSNumericType { CSSNumericType::BaseType::Angle, 1 } };
}

void AtanCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ATAN: {}\n", "", indent, to_string());
}

bool AtanCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<AtanCalculationNode const&>(other).m_value);
}

NonnullOwnPtr<Atan2CalculationNode> Atan2CalculationNode::create(NonnullOwnPtr<CalculationNode> y, NonnullOwnPtr<CalculationNode> x)
{
    return adopt_own(*new (nothrow) Atan2CalculationNode(move(y), move(x)));
}

Atan2CalculationNode::Atan2CalculationNode(NonnullOwnPtr<CalculationNode> y, NonnullOwnPtr<CalculationNode> x)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ "angle" → 1 ]».
    : CalculationNode(Type::Atan2, CSSNumericType { CSSNumericType::BaseType::Angle, 1 })
    , m_y(move(y))
    , m_x(move(x))
{
}

Atan2CalculationNode::~Atan2CalculationNode() = default;

String Atan2CalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("atan2("sv);
    builder.append(m_y->to_string());
    builder.append(", "sv);
    builder.append(m_x->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool Atan2CalculationNode::contains_percentage() const
{
    return m_y->contains_percentage() || m_x->contains_percentage();
}

CalculatedStyleValue::CalculationResult Atan2CalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_y->resolve(context);
    auto node_b = m_x->resolve(context);
    auto result = AK::to_degrees(atan2(node_a.value(), node_b.value()));
    return { result, CSSNumericType { CSSNumericType::BaseType::Angle, 1 } };
}

void Atan2CalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ATAN2: {}\n", "", indent, to_string());
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

NonnullOwnPtr<PowCalculationNode> PowCalculationNode::create(NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y)
{
    return adopt_own(*new (nothrow) PowCalculationNode(move(x), move(y)));
}

PowCalculationNode::PowCalculationNode(NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Pow, CSSNumericType {})
    , m_x(move(x))
    , m_y(move(y))
{
}

PowCalculationNode::~PowCalculationNode() = default;

String PowCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("pow("sv);
    builder.append(m_x->to_string());
    builder.append(", "sv);
    builder.append(m_y->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

CalculatedStyleValue::CalculationResult PowCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_x->resolve(context);
    auto node_b = m_y->resolve(context);
    auto result = pow(node_a.value(), node_b.value());
    return { result, CSSNumericType {} };
}

void PowCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}POW: {}\n", "", indent, to_string());
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

NonnullOwnPtr<SqrtCalculationNode> SqrtCalculationNode::create(NonnullOwnPtr<CalculationNode> value)
{
    return adopt_own(*new (nothrow) SqrtCalculationNode(move(value)));
}

SqrtCalculationNode::SqrtCalculationNode(NonnullOwnPtr<CalculationNode> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Sqrt, CSSNumericType {})
    , m_value(move(value))
{
}

SqrtCalculationNode::~SqrtCalculationNode() = default;

String SqrtCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("sqrt("sv);
    builder.append(m_value->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

CalculatedStyleValue::CalculationResult SqrtCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_value->resolve(context);
    auto result = sqrt(node_a.value());
    return { result, CSSNumericType {} };
}

void SqrtCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SQRT: {}\n", "", indent, to_string());
}

bool SqrtCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<SqrtCalculationNode const&>(other).m_value);
}

NonnullOwnPtr<HypotCalculationNode> HypotCalculationNode::create(Vector<NonnullOwnPtr<CalculationNode>> values)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(values);
    return adopt_own(*new (nothrow) HypotCalculationNode(move(values), move(numeric_type)));
}

HypotCalculationNode::HypotCalculationNode(Vector<NonnullOwnPtr<CalculationNode>> values, Optional<CSSNumericType> numeric_type)
    : CalculationNode(Type::Hypot, move(numeric_type))
    , m_values(move(values))
{
}

HypotCalculationNode::~HypotCalculationNode() = default;

String HypotCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("hypot("sv);
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (i != 0)
            builder.append(", "sv);
        builder.append(m_values[i]->to_string());
    }
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool HypotCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }

    return false;
}

CalculatedStyleValue::CalculationResult HypotCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    double square_sum = 0.0;
    Optional<CSSNumericType> result_type;

    for (auto const& value : m_values) {
        auto child_resolved = value->resolve(context);
        auto child_value = child_resolved.value();

        square_sum += child_value * child_value;
        if (result_type.has_value()) {
            result_type = result_type->consistent_type(*child_resolved.type());
        } else {
            result_type = child_resolved.type();
        }
    }

    auto result = sqrt(square_sum);
    return { result, result_type };
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

NonnullOwnPtr<LogCalculationNode> LogCalculationNode::create(NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y)
{
    return adopt_own(*new (nothrow) LogCalculationNode(move(x), move(y)));
}

LogCalculationNode::LogCalculationNode(NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Log, CSSNumericType {})
    , m_x(move(x))
    , m_y(move(y))
{
}

LogCalculationNode::~LogCalculationNode() = default;

String LogCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("log("sv);
    builder.append(m_x->to_string());
    builder.append(", "sv);
    builder.append(m_y->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

CalculatedStyleValue::CalculationResult LogCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_x->resolve(context);
    auto node_b = m_y->resolve(context);
    auto result = log2(node_a.value()) / log2(node_b.value());
    return { result, CSSNumericType {} };
}

void LogCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}LOG: {}\n", "", indent, to_string());
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

NonnullOwnPtr<ExpCalculationNode> ExpCalculationNode::create(NonnullOwnPtr<CalculationNode> value)
{
    return adopt_own(*new (nothrow) ExpCalculationNode(move(value)));
}

ExpCalculationNode::ExpCalculationNode(NonnullOwnPtr<CalculationNode> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Exp, CSSNumericType {})
    , m_value(move(value))
{
}

ExpCalculationNode::~ExpCalculationNode() = default;

String ExpCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("exp("sv);
    builder.append(m_value->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

CalculatedStyleValue::CalculationResult ExpCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_value->resolve(context);
    auto result = exp(node_a.value());
    return { result, CSSNumericType {} };
}

void ExpCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}EXP: {}\n", "", indent, to_string());
}

bool ExpCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<ExpCalculationNode const&>(other).m_value);
}

NonnullOwnPtr<RoundCalculationNode> RoundCalculationNode::create(RoundingStrategy strategy, NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y)
{
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(*x, *y);
    return adopt_own(*new (nothrow) RoundCalculationNode(strategy, move(x), move(y), move(numeric_type)));
}

RoundCalculationNode::RoundCalculationNode(RoundingStrategy mode, NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y, Optional<CSSNumericType> numeric_type)
    : CalculationNode(Type::Round, move(numeric_type))
    , m_strategy(mode)
    , m_x(move(x))
    , m_y(move(y))
{
}

RoundCalculationNode::~RoundCalculationNode() = default;

String RoundCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("round("sv);
    builder.append(CSS::to_string(m_strategy));
    builder.append(", "sv);
    builder.append(m_x->to_string());
    builder.append(", "sv);
    builder.append(m_y->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool RoundCalculationNode::contains_percentage() const
{
    return m_x->contains_percentage() || m_y->contains_percentage();
}

CalculatedStyleValue::CalculationResult RoundCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_x->resolve(context);
    auto node_b = m_y->resolve(context);

    auto node_a_value = node_a.value();
    auto node_b_value = node_b.value();

    auto upper_b = ceil(node_a_value / node_b_value) * node_b_value;
    auto lower_b = floor(node_a_value / node_b_value) * node_b_value;

    auto resolved_type = node_a.type()->consistent_type(*node_b.type());

    if (m_strategy == RoundingStrategy::Nearest) {
        auto upper_diff = fabs(upper_b - node_a_value);
        auto lower_diff = fabs(node_a_value - lower_b);
        auto rounded_value = upper_diff < lower_diff ? upper_b : lower_b;
        return { rounded_value, resolved_type };
    }

    if (m_strategy == RoundingStrategy::Up) {
        return { upper_b, resolved_type };
    }

    if (m_strategy == RoundingStrategy::Down) {
        return { lower_b, resolved_type };
    }

    if (m_strategy == RoundingStrategy::ToZero) {
        auto upper_diff = fabs(upper_b);
        auto lower_diff = fabs(lower_b);
        auto rounded_value = upper_diff < lower_diff ? upper_b : lower_b;
        return { rounded_value, resolved_type };
    }

    VERIFY_NOT_REACHED();
}

void RoundCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ROUND: {}\n", "", indent, to_string());
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

NonnullOwnPtr<ModCalculationNode> ModCalculationNode::create(NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y)
{
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(*x, *y);
    return adopt_own(*new (nothrow) ModCalculationNode(move(x), move(y), move(numeric_type)));
}

ModCalculationNode::ModCalculationNode(NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y, Optional<CSSNumericType> numeric_type)
    : CalculationNode(Type::Mod, move(numeric_type))
    , m_x(move(x))
    , m_y(move(y))
{
}

ModCalculationNode::~ModCalculationNode() = default;

String ModCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("mod("sv);
    builder.append(m_x->to_string());
    builder.append(", "sv);
    builder.append(m_y->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool ModCalculationNode::contains_percentage() const
{
    return m_x->contains_percentage() || m_y->contains_percentage();
}

CalculatedStyleValue::CalculationResult ModCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_x->resolve(context);
    auto node_b = m_y->resolve(context);

    auto node_a_value = node_a.value();
    auto node_b_value = node_b.value();

    auto quotient = floor(node_a_value / node_b_value);
    auto value = node_a_value - (node_b_value * quotient);
    return { value, node_a.type() };
}

void ModCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}MOD: {}\n", "", indent, to_string());
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

NonnullOwnPtr<RemCalculationNode> RemCalculationNode::create(NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y)
{
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(*x, *y);
    return adopt_own(*new (nothrow) RemCalculationNode(move(x), move(y), move(numeric_type)));
}

RemCalculationNode::RemCalculationNode(NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y, Optional<CSSNumericType> numeric_type)
    : CalculationNode(Type::Rem, move(numeric_type))
    , m_x(move(x))
    , m_y(move(y))
{
}

RemCalculationNode::~RemCalculationNode() = default;

String RemCalculationNode::to_string() const
{
    StringBuilder builder;
    builder.append("rem("sv);
    builder.append(m_x->to_string());
    builder.append(", "sv);
    builder.append(m_y->to_string());
    builder.append(")"sv);
    return MUST(builder.to_string());
}

bool RemCalculationNode::contains_percentage() const
{
    return m_x->contains_percentage() || m_y->contains_percentage();
}

CalculatedStyleValue::CalculationResult RemCalculationNode::resolve(CalculationResolutionContext const& context) const
{
    auto node_a = m_x->resolve(context);
    auto node_b = m_y->resolve(context);
    auto value = fmod(node_a.value(), node_b.value());
    return { value, node_a.type() };
}

void RemCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}REM: {}\n", "", indent, to_string());
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

CalculatedStyleValue::CalculationResult CalculatedStyleValue::CalculationResult::from_value(Value const& value, CalculationResolutionContext const& context, Optional<CSSNumericType> numeric_type)
{
    auto const expected_numeric_type = numeric_type_from_calculated_style_value(value, {});
    if (numeric_type.has_value()) {
        VERIFY(numeric_type.value() == expected_numeric_type);
    }

    auto number = value.visit(
        [](Number const& number) { return number.value(); },
        [](Angle const& angle) { return angle.to_degrees(); },
        [](Flex const& flex) { return flex.to_fr(); },
        [](Frequency const& frequency) { return frequency.to_hertz(); },
        [&context](Length const& length) {
            // Handle some common cases first, so we can resolve more without a context
            if (length.is_auto())
                return 0.0;

            if (length.is_absolute())
                return length.absolute_length_to_px().to_double();

            // If we don't have a context, we cant resolve the length, so return NAN
            if (!context.length_resolution_context.has_value()) {
                dbgln("Failed to resolve length, likely due to calc() being used with relative units and a property not taking it into account");
                return AK::NaN<double>;
            }

            return length.to_px(context.length_resolution_context.value()).to_double();
        },
        [](Resolution const& resolution) { return resolution.to_dots_per_pixel(); },
        [](Time const& time) { return time.to_seconds(); },
        [](Percentage const& percentage) { return percentage.value(); });

    return CalculationResult { number, numeric_type };
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

String CalculatedStyleValue::to_string(SerializationMode) const
{
    // FIXME: Implement this according to https://www.w3.org/TR/css-values-4/#calc-serialize once that stabilizes.
    return MUST(String::formatted("calc({})", m_calculation->to_string()));
}

bool CalculatedStyleValue::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;

    return m_calculation->equals(*other.as_calculated().m_calculation);
}

Optional<Angle> CalculatedStyleValue::resolve_angle(CalculationResolutionContext const& context) const
{
    auto result = m_calculation->resolve(context);
    if (result.type().has_value() && result.type()->matches_angle(m_context.percentages_resolve_as))
        return Angle::make_degrees(result.value());
    return {};
}

Optional<Flex> CalculatedStyleValue::resolve_flex(CalculationResolutionContext const& context) const
{
    auto result = m_calculation->resolve(context);
    if (result.type().has_value() && result.type()->matches_flex(m_context.percentages_resolve_as))
        return Flex::make_fr(result.value());
    return {};
}

Optional<Frequency> CalculatedStyleValue::resolve_frequency(CalculationResolutionContext const& context) const
{
    auto result = m_calculation->resolve(context);
    if (result.type().has_value() && result.type()->matches_frequency(m_context.percentages_resolve_as))
        return Frequency::make_hertz(result.value());
    return {};
}

Optional<Length> CalculatedStyleValue::resolve_length(CalculationResolutionContext const& context) const
{
    auto result = m_calculation->resolve(context);
    if (result.type().has_value() && result.type()->matches_length(m_context.percentages_resolve_as))
        return Length::make_px(CSSPixels { result.value() });
    return {};
}

Optional<Percentage> CalculatedStyleValue::resolve_percentage(CalculationResolutionContext const& context) const
{
    auto result = m_calculation->resolve(context);
    if (result.type().has_value() && result.type()->matches_percentage())
        return Percentage { result.value() };
    return {};
}

Optional<Resolution> CalculatedStyleValue::resolve_resolution(CalculationResolutionContext const& context) const
{
    auto result = m_calculation->resolve(context);
    if (result.type().has_value() && result.type()->matches_resolution(m_context.percentages_resolve_as))
        return Resolution::make_dots_per_pixel(result.value());
    return {};
}

Optional<Time> CalculatedStyleValue::resolve_time(CalculationResolutionContext const& context) const
{
    auto result = m_calculation->resolve(context);
    if (result.type().has_value() && result.type()->matches_time(m_context.percentages_resolve_as))
        return Time::make_seconds(result.value());
    return {};
}

Optional<double> CalculatedStyleValue::resolve_number(CalculationResolutionContext const& context) const
{
    auto result = m_calculation->resolve(context);
    if (result.type().has_value() && result.type()->matches_number(m_context.percentages_resolve_as))
        return result.value();
    return {};
}

Optional<i64> CalculatedStyleValue::resolve_integer(CalculationResolutionContext const& context) const
{
    auto result = m_calculation->resolve(context);
    if (result.type().has_value() && result.type()->matches_number(m_context.percentages_resolve_as))
        return llround(result.value());
    return {};
}

bool CalculatedStyleValue::contains_percentage() const
{
    return m_calculation->contains_percentage();
}

String CalculatedStyleValue::dump() const
{
    StringBuilder builder;
    m_calculation->dump(builder, 0);
    return builder.to_string_without_validation();
}

}
