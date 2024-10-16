/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSMathValue.h"
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/PropertyID.h>

namespace Web::CSS {

static bool is_number(CSSMathValue::ResolvedType type)
{
    return type == CSSMathValue::ResolvedType::Number || type == CSSMathValue::ResolvedType::Integer;
}

static bool is_dimension(CSSMathValue::ResolvedType type)
{
    return type != CSSMathValue::ResolvedType::Number
        && type != CSSMathValue::ResolvedType::Integer
        && type != CSSMathValue::ResolvedType::Percentage;
}

static double resolve_value_radians(CSSMathValue::CalculationResult::Value value)
{
    return value.visit(
        [](Number const& number) { return number.value(); },
        [](Angle const& angle) { return angle.to_radians(); },
        [](auto const&) { VERIFY_NOT_REACHED(); return 0.0; });
}

static double resolve_value(CSSMathValue::CalculationResult::Value value, Optional<Length::ResolutionContext const&> context)
{
    return value.visit(
        [](Number const& number) { return number.value(); },
        [](Angle const& angle) { return angle.to_degrees(); },
        [](Flex const& flex) { return flex.to_fr(); },
        [](Frequency const& frequency) { return frequency.to_hertz(); },
        [](Percentage const& percentage) { return percentage.value(); },
        [](Resolution const& resolution) { return resolution.to_dots_per_pixel(); },
        [](Time const& time) { return time.to_seconds(); },
        [&context](Length const& length) {
            // Handle some common cases first, so we can resolve more without a context
            if (length.is_auto())
                return 0.0;

            if (length.is_absolute())
                return length.absolute_length_to_px().to_double();

            // If we dont have a context, we cant resolve the length, so return NAN
            if (!context.has_value())
                return Number(Number::Type::Number, NAN).value();

            return length.to_px(*context).to_double();
        });
}

static Optional<CSSNumericType> add_the_types(Vector<NonnullOwnPtr<CalculationNode>> const& nodes, PropertyID property_id)
{
    Optional<CSSNumericType> left_type;
    for (auto const& value : nodes) {
        auto right_type = value->determine_type(property_id);
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

static CSSMathValue::CalculationResult to_resolved_type(CSSMathValue::ResolvedType type, double value)
{
    switch (type) {
    case CSSMathValue::ResolvedType::Integer:
        return { Number(Number::Type::Integer, value) };
    case CSSMathValue::ResolvedType::Number:
        return { Number(Number::Type::Number, value) };
    case CSSMathValue::ResolvedType::Angle:
        return { Angle::make_degrees(value) };
    case CSSMathValue::ResolvedType::Flex:
        return { Flex::make_fr(value) };
    case CSSMathValue::ResolvedType::Frequency:
        return { Frequency::make_hertz(value) };
    case CSSMathValue::ResolvedType::Length:
        return { Length::make_px(CSSPixels::nearest_value_for(value)) };
    case CSSMathValue::ResolvedType::Percentage:
        return { Percentage(value) };
    case CSSMathValue::ResolvedType::Resolution:
        return { Resolution::make_dots_per_pixel(value) };
    case CSSMathValue::ResolvedType::Time:
        return { Time::make_seconds(value) };
    }

    VERIFY_NOT_REACHED();
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

CalculationNode::CalculationNode(Type type)
    : m_type(type)
{
}

CalculationNode::~CalculationNode() = default;

NonnullOwnPtr<NumericCalculationNode> NumericCalculationNode::create(NumericValue value)
{
    return adopt_own(*new (nothrow) NumericCalculationNode(move(value)));
}

NumericCalculationNode::NumericCalculationNode(NumericValue value)
    : CalculationNode(Type::Numeric)
    , m_value(move(value))
{
}

NumericCalculationNode::~NumericCalculationNode() = default;

String NumericCalculationNode::to_string() const
{
    return m_value.visit([](auto& value) { return value.to_string(); });
}

Optional<CSSMathValue::ResolvedType> NumericCalculationNode::resolved_type() const
{
    return m_value.visit(
        [](Number const&) { return CSSMathValue::ResolvedType::Number; },
        [](Angle const&) { return CSSMathValue::ResolvedType::Angle; },
        [](Flex const&) { return CSSMathValue::ResolvedType::Flex; },
        [](Frequency const&) { return CSSMathValue::ResolvedType::Frequency; },
        [](Length const&) { return CSSMathValue::ResolvedType::Length; },
        [](Percentage const&) { return CSSMathValue::ResolvedType::Percentage; },
        [](Resolution const&) { return CSSMathValue::ResolvedType::Resolution; },
        [](Time const&) { return CSSMathValue::ResolvedType::Time; });
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> NumericCalculationNode::determine_type(PropertyID property_id) const
{
    // Anything else is a terminal value, whose type is determined based on its CSS type:
    return m_value.visit(
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
        [property_id](Percentage const&) {
            // -> <percentage>
            //    If, in the context in which the math function containing this calculation is placed,
            //    <percentage>s are resolved relative to another type of value (such as in width,
            //    where <percentage> is resolved against a <length>), and that other type is not <number>,
            //    the type is determined as the other type.
            auto percentage_resolved_type = property_resolves_percentages_relative_to(property_id);
            if (percentage_resolved_type.has_value() && percentage_resolved_type != ValueType::Number && percentage_resolved_type != ValueType::Percentage) {
                auto base_type = CSSNumericType::base_type_from_value_type(*percentage_resolved_type);
                VERIFY(base_type.has_value());
                return CSSNumericType { base_type.value(), 1 };
            }

            //    Otherwise, the type is «[ "percent" → 1 ]».
            return CSSNumericType { CSSNumericType::BaseType::Percent, 1 };
        });
    // In all cases, the associated percent hint is null.
}

bool NumericCalculationNode::contains_percentage() const
{
    return m_value.has<Percentage>();
}

CSSMathValue::CalculationResult NumericCalculationNode::resolve(Optional<Length::ResolutionContext const&>, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    if (m_value.has<Percentage>()) {
        // NOTE: Depending on whether percentage_basis is set, the caller of resolve() is expecting a raw percentage or
        //       resolved length.
        return percentage_basis.visit(
            [&](Empty const&) -> CSSMathValue::CalculationResult {
                return m_value;
            },
            [&](auto const& value) {
                return CSSMathValue::CalculationResult(value.percentage_of(m_value.get<Percentage>()));
            });
    }

    return m_value;
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
    return adopt_own(*new (nothrow) SumCalculationNode(move(values)));
}

SumCalculationNode::SumCalculationNode(Vector<NonnullOwnPtr<CalculationNode>> values)
    : CalculationNode(Type::Sum)
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

Optional<CSSMathValue::ResolvedType> SumCalculationNode::resolved_type() const
{
    // FIXME: Implement https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    //        For now, this is just ad-hoc, based on the old implementation.

    Optional<CSSMathValue::ResolvedType> type;
    for (auto const& value : m_values) {
        auto maybe_value_type = value->resolved_type();
        if (!maybe_value_type.has_value())
            return {};
        auto value_type = maybe_value_type.value();

        if (!type.has_value()) {
            type = value_type;
            continue;
        }

        // At + or -, check that both sides have the same type, or that one side is a <number> and the other is an <integer>.
        // If both sides are the same type, resolve to that type.
        if (value_type == type)
            continue;

        // If one side is a <number> and the other is an <integer>, resolve to <number>.
        if (is_number(*type) && is_number(value_type)) {
            type = CSSMathValue::ResolvedType::Number;
            continue;
        }

        // FIXME: calc() handles <percentage> by allowing them to pretend to be whatever <dimension> type is allowed at this location.
        //        Since we can't easily check what that type is, we just allow <percentage> to combine with any other <dimension> type.
        if (type == CSSMathValue::ResolvedType::Percentage && is_dimension(value_type)) {
            type = value_type;
            continue;
        }
        if (is_dimension(*type) && value_type == CSSMathValue::ResolvedType::Percentage)
            continue;

        return {};
    }
    return type;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> SumCalculationNode::determine_type(PropertyID property_id) const
{
    // At a + or - sub-expression, attempt to add the types of the left and right arguments.
    // If this returns failure, the entire calculation’s type is failure.
    // Otherwise, the sub-expression’s type is the returned type.
    return add_the_types(m_values, property_id);
}

bool SumCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }
    return false;
}

CSSMathValue::CalculationResult SumCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    Optional<CSSMathValue::CalculationResult> total;

    for (auto& additional_product : m_values) {
        auto additional_value = additional_product->resolve(context, percentage_basis);
        if (!total.has_value()) {
            total = additional_value;
            continue;
        }
        total->add(additional_value, context, percentage_basis);
    }

    return total.value();
}

void SumCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    for (auto& item : m_values) {
        item->for_each_child_node(callback);
        callback(item);
    }
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
    return adopt_own(*new (nothrow) ProductCalculationNode(move(values)));
}

ProductCalculationNode::ProductCalculationNode(Vector<NonnullOwnPtr<CalculationNode>> values)
    : CalculationNode(Type::Product)
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

Optional<CSSMathValue::ResolvedType> ProductCalculationNode::resolved_type() const
{
    // FIXME: Implement https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    //        For now, this is just ad-hoc, based on the old implementation.

    Optional<CSSMathValue::ResolvedType> type;
    for (auto const& value : m_values) {
        auto maybe_value_type = value->resolved_type();
        if (!maybe_value_type.has_value())
            return {};
        auto value_type = maybe_value_type.value();

        if (!type.has_value()) {
            type = value_type;
            continue;
        }

        // At *, check that at least one side is <number>.
        if (!(is_number(*type) || is_number(value_type)))
            return {};
        // If both sides are <integer>, resolve to <integer>.
        if (type == CSSMathValue::ResolvedType::Integer && value_type == CSSMathValue::ResolvedType::Integer) {
            type = CSSMathValue::ResolvedType::Integer;
        } else {
            // Otherwise, resolve to the type of the other side.
            if (is_number(*type))
                type = value_type;
        }
    }
    return type;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> ProductCalculationNode::determine_type(PropertyID property_id) const
{
    // At a * sub-expression, multiply the types of the left and right arguments.
    // The sub-expression’s type is the returned result.
    Optional<CSSNumericType> left_type;
    for (auto const& value : m_values) {
        auto right_type = value->determine_type(property_id);
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

bool ProductCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }
    return false;
}

CSSMathValue::CalculationResult ProductCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    Optional<CSSMathValue::CalculationResult> total;

    for (auto& additional_product : m_values) {
        auto additional_value = additional_product->resolve(context, percentage_basis);
        if (!total.has_value()) {
            total = additional_value;
            continue;
        }
        total->multiply_by(additional_value, context);
    }

    return total.value();
}

void ProductCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    for (auto& item : m_values) {
        item->for_each_child_node(callback);
        callback(item);
    }
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

NonnullOwnPtr<NegateCalculationNode> NegateCalculationNode::create(NonnullOwnPtr<Web::CSS::CalculationNode> value)
{
    return adopt_own(*new (nothrow) NegateCalculationNode(move(value)));
}

NegateCalculationNode::NegateCalculationNode(NonnullOwnPtr<CalculationNode> value)
    : CalculationNode(Type::Negate)
    , m_value(move(value))
{
}

NegateCalculationNode::~NegateCalculationNode() = default;

String NegateCalculationNode::to_string() const
{
    return MUST(String::formatted("(0 - {})", m_value->to_string()));
}

Optional<CSSMathValue::ResolvedType> NegateCalculationNode::resolved_type() const
{
    return m_value->resolved_type();
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> NegateCalculationNode::determine_type(PropertyID property_id) const
{
    // NOTE: `- foo` doesn't change the type
    return m_value->determine_type(property_id);
}

bool NegateCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CSSMathValue::CalculationResult NegateCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto child_value = m_value->resolve(context, percentage_basis);
    child_value.negate();
    return child_value;
}

void NegateCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_value->for_each_child_node(callback);
    callback(m_value);
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

NonnullOwnPtr<InvertCalculationNode> InvertCalculationNode::create(NonnullOwnPtr<Web::CSS::CalculationNode> value)
{
    return adopt_own(*new (nothrow) InvertCalculationNode(move(value)));
}

InvertCalculationNode::InvertCalculationNode(NonnullOwnPtr<CalculationNode> value)
    : CalculationNode(Type::Invert)
    , m_value(move(value))
{
}

InvertCalculationNode::~InvertCalculationNode() = default;

String InvertCalculationNode::to_string() const
{
    return MUST(String::formatted("(1 / {})", m_value->to_string()));
}

Optional<CSSMathValue::ResolvedType> InvertCalculationNode::resolved_type() const
{
    auto type = m_value->resolved_type();
    if (type == CSSMathValue::ResolvedType::Integer)
        return CSSMathValue::ResolvedType::Number;
    return type;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> InvertCalculationNode::determine_type(PropertyID property_id) const
{
    // At a / sub-expression, let left type be the result of finding the types of its left argument,
    // and right type be the result of finding the types of its right argument and then inverting it.
    // The sub-expression’s type is the result of multiplying the left type and right type.
    // NOTE: An InvertCalculationNode only represents the right argument here, and the multiplication
    //       is handled in the parent ProductCalculationNode.
    return m_value->determine_type(property_id).map([](auto& it) { return it.inverted(); });
}

bool InvertCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CSSMathValue::CalculationResult InvertCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto child_value = m_value->resolve(context, percentage_basis);
    child_value.invert();
    return child_value;
}

void InvertCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_value->for_each_child_node(callback);
    callback(m_value);
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

NonnullOwnPtr<MinCalculationNode> MinCalculationNode::create(Vector<NonnullOwnPtr<Web::CSS::CalculationNode>> values)
{
    return adopt_own(*new (nothrow) MinCalculationNode(move(values)));
}

MinCalculationNode::MinCalculationNode(Vector<NonnullOwnPtr<CalculationNode>> values)
    : CalculationNode(Type::Min)
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

Optional<CSSMathValue::ResolvedType> MinCalculationNode::resolved_type() const
{
    // NOTE: We check during parsing that all values have the same type.
    return m_values[0]->resolved_type();
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> MinCalculationNode::determine_type(PropertyID property_id) const
{
    // The result of adding the types of its comma-separated calculations.
    return add_the_types(m_values, property_id);
}

bool MinCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }

    return false;
}

CSSMathValue::CalculationResult MinCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    CSSMathValue::CalculationResult smallest_node = m_values.first()->resolve(context, percentage_basis);
    auto smallest_value = resolve_value(smallest_node.value(), context);

    for (size_t i = 1; i < m_values.size(); i++) {
        auto child_resolved = m_values[i]->resolve(context, percentage_basis);
        auto child_value = resolve_value(child_resolved.value(), context);

        if (child_value < smallest_value) {
            smallest_value = child_value;
            smallest_node = child_resolved;
        }
    }

    return smallest_node;
}

void MinCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    for (auto& value : m_values) {
        value->for_each_child_node(callback);
        callback(value);
    }
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

NonnullOwnPtr<MaxCalculationNode> MaxCalculationNode::create(Vector<NonnullOwnPtr<Web::CSS::CalculationNode>> values)
{
    return adopt_own(*new (nothrow) MaxCalculationNode(move(values)));
}

MaxCalculationNode::MaxCalculationNode(Vector<NonnullOwnPtr<CalculationNode>> values)
    : CalculationNode(Type::Max)
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

Optional<CSSMathValue::ResolvedType> MaxCalculationNode::resolved_type() const
{
    // NOTE: We check during parsing that all values have the same type.
    return m_values[0]->resolved_type();
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> MaxCalculationNode::determine_type(PropertyID property_id) const
{
    // The result of adding the types of its comma-separated calculations.
    return add_the_types(m_values, property_id);
}

bool MaxCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }

    return false;
}

CSSMathValue::CalculationResult MaxCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    CSSMathValue::CalculationResult largest_node = m_values.first()->resolve(context, percentage_basis);
    auto largest_value = resolve_value(largest_node.value(), context);

    for (size_t i = 1; i < m_values.size(); i++) {
        auto child_resolved = m_values[i]->resolve(context, percentage_basis);
        auto child_value = resolve_value(child_resolved.value(), context);

        if (child_value > largest_value) {
            largest_value = child_value;
            largest_node = child_resolved;
        }
    }

    return largest_node;
}

void MaxCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    for (auto& value : m_values) {
        value->for_each_child_node(callback);
        callback(value);
    }
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
    return adopt_own(*new (nothrow) ClampCalculationNode(move(min), move(center), move(max)));
}

ClampCalculationNode::ClampCalculationNode(NonnullOwnPtr<CalculationNode> min, NonnullOwnPtr<CalculationNode> center, NonnullOwnPtr<CalculationNode> max)
    : CalculationNode(Type::Clamp)
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

Optional<CSSMathValue::ResolvedType> ClampCalculationNode::resolved_type() const
{
    // NOTE: We check during parsing that all values have the same type.
    return m_min_value->resolved_type();
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> ClampCalculationNode::determine_type(PropertyID property_id) const
{
    // The result of adding the types of its comma-separated calculations.
    auto min_type = m_min_value->determine_type(property_id);
    auto center_type = m_center_value->determine_type(property_id);
    auto max_type = m_max_value->determine_type(property_id);

    if (!min_type.has_value() || !center_type.has_value() || !max_type.has_value())
        return {};

    auto result = min_type->added_to(*center_type);
    if (!result.has_value())
        return {};
    return result->added_to(*max_type);
}

bool ClampCalculationNode::contains_percentage() const
{
    return m_min_value->contains_percentage() || m_center_value->contains_percentage() || m_max_value->contains_percentage();
}

CSSMathValue::CalculationResult ClampCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto min_node = m_min_value->resolve(context, percentage_basis);
    auto center_node = m_center_value->resolve(context, percentage_basis);
    auto max_node = m_max_value->resolve(context, percentage_basis);

    auto min_value = resolve_value(min_node.value(), context);
    auto center_value = resolve_value(center_node.value(), context);
    auto max_value = resolve_value(max_node.value(), context);

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

void ClampCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_min_value->for_each_child_node(callback);
    m_center_value->for_each_child_node(callback);
    m_max_value->for_each_child_node(callback);
    callback(m_min_value);
    callback(m_center_value);
    callback(m_max_value);
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
    : CalculationNode(Type::Abs)
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

Optional<CSSMathValue::ResolvedType> AbsCalculationNode::resolved_type() const
{
    return m_value->resolved_type();
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> AbsCalculationNode::determine_type(PropertyID property_id) const
{
    // The type of its contained calculation.
    return m_value->determine_type(property_id);
}

bool AbsCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CSSMathValue::CalculationResult AbsCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto resolved_type = m_value->resolved_type().value();
    auto node_a = m_value->resolve(context, percentage_basis);
    auto node_a_value = resolve_value(node_a.value(), context);

    if (node_a_value < 0)
        return to_resolved_type(resolved_type, -node_a_value);

    return node_a;
}

void AbsCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_value->for_each_child_node(callback);
    callback(m_value);
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
    : CalculationNode(Type::Sign)
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

Optional<CSSMathValue::ResolvedType> SignCalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Integer;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> SignCalculationNode::determine_type(PropertyID) const
{
    // «[ ]» (empty map).
    return CSSNumericType {};
}

bool SignCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CSSMathValue::CalculationResult SignCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_value->resolve(context, percentage_basis);
    auto node_a_value = resolve_value(node_a.value(), context);

    if (node_a_value < 0)
        return { Number(Number::Type::Integer, -1) };

    if (node_a_value > 0)
        return { Number(Number::Type::Integer, 1) };

    return { Number(Number::Type::Integer, 0) };
}

void SignCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_value->for_each_child_node(callback);
    callback(m_value);
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
    : CalculationNode(Type::Constant)
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
Optional<CSSMathValue::ResolvedType> ConstantCalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Number;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> ConstantCalculationNode::determine_type(PropertyID) const
{
    // Anything else is a terminal value, whose type is determined based on its CSS type:
    // -> <calc-constant>
    //    the type is «[ ]» (empty map)
    return CSSNumericType {};
}

CSSMathValue::CalculationResult ConstantCalculationNode::resolve([[maybe_unused]] Optional<Length::ResolutionContext const&> context, [[maybe_unused]] CSSMathValue::PercentageBasis const& percentage_basis) const
{
    switch (m_constant) {
    case CalculationNode::ConstantType::E:
        return { Number(Number::Type::Number, M_E) };
    case CalculationNode::ConstantType::Pi:
        return { Number(Number::Type::Number, M_PI) };
    // FIXME: We need to keep track of Infinity and NaN across all nodes, since they require special handling.
    case CalculationNode::ConstantType::Infinity:
        return { Number(Number::Type::Number, NumericLimits<double>::max()) };
    case CalculationNode::ConstantType::MinusInfinity:
        return { Number(Number::Type::Number, NumericLimits<double>::lowest()) };
    case CalculationNode::ConstantType::NaN:
        return { Number(Number::Type::Number, NAN) };
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
    : CalculationNode(Type::Sin)
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

Optional<CSSMathValue::ResolvedType> SinCalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Number;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> SinCalculationNode::determine_type(PropertyID) const
{
    // «[ ]» (empty map).
    return CSSNumericType {};
}

bool SinCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CSSMathValue::CalculationResult SinCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_value->resolve(context, percentage_basis);
    auto node_a_value = resolve_value_radians(node_a.value());
    auto result = sin(node_a_value);

    return { Number(Number::Type::Number, result) };
}

void SinCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_value->for_each_child_node(callback);
    callback(m_value);
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
    : CalculationNode(Type::Cos)
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

Optional<CSSMathValue::ResolvedType> CosCalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Number;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> CosCalculationNode::determine_type(PropertyID) const
{
    // «[ ]» (empty map).
    return CSSNumericType {};
}

bool CosCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CSSMathValue::CalculationResult CosCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_value->resolve(context, percentage_basis);
    auto node_a_value = resolve_value_radians(node_a.value());
    auto result = cos(node_a_value);

    return { Number(Number::Type::Number, result) };
}

void CosCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_value->for_each_child_node(callback);
    callback(m_value);
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
    : CalculationNode(Type::Tan)
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

Optional<CSSMathValue::ResolvedType> TanCalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Number;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> TanCalculationNode::determine_type(PropertyID) const
{
    // «[ ]» (empty map).
    return CSSNumericType {};
}

bool TanCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CSSMathValue::CalculationResult TanCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_value->resolve(context, percentage_basis);
    auto node_a_value = resolve_value_radians(node_a.value());
    auto result = tan(node_a_value);

    return { Number(Number::Type::Number, result) };
}

void TanCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_value->for_each_child_node(callback);
    callback(m_value);
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
    : CalculationNode(Type::Asin)
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

Optional<CSSMathValue::ResolvedType> AsinCalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Angle;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> AsinCalculationNode::determine_type(PropertyID) const
{
    // «[ "angle" → 1 ]».
    return CSSNumericType { CSSNumericType::BaseType::Angle, 1 };
}

bool AsinCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CSSMathValue::CalculationResult AsinCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_value->resolve(context, percentage_basis);
    auto node_a_value = resolve_value(node_a.value(), context);
    auto result = asin(node_a_value);

    return { Angle(result, Angle::Type::Rad) };
}

void AsinCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_value->for_each_child_node(callback);
    callback(m_value);
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
    : CalculationNode(Type::Acos)
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

Optional<CSSMathValue::ResolvedType> AcosCalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Angle;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> AcosCalculationNode::determine_type(PropertyID) const
{
    // «[ "angle" → 1 ]».
    return CSSNumericType { CSSNumericType::BaseType::Angle, 1 };
}

bool AcosCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CSSMathValue::CalculationResult AcosCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_value->resolve(context, percentage_basis);
    auto node_a_value = resolve_value(node_a.value(), context);
    auto result = acos(node_a_value);

    return { Angle(result, Angle::Type::Rad) };
}

void AcosCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_value->for_each_child_node(callback);
    callback(m_value);
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
    : CalculationNode(Type::Atan)
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

Optional<CSSMathValue::ResolvedType> AtanCalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Angle;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> AtanCalculationNode::determine_type(PropertyID) const
{
    // «[ "angle" → 1 ]».
    return CSSNumericType { CSSNumericType::BaseType::Angle, 1 };
}

bool AtanCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

CSSMathValue::CalculationResult AtanCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_value->resolve(context, percentage_basis);
    auto node_a_value = resolve_value(node_a.value(), context);
    auto result = atan(node_a_value);

    return { Angle(result, Angle::Type::Rad) };
}

void AtanCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_value->for_each_child_node(callback);
    callback(m_value);
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
    : CalculationNode(Type::Atan2)
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

Optional<CSSMathValue::ResolvedType> Atan2CalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Angle;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> Atan2CalculationNode::determine_type(PropertyID) const
{
    // «[ "angle" → 1 ]».
    return CSSNumericType { CSSNumericType::BaseType::Angle, 1 };
}

bool Atan2CalculationNode::contains_percentage() const
{
    return m_y->contains_percentage() || m_x->contains_percentage();
}

CSSMathValue::CalculationResult Atan2CalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_y->resolve(context, percentage_basis);
    auto node_a_value = resolve_value(node_a.value(), context);

    auto node_b = m_x->resolve(context, percentage_basis);
    auto node_b_value = resolve_value(node_b.value(), context);

    auto result = atan2(node_a_value, node_b_value);

    return { Angle(result, Angle::Type::Rad) };
}

void Atan2CalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_y->for_each_child_node(callback);
    m_x->for_each_child_node(callback);
    callback(m_y);
    callback(m_x);
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
    : CalculationNode(Type::Pow)
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

Optional<CSSMathValue::ResolvedType> PowCalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Number;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> PowCalculationNode::determine_type(PropertyID) const
{
    // «[ ]» (empty map).
    return CSSNumericType {};
}

CSSMathValue::CalculationResult PowCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_x->resolve(context, percentage_basis);
    auto node_a_value = resolve_value(node_a.value(), context);

    auto node_b = m_y->resolve(context, percentage_basis);
    auto node_b_value = resolve_value(node_b.value(), context);

    auto result = pow(node_a_value, node_b_value);

    return { Number(Number::Type::Number, result) };
}

void PowCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_x->for_each_child_node(callback);
    m_y->for_each_child_node(callback);
    callback(m_x);
    callback(m_y);
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
    : CalculationNode(Type::Sqrt)
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

Optional<CSSMathValue::ResolvedType> SqrtCalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Number;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> SqrtCalculationNode::determine_type(PropertyID) const
{
    // «[ ]» (empty map).
    return CSSNumericType {};
}

CSSMathValue::CalculationResult SqrtCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_value->resolve(context, percentage_basis);
    auto node_a_value = resolve_value(node_a.value(), context);
    auto result = sqrt(node_a_value);

    return { Number(Number::Type::Number, result) };
}

void SqrtCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_value->for_each_child_node(callback);
    callback(m_value);
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

NonnullOwnPtr<HypotCalculationNode> HypotCalculationNode::create(Vector<NonnullOwnPtr<Web::CSS::CalculationNode>> values)
{
    return adopt_own(*new (nothrow) HypotCalculationNode(move(values)));
}

HypotCalculationNode::HypotCalculationNode(Vector<NonnullOwnPtr<CalculationNode>> values)
    : CalculationNode(Type::Hypot)
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

Optional<CSSMathValue::ResolvedType> HypotCalculationNode::resolved_type() const
{
    // NOTE: We check during parsing that all values have the same type.
    return m_values[0]->resolved_type();
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> HypotCalculationNode::determine_type(PropertyID property_id) const
{
    // The result of adding the types of its comma-separated calculations.
    return add_the_types(m_values, property_id);
}

bool HypotCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }

    return false;
}

CSSMathValue::CalculationResult HypotCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    double square_sum = 0.0;

    for (auto const& value : m_values) {
        auto child_resolved = value->resolve(context, percentage_basis);
        auto child_value = resolve_value(child_resolved.value(), context);

        square_sum += child_value * child_value;
    }

    auto result = sqrt(square_sum);

    return to_resolved_type(resolved_type().value(), result);
}

void HypotCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    for (auto& value : m_values) {
        value->for_each_child_node(callback);
        callback(value);
    }
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
    : CalculationNode(Type::Log)
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

Optional<CSSMathValue::ResolvedType> LogCalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Number;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> LogCalculationNode::determine_type(PropertyID) const
{
    // «[ ]» (empty map).
    return CSSNumericType {};
}

CSSMathValue::CalculationResult LogCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_x->resolve(context, percentage_basis);
    auto node_a_value = resolve_value(node_a.value(), context);

    auto node_b = m_y->resolve(context, percentage_basis);
    auto node_b_value = resolve_value(node_b.value(), context);

    auto result = log2(node_a_value) / log2(node_b_value);

    return { Number(Number::Type::Number, result) };
}

void LogCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_x->for_each_child_node(callback);
    m_y->for_each_child_node(callback);
    callback(m_x);
    callback(m_y);
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
    : CalculationNode(Type::Exp)
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

Optional<CSSMathValue::ResolvedType> ExpCalculationNode::resolved_type() const
{
    return CSSMathValue::ResolvedType::Number;
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> ExpCalculationNode::determine_type(PropertyID) const
{
    // «[ ]» (empty map).
    return CSSNumericType {};
}

CSSMathValue::CalculationResult ExpCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_value->resolve(context, percentage_basis);
    auto node_a_value = resolve_value(node_a.value(), context);
    auto result = exp(node_a_value);

    return { Number(Number::Type::Number, result) };
}

void ExpCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_value->for_each_child_node(callback);
    callback(m_value);
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
    return adopt_own(*new (nothrow) RoundCalculationNode(strategy, move(x), move(y)));
}

RoundCalculationNode::RoundCalculationNode(RoundingStrategy mode, NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y)
    : CalculationNode(Type::Round)
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

Optional<CSSMathValue::ResolvedType> RoundCalculationNode::resolved_type() const
{
    // Note: We check during parsing that all values have the same type
    return m_x->resolved_type();
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> RoundCalculationNode::determine_type(PropertyID property_id) const
{
    // The result of adding the types of its comma-separated calculations.
    auto x_type = m_x->determine_type(property_id);
    auto y_type = m_y->determine_type(property_id);

    if (!x_type.has_value() || !y_type.has_value())
        return {};

    return x_type->added_to(*y_type);
}

bool RoundCalculationNode::contains_percentage() const
{
    return m_x->contains_percentage() || m_y->contains_percentage();
}

CSSMathValue::CalculationResult RoundCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto node_a = m_x->resolve(context, percentage_basis);
    auto node_b = m_y->resolve(context, percentage_basis);

    auto node_a_value = resolve_value(node_a.value(), context);
    auto node_b_value = resolve_value(node_b.value(), context);

    auto upper_b = ceil(node_a_value / node_b_value) * node_b_value;
    auto lower_b = floor(node_a_value / node_b_value) * node_b_value;

    auto resolved_type = node_a.resolved_type();

    if (m_strategy == RoundingStrategy::Nearest) {
        auto upper_diff = fabs(upper_b - node_a_value);
        auto lower_diff = fabs(node_a_value - lower_b);
        auto rounded_value = upper_diff < lower_diff ? upper_b : lower_b;
        return to_resolved_type(resolved_type, rounded_value);
    }

    if (m_strategy == RoundingStrategy::Up) {
        return to_resolved_type(resolved_type, upper_b);
    }

    if (m_strategy == RoundingStrategy::Down) {
        return to_resolved_type(resolved_type, lower_b);
    }

    if (m_strategy == RoundingStrategy::ToZero) {
        auto upper_diff = fabs(upper_b);
        auto lower_diff = fabs(lower_b);
        auto rounded_value = upper_diff < lower_diff ? upper_b : lower_b;
        return to_resolved_type(resolved_type, rounded_value);
    }

    VERIFY_NOT_REACHED();
}

void RoundCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_x->for_each_child_node(callback);
    m_y->for_each_child_node(callback);
    callback(m_x);
    callback(m_y);
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
    return adopt_own(*new (nothrow) ModCalculationNode(move(x), move(y)));
}

ModCalculationNode::ModCalculationNode(NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y)
    : CalculationNode(Type::Mod)
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

Optional<CSSMathValue::ResolvedType> ModCalculationNode::resolved_type() const
{
    // Note: We check during parsing that all values have the same type
    return m_x->resolved_type();
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> ModCalculationNode::determine_type(PropertyID property_id) const
{
    // The result of adding the types of its comma-separated calculations.
    auto x_type = m_x->determine_type(property_id);
    auto y_type = m_y->determine_type(property_id);

    if (!x_type.has_value() || !y_type.has_value())
        return {};

    return x_type->added_to(*y_type);
}

bool ModCalculationNode::contains_percentage() const
{
    return m_x->contains_percentage() || m_y->contains_percentage();
}

CSSMathValue::CalculationResult ModCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto resolved_type = m_x->resolved_type().value();
    auto node_a = m_x->resolve(context, percentage_basis);
    auto node_b = m_y->resolve(context, percentage_basis);

    auto node_a_value = resolve_value(node_a.value(), context);
    auto node_b_value = resolve_value(node_b.value(), context);

    auto quotient = floor(node_a_value / node_b_value);
    auto value = node_a_value - (node_b_value * quotient);
    return to_resolved_type(resolved_type, value);
}

void ModCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_x->for_each_child_node(callback);
    m_y->for_each_child_node(callback);
    callback(m_x);
    callback(m_y);
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
    return adopt_own(*new (nothrow) RemCalculationNode(move(x), move(y)));
}

RemCalculationNode::RemCalculationNode(NonnullOwnPtr<CalculationNode> x, NonnullOwnPtr<CalculationNode> y)
    : CalculationNode(Type::Rem)
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

Optional<CSSMathValue::ResolvedType> RemCalculationNode::resolved_type() const
{
    // Note: We check during parsing that all values have the same type
    return m_x->resolved_type();
}

// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
Optional<CSSNumericType> RemCalculationNode::determine_type(PropertyID property_id) const
{
    // The result of adding the types of its comma-separated calculations.
    auto x_type = m_x->determine_type(property_id);
    auto y_type = m_y->determine_type(property_id);

    if (!x_type.has_value() || !y_type.has_value())
        return {};

    return x_type->added_to(*y_type);
}

bool RemCalculationNode::contains_percentage() const
{
    return m_x->contains_percentage() || m_y->contains_percentage();
}

CSSMathValue::CalculationResult RemCalculationNode::resolve(Optional<Length::ResolutionContext const&> context, CSSMathValue::PercentageBasis const& percentage_basis) const
{
    auto resolved_type = m_x->resolved_type().value();
    auto node_a = m_x->resolve(context, percentage_basis);
    auto node_b = m_y->resolve(context, percentage_basis);

    auto node_a_value = resolve_value(node_a.value(), context);
    auto node_b_value = resolve_value(node_b.value(), context);

    auto value = fmod(node_a_value, node_b_value);
    return to_resolved_type(resolved_type, value);
}

void RemCalculationNode::for_each_child_node(Function<void(NonnullOwnPtr<CalculationNode>&)> const& callback)
{
    m_x->for_each_child_node(callback);
    m_y->for_each_child_node(callback);
    callback(m_x);
    callback(m_y);
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

void CSSMathValue::CalculationResult::add(CalculationResult const& other, Optional<Length::ResolutionContext const&> context, PercentageBasis const& percentage_basis)
{
    add_or_subtract_internal(SumOperation::Add, other, context, percentage_basis);
}

void CSSMathValue::CalculationResult::subtract(CalculationResult const& other, Optional<Length::ResolutionContext const&> context, PercentageBasis const& percentage_basis)
{
    add_or_subtract_internal(SumOperation::Subtract, other, context, percentage_basis);
}

void CSSMathValue::CalculationResult::add_or_subtract_internal(SumOperation op, CalculationResult const& other, Optional<Length::ResolutionContext const&> context, PercentageBasis const& percentage_basis)
{
    // We know from validation when resolving the type, that "both sides have the same type, or that one side is a <number> and the other is an <integer>".
    // Though, having the same type may mean that one side is a <dimension> and the other a <percentage>.
    // Note: This is almost identical to ::add()

    m_value.visit(
        [&](Number const& number) {
            auto other_number = other.m_value.get<Number>();
            if (op == SumOperation::Add) {
                m_value = number + other_number;
            } else {
                m_value = number - other_number;
            }
        },
        [&](Angle const& angle) {
            auto this_degrees = angle.to_degrees();
            if (other.m_value.has<Angle>()) {
                auto other_degrees = other.m_value.get<Angle>().to_degrees();
                if (op == SumOperation::Add)
                    m_value = Angle::make_degrees(this_degrees + other_degrees);
                else
                    m_value = Angle::make_degrees(this_degrees - other_degrees);
            } else {
                VERIFY(percentage_basis.has<Angle>());

                auto other_degrees = percentage_basis.get<Angle>().percentage_of(other.m_value.get<Percentage>()).to_degrees();
                if (op == SumOperation::Add)
                    m_value = Angle::make_degrees(this_degrees + other_degrees);
                else
                    m_value = Angle::make_degrees(this_degrees - other_degrees);
            }
        },
        [&](Flex const& flex) {
            auto this_fr = flex.to_fr();
            if (other.m_value.has<Flex>()) {
                auto other_fr = other.m_value.get<Flex>().to_fr();
                if (op == SumOperation::Add)
                    m_value = Flex::make_fr(this_fr + other_fr);
                else
                    m_value = Flex::make_fr(this_fr - other_fr);
            } else {
                VERIFY(percentage_basis.has<Flex>());

                auto other_fr = percentage_basis.get<Flex>().percentage_of(other.m_value.get<Percentage>()).to_fr();
                if (op == SumOperation::Add)
                    m_value = Flex::make_fr(this_fr + other_fr);
                else
                    m_value = Flex::make_fr(this_fr - other_fr);
            }
        },
        [&](Frequency const& frequency) {
            auto this_hertz = frequency.to_hertz();
            if (other.m_value.has<Frequency>()) {
                auto other_hertz = other.m_value.get<Frequency>().to_hertz();
                if (op == SumOperation::Add)
                    m_value = Frequency::make_hertz(this_hertz + other_hertz);
                else
                    m_value = Frequency::make_hertz(this_hertz - other_hertz);
            } else {
                VERIFY(percentage_basis.has<Frequency>());

                auto other_hertz = percentage_basis.get<Frequency>().percentage_of(other.m_value.get<Percentage>()).to_hertz();
                if (op == SumOperation::Add)
                    m_value = Frequency::make_hertz(this_hertz + other_hertz);
                else
                    m_value = Frequency::make_hertz(this_hertz - other_hertz);
            }
        },
        [&](Length const& length) {
            if (!context.has_value()) {
                dbgln("CSSMathValue::CalculationResult::add_or_subtract_internal: Length without context");
                m_value = Length::make_px(0);
                return;
            }

            auto this_px = length.to_px(*context);
            if (other.m_value.has<Length>()) {
                auto other_px = other.m_value.get<Length>().to_px(*context);
                if (op == SumOperation::Add)
                    m_value = Length::make_px(this_px + other_px);
                else
                    m_value = Length::make_px(this_px - other_px);
            } else {
                VERIFY(percentage_basis.has<Length>());

                auto other_px = percentage_basis.get<Length>().percentage_of(other.m_value.get<Percentage>()).to_px(*context);
                if (op == SumOperation::Add)
                    m_value = Length::make_px(this_px + other_px);
                else
                    m_value = Length::make_px(this_px - other_px);
            }
        },
        [&](Resolution const& resolution) {
            auto this_dots_per_pixel = resolution.to_dots_per_pixel();
            // NOTE: <resolution-percentage> is not a type, so we don't have to worry about percentages.
            auto other_dots_per_pixel = other.m_value.get<Resolution>().to_dots_per_pixel();
            if (op == SumOperation::Add)
                m_value = Resolution::make_dots_per_pixel(this_dots_per_pixel + other_dots_per_pixel);
            else
                m_value = Resolution::make_dots_per_pixel(this_dots_per_pixel - other_dots_per_pixel);
        },
        [&](Time const& time) {
            auto this_seconds = time.to_seconds();
            if (other.m_value.has<Time>()) {
                auto other_seconds = other.m_value.get<Time>().to_seconds();
                if (op == SumOperation::Add)
                    m_value = Time::make_seconds(this_seconds + other_seconds);
                else
                    m_value = Time::make_seconds(this_seconds - other_seconds);
            } else {
                VERIFY(percentage_basis.has<Time>());

                auto other_seconds = percentage_basis.get<Time>().percentage_of(other.m_value.get<Percentage>()).to_seconds();
                if (op == SumOperation::Add)
                    m_value = Time::make_seconds(this_seconds + other_seconds);
                else
                    m_value = Time::make_seconds(this_seconds - other_seconds);
            }
        },
        [&](Percentage const& percentage) {
            if (other.m_value.has<Percentage>()) {
                if (op == SumOperation::Add)
                    m_value = Percentage { percentage.value() + other.m_value.get<Percentage>().value() };
                else
                    m_value = Percentage { percentage.value() - other.m_value.get<Percentage>().value() };
                return;
            }

            // Other side isn't a percentage, so the easiest way to handle it without duplicating all the logic, is just to swap `this` and `other`.
            CalculationResult new_value = other;
            if (op == SumOperation::Add) {
                new_value.add(*this, context, percentage_basis);
            } else {
                // Turn 'this - other' into '-other + this', as 'A + B == B + A', but 'A - B != B - A'
                new_value.multiply_by({ Number { Number::Type::Integer, -1.0f } }, context);
                new_value.add(*this, context, percentage_basis);
            }

            *this = new_value;
        });
}

void CSSMathValue::CalculationResult::multiply_by(CalculationResult const& other, Optional<Length::ResolutionContext const&> context)
{
    // We know from validation when resolving the type, that at least one side must be a <number> or <integer>.
    // Both of these are represented as a double.
    VERIFY(m_value.has<Number>() || other.m_value.has<Number>());
    bool other_is_number = other.m_value.has<Number>();

    m_value.visit(
        [&](Number const& number) {
            if (other_is_number) {
                m_value = number * other.m_value.get<Number>();
            } else {
                // Avoid duplicating all the logic by swapping `this` and `other`.
                CalculationResult new_value = other;
                new_value.multiply_by(*this, context);
                *this = new_value;
            }
        },
        [&](Angle const& angle) {
            m_value = Angle::make_degrees(angle.to_degrees() * other.m_value.get<Number>().value());
        },
        [&](Flex const& flex) {
            m_value = Flex::make_fr(flex.to_fr() * other.m_value.get<Number>().value());
        },
        [&](Frequency const& frequency) {
            m_value = Frequency::make_hertz(frequency.to_hertz() * other.m_value.get<Number>().value());
        },
        [&](Length const& length) {
            m_value = Length::make_px(CSSPixels::nearest_value_for(length.to_px(*context) * static_cast<double>(other.m_value.get<Number>().value())));
        },
        [&](Resolution const& resolution) {
            m_value = Resolution::make_dots_per_pixel(resolution.to_dots_per_pixel() * other.m_value.get<Number>().value());
        },
        [&](Time const& time) {
            m_value = Time::make_seconds(time.to_seconds() * other.m_value.get<Number>().value());
        },
        [&](Percentage const& percentage) {
            m_value = Percentage { percentage.value() * other.m_value.get<Number>().value() };
        });
}

void CSSMathValue::CalculationResult::divide_by(CalculationResult const& other, Optional<Length::ResolutionContext const&> context)
{
    // We know from validation when resolving the type, that `other` must be a <number> or <integer>.
    // Both of these are represented as a Number.
    auto denominator = other.m_value.get<Number>().value();
    // FIXME: Dividing by 0 is invalid, and should be caught during parsing.
    VERIFY(denominator != 0.0);

    m_value.visit(
        [&](Number const& number) {
            m_value = Number {
                Number::Type::Number,
                number.value() / denominator
            };
        },
        [&](Angle const& angle) {
            m_value = Angle::make_degrees(angle.to_degrees() / denominator);
        },
        [&](Flex const& flex) {
            m_value = Flex::make_fr(flex.to_fr() / denominator);
        },
        [&](Frequency const& frequency) {
            m_value = Frequency::make_hertz(frequency.to_hertz() / denominator);
        },
        [&](Length const& length) {
            m_value = Length::make_px(CSSPixels::nearest_value_for(length.to_px(*context) / static_cast<double>(denominator)));
        },
        [&](Resolution const& resolution) {
            m_value = Resolution::make_dots_per_pixel(resolution.to_dots_per_pixel() / denominator);
        },
        [&](Time const& time) {
            m_value = Time::make_seconds(time.to_seconds() / denominator);
        },
        [&](Percentage const& percentage) {
            m_value = Percentage { percentage.value() / denominator };
        });
}

void CSSMathValue::CalculationResult::negate()
{
    m_value.visit(
        [&](Number const& number) {
            m_value = Number { number.type(), 0 - number.value() };
        },
        [&](Angle const& angle) {
            m_value = Angle { 0 - angle.raw_value(), angle.type() };
        },
        [&](Flex const& flex) {
            m_value = Flex { 0 - flex.raw_value(), flex.type() };
        },
        [&](Frequency const& frequency) {
            m_value = Frequency { 0 - frequency.raw_value(), frequency.type() };
        },
        [&](Length const& length) {
            m_value = Length { 0 - length.raw_value(), length.type() };
        },
        [&](Resolution const& resolution) {
            m_value = Resolution { 0 - resolution.raw_value(), resolution.type() };
        },
        [&](Time const& time) {
            m_value = Time { 0 - time.raw_value(), time.type() };
        },
        [&](Percentage const& percentage) {
            m_value = Percentage { 0 - percentage.value() };
        });
}

void CSSMathValue::CalculationResult::invert()
{
    // FIXME: Correctly handle division by zero.
    m_value.visit(
        [&](Number const& number) {
            m_value = Number { Number::Type::Number, 1 / number.value() };
        },
        [&](Angle const& angle) {
            m_value = Angle { 1 / angle.raw_value(), angle.type() };
        },
        [&](Flex const& flex) {
            m_value = Flex { 1 / flex.raw_value(), flex.type() };
        },
        [&](Frequency const& frequency) {
            m_value = Frequency { 1 / frequency.raw_value(), frequency.type() };
        },
        [&](Length const& length) {
            m_value = Length { 1 / length.raw_value(), length.type() };
        },
        [&](Resolution const& resolution) {
            m_value = Resolution { 1 / resolution.raw_value(), resolution.type() };
        },
        [&](Time const& time) {
            m_value = Time { 1 / time.raw_value(), time.type() };
        },
        [&](Percentage const& percentage) {
            m_value = Percentage { 1 / percentage.value() };
        });
}

CSSMathValue::ResolvedType CSSMathValue::CalculationResult::resolved_type() const
{
    return m_value.visit(
        [](Number const&) { return ResolvedType::Number; },
        [](Angle const&) { return ResolvedType::Angle; },
        [](Flex const&) { return ResolvedType::Flex; },
        [](Frequency const&) { return ResolvedType::Frequency; },
        [](Length const&) { return ResolvedType::Length; },
        [](Percentage const&) { return ResolvedType::Percentage; },
        [](Resolution const&) { return ResolvedType::Resolution; },
        [](Time const&) { return ResolvedType::Time; });
}

String CSSMathValue::to_string() const
{
    // FIXME: Implement this according to https://www.w3.org/TR/css-values-4/#calc-serialize once that stabilizes.
    return MUST(String::formatted("calc({})", m_calculation->to_string()));
}

bool CSSMathValue::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;

    return m_calculation->equals(*static_cast<CSSMathValue const&>(other).m_calculation);
}

Optional<Angle> CSSMathValue::resolve_angle() const
{
    auto result = m_calculation->resolve({}, {});

    if (result.value().has<Angle>())
        return result.value().get<Angle>();
    return {};
}

Optional<Angle> CSSMathValue::resolve_angle(Layout::Node const& layout_node) const
{
    return resolve_angle(Length::ResolutionContext::for_layout_node(layout_node));
}

Optional<Angle> CSSMathValue::resolve_angle(Length::ResolutionContext const& context) const
{
    auto result = m_calculation->resolve(context, {});

    if (result.value().has<Angle>())
        return result.value().get<Angle>();
    return {};
}

Optional<Angle> CSSMathValue::resolve_angle_percentage(Angle const& percentage_basis) const
{
    auto result = m_calculation->resolve({}, percentage_basis);

    return result.value().visit(
        [&](Angle const& angle) -> Optional<Angle> {
            return angle;
        },
        [&](Percentage const& percentage) -> Optional<Angle> {
            return percentage_basis.percentage_of(percentage);
        },
        [&](auto const&) -> Optional<Angle> {
            return {};
        });
}

Optional<Flex> CSSMathValue::resolve_flex() const
{
    auto result = m_calculation->resolve({}, {});

    if (result.value().has<Flex>())
        return result.value().get<Flex>();
    return {};
}

Optional<Frequency> CSSMathValue::resolve_frequency() const
{
    auto result = m_calculation->resolve({}, {});

    if (result.value().has<Frequency>())
        return result.value().get<Frequency>();
    return {};
}

Optional<Frequency> CSSMathValue::resolve_frequency_percentage(Frequency const& percentage_basis) const
{
    auto result = m_calculation->resolve({}, percentage_basis);

    return result.value().visit(
        [&](Frequency const& frequency) -> Optional<Frequency> {
            return frequency;
        },
        [&](Percentage const& percentage) -> Optional<Frequency> {
            return percentage_basis.percentage_of(percentage);
        },
        [&](auto const&) -> Optional<Frequency> {
            return {};
        });
}

Optional<Length> CSSMathValue::resolve_length(Length::ResolutionContext const& context) const
{
    auto result = m_calculation->resolve(context, {});

    if (result.value().has<Length>())
        return result.value().get<Length>();
    return {};
}

Optional<Length> CSSMathValue::resolve_length(Layout::Node const& layout_node) const
{
    return resolve_length(Length::ResolutionContext::for_layout_node(layout_node));
}

Optional<Length> CSSMathValue::resolve_length_percentage(Layout::Node const& layout_node, Length const& percentage_basis) const
{
    return resolve_length_percentage(Length::ResolutionContext::for_layout_node(layout_node), percentage_basis);
}

Optional<Length> CSSMathValue::resolve_length_percentage(Layout::Node const& layout_node, CSSPixels percentage_basis) const
{
    return resolve_length_percentage(Length::ResolutionContext::for_layout_node(layout_node), Length::make_px(percentage_basis));
}

Optional<Length> CSSMathValue::resolve_length_percentage(Length::ResolutionContext const& resolution_context, Length const& percentage_basis) const
{
    auto result = m_calculation->resolve(resolution_context, percentage_basis);

    return result.value().visit(
        [&](Length const& length) -> Optional<Length> {
            return length;
        },
        [&](Percentage const& percentage) -> Optional<Length> {
            return percentage_basis.percentage_of(percentage);
        },
        [&](auto const&) -> Optional<Length> {
            return {};
        });
}

Optional<Percentage> CSSMathValue::resolve_percentage() const
{
    auto result = m_calculation->resolve({}, {});
    if (result.value().has<Percentage>())
        return result.value().get<Percentage>();
    return {};
}

Optional<Resolution> CSSMathValue::resolve_resolution() const
{
    auto result = m_calculation->resolve({}, {});
    if (result.value().has<Resolution>())
        return result.value().get<Resolution>();
    return {};
}

Optional<Time> CSSMathValue::resolve_time() const
{
    auto result = m_calculation->resolve({}, {});

    if (result.value().has<Time>())
        return result.value().get<Time>();
    return {};
}

Optional<Time> CSSMathValue::resolve_time_percentage(Time const& percentage_basis) const
{
    auto result = m_calculation->resolve({}, percentage_basis);

    return result.value().visit(
        [&](Time const& time) -> Optional<Time> {
            return time;
        },
        [&](auto const&) -> Optional<Time> {
            return {};
        });
}

Optional<double> CSSMathValue::resolve_number() const
{
    auto result = m_calculation->resolve({}, {});
    if (result.value().has<Number>())
        return result.value().get<Number>().value();
    return {};
}

Optional<i64> CSSMathValue::resolve_integer() const
{
    auto result = m_calculation->resolve({}, {});
    if (result.value().has<Number>())
        return result.value().get<Number>().integer_value();
    return {};
}

bool CSSMathValue::contains_percentage() const
{
    return m_calculation->contains_percentage();
}

String CSSMathValue::dump() const
{
    StringBuilder builder;
    m_calculation->dump(builder, 0);
    return builder.to_string_without_validation();
}

}
