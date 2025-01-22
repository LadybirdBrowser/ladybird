/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/CSSNumericType.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Flex.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

class CalculationNode;

// https://drafts.csswg.org/css-values-4/#ref-for-calc-calculation%E2%91%A2%E2%91%A7
struct CalculationContext {
    Optional<ValueType> percentages_resolve_as {};
};

class CalculatedStyleValue : public CSSStyleValue {
public:
    using PercentageBasis = Variant<Empty, Angle, Frequency, Length, Time>;

    class CalculationResult {
    public:
        using Value = Variant<Number, Angle, Flex, Frequency, Length, Percentage, Resolution, Time>;
        static CalculationResult from_value(Value const&, Optional<Length::ResolutionContext const&>, Optional<CSSNumericType>);

        CalculationResult(double value, Optional<CSSNumericType> type)
            : m_value(value)
            , m_type(move(type))
        {
        }

        void add(CalculationResult const& other, Optional<Length::ResolutionContext const&>, PercentageBasis const& percentage_basis);
        void subtract(CalculationResult const& other, Optional<Length::ResolutionContext const&>, PercentageBasis const& percentage_basis);
        void multiply_by(CalculationResult const& other, Optional<Length::ResolutionContext const&>);
        void divide_by(CalculationResult const& other, Optional<Length::ResolutionContext const&>);
        void negate();
        void invert();

        double value() const { return m_value; }
        Optional<CSSNumericType> const& type() const { return m_type; }

        [[nodiscard]] bool operator==(CalculationResult const&) const = default;

    private:
        double m_value;
        Optional<CSSNumericType> m_type;
    };

    static ValueComparingNonnullRefPtr<CalculatedStyleValue> create(NonnullOwnPtr<CalculationNode> calculation, CSSNumericType resolved_type, CalculationContext context)
    {
        return adopt_ref(*new (nothrow) CalculatedStyleValue(move(calculation), move(resolved_type), move(context)));
    }

    virtual String to_string(SerializationMode) const override;
    virtual bool equals(CSSStyleValue const& other) const override;

    bool resolves_to_angle() const { return m_resolved_type.matches_angle(m_context.percentages_resolve_as); }
    bool resolves_to_angle_percentage() const { return m_resolved_type.matches_angle_percentage(m_context.percentages_resolve_as); }
    Optional<Angle> resolve_angle() const;
    Optional<Angle> resolve_angle(Layout::Node const& layout_node) const;
    Optional<Angle> resolve_angle(Length::ResolutionContext const& context) const;
    Optional<Angle> resolve_angle_percentage(Angle const& percentage_basis) const;

    bool resolves_to_flex() const { return m_resolved_type.matches_flex(m_context.percentages_resolve_as); }
    Optional<Flex> resolve_flex() const;

    bool resolves_to_frequency() const { return m_resolved_type.matches_frequency(m_context.percentages_resolve_as); }
    bool resolves_to_frequency_percentage() const { return m_resolved_type.matches_frequency_percentage(m_context.percentages_resolve_as); }
    Optional<Frequency> resolve_frequency() const;
    Optional<Frequency> resolve_frequency_percentage(Frequency const& percentage_basis) const;

    bool resolves_to_length() const { return m_resolved_type.matches_length(m_context.percentages_resolve_as); }
    bool resolves_to_length_percentage() const { return m_resolved_type.matches_length_percentage(m_context.percentages_resolve_as); }
    Optional<Length> resolve_length(Length::ResolutionContext const&) const;
    Optional<Length> resolve_length(Layout::Node const& layout_node) const;
    Optional<Length> resolve_length_percentage(Layout::Node const&, Length const& percentage_basis) const;
    Optional<Length> resolve_length_percentage(Layout::Node const&, CSSPixels percentage_basis) const;
    Optional<Length> resolve_length_percentage(Length::ResolutionContext const&, Length const& percentage_basis) const;

    bool resolves_to_percentage() const { return m_resolved_type.matches_percentage(); }
    Optional<Percentage> resolve_percentage() const;

    bool resolves_to_resolution() const { return m_resolved_type.matches_resolution(m_context.percentages_resolve_as); }
    Optional<Resolution> resolve_resolution() const;

    bool resolves_to_time() const { return m_resolved_type.matches_time(m_context.percentages_resolve_as); }
    bool resolves_to_time_percentage() const { return m_resolved_type.matches_time_percentage(m_context.percentages_resolve_as); }
    Optional<Time> resolve_time() const;
    Optional<Time> resolve_time_percentage(Time const& percentage_basis) const;

    bool resolves_to_number() const { return m_resolved_type.matches_number(m_context.percentages_resolve_as); }
    Optional<double> resolve_number() const;
    Optional<double> resolve_number(Length::ResolutionContext const&) const;
    Optional<double> resolve_number(Layout::Node const& layout_node) const;
    Optional<i64> resolve_integer() const;
    Optional<i64> resolve_integer(Length::ResolutionContext const&) const;
    Optional<i64> resolve_integer(Layout::Node const& layout_node) const;

    bool resolves_to_dimension() const { return m_resolved_type.matches_dimension(); }

    bool contains_percentage() const;

    String dump() const;

private:
    explicit CalculatedStyleValue(NonnullOwnPtr<CalculationNode> calculation, CSSNumericType resolved_type, CalculationContext context)
        : CSSStyleValue(Type::Calculated)
        , m_resolved_type(move(resolved_type))
        , m_calculation(move(calculation))
        , m_context(move(context))
    {
    }

    Optional<ValueType> percentage_resolved_type() const;

    CSSNumericType m_resolved_type;
    NonnullOwnPtr<CalculationNode> m_calculation;
    CalculationContext m_context;
};

// https://www.w3.org/TR/css-values-4/#calculation-tree
class CalculationNode {
public:
    // https://drafts.csswg.org/css-values-4/#calc-constants
    // https://drafts.csswg.org/css-values-4/#calc-error-constants
    enum class ConstantType {
        E,
        Pi,
        NaN,
        Infinity,
        MinusInfinity,
    };
    static Optional<ConstantType> constant_type_from_string(StringView);

    enum class Type {
        Numeric,
        // NOTE: Currently, any value with a `var()` or `attr()` function in it is always an
        //       UnresolvedStyleValue so we do not have to implement a NonMathFunction type here.

        // Comparison function nodes, a sub-type of operator node
        // https://drafts.csswg.org/css-values-4/#comp-func
        Min,
        Max,
        Clamp,

        // Calc-operator nodes, a sub-type of operator node
        // https://www.w3.org/TR/css-values-4/#calculation-tree-calc-operator-nodes
        Sum,
        Product,
        Negate,
        Invert,

        // Sign-Related Functions, a sub-type of operator node
        // https://drafts.csswg.org/css-values-4/#sign-funcs
        Abs,
        Sign,

        // Constant Nodes
        // https://drafts.csswg.org/css-values-4/#calc-constants
        Constant,

        // Trigonometric functions, a sub-type of operator node
        // https://drafts.csswg.org/css-values-4/#trig-funcs
        Sin,
        Cos,
        Tan,
        Asin,
        Acos,
        Atan,
        Atan2,

        // Exponential functions, a sub-type of operator node
        // https://drafts.csswg.org/css-values-4/#exponent-funcs
        Pow,
        Sqrt,
        Hypot,
        Log,
        Exp,

        // Stepped value functions, a sub-type of operator node
        // https://drafts.csswg.org/css-values-4/#round-func
        Round,
        Mod,
        Rem,
    };
    using NumericValue = CalculatedStyleValue::CalculationResult::Value;

    virtual ~CalculationNode();

    Type type() const { return m_type; }

    // https://www.w3.org/TR/css-values-4/#calculation-tree-operator-nodes
    bool is_operator_node() const
    {
        return is_calc_operator_node() || is_math_function_node();
    }

    bool is_math_function_node() const
    {
        switch (m_type) {
        case Type::Min:
        case Type::Max:
        case Type::Clamp:
        case Type::Abs:
        case Type::Sign:
        case Type::Sin:
        case Type::Cos:
        case Type::Tan:
        case Type::Asin:
        case Type::Acos:
        case Type::Atan:
        case Type::Atan2:
        case Type::Pow:
        case Type::Sqrt:
        case Type::Hypot:
        case Type::Log:
        case Type::Exp:
        case Type::Round:
        case Type::Mod:
        case Type::Rem:
            return true;

        default:
            return false;
        }
    }

    // https://www.w3.org/TR/css-values-4/#calculation-tree-calc-operator-nodes
    bool is_calc_operator_node() const
    {
        return first_is_one_of(m_type, Type::Sum, Type::Product, Type::Negate, Type::Invert);
    }

    virtual String to_string() const = 0;
    Optional<CSSNumericType> const& numeric_type() const { return m_numeric_type; }
    virtual bool contains_percentage() const = 0;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const = 0;

    virtual void dump(StringBuilder&, int indent) const = 0;
    virtual bool equals(CalculationNode const&) const = 0;

protected:
    CalculationNode(Type, Optional<CSSNumericType>);

private:
    Type m_type;
    Optional<CSSNumericType> m_numeric_type;
};

class NumericCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<NumericCalculationNode> create(NumericValue, CalculationContext const&);
    ~NumericCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    NumericCalculationNode(NumericValue, CSSNumericType);
    NumericValue m_value;
};

class SumCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<SumCalculationNode> create(Vector<NonnullOwnPtr<CalculationNode>>);
    ~SumCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    SumCalculationNode(Vector<NonnullOwnPtr<CalculationNode>>, Optional<CSSNumericType>);
    Vector<NonnullOwnPtr<CalculationNode>> m_values;
};

class ProductCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<ProductCalculationNode> create(Vector<NonnullOwnPtr<CalculationNode>>);
    ~ProductCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    ProductCalculationNode(Vector<NonnullOwnPtr<CalculationNode>>, Optional<CSSNumericType>);
    Vector<NonnullOwnPtr<CalculationNode>> m_values;
};

class NegateCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<NegateCalculationNode> create(NonnullOwnPtr<CalculationNode>);
    ~NegateCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    NegateCalculationNode(NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_value;
};

class InvertCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<InvertCalculationNode> create(NonnullOwnPtr<CalculationNode>);
    ~InvertCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    InvertCalculationNode(NonnullOwnPtr<CalculationNode>, Optional<CSSNumericType>);
    NonnullOwnPtr<CalculationNode> m_value;
};

class MinCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<MinCalculationNode> create(Vector<NonnullOwnPtr<CalculationNode>>);
    ~MinCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    MinCalculationNode(Vector<NonnullOwnPtr<CalculationNode>>, Optional<CSSNumericType>);
    Vector<NonnullOwnPtr<CalculationNode>> m_values;
};

class MaxCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<MaxCalculationNode> create(Vector<NonnullOwnPtr<CalculationNode>>);
    ~MaxCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    MaxCalculationNode(Vector<NonnullOwnPtr<CalculationNode>>, Optional<CSSNumericType>);
    Vector<NonnullOwnPtr<CalculationNode>> m_values;
};

class ClampCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<ClampCalculationNode> create(NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>);
    ~ClampCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    ClampCalculationNode(NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>, Optional<CSSNumericType>);
    NonnullOwnPtr<CalculationNode> m_min_value;
    NonnullOwnPtr<CalculationNode> m_center_value;
    NonnullOwnPtr<CalculationNode> m_max_value;
};

class AbsCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<AbsCalculationNode> create(NonnullOwnPtr<CalculationNode>);
    ~AbsCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    AbsCalculationNode(NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_value;
};

class SignCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<SignCalculationNode> create(NonnullOwnPtr<CalculationNode>);
    ~SignCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    SignCalculationNode(NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_value;
};

class ConstantCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<ConstantCalculationNode> create(CalculationNode::ConstantType);
    ~ConstantCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override { return false; }
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&> context, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    ConstantCalculationNode(ConstantType);
    CalculationNode::ConstantType m_constant;
};

class SinCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<SinCalculationNode> create(NonnullOwnPtr<CalculationNode>);
    ~SinCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    SinCalculationNode(NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_value;
};

class CosCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<CosCalculationNode> create(NonnullOwnPtr<CalculationNode>);
    ~CosCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    CosCalculationNode(NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_value;
};

class TanCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<TanCalculationNode> create(NonnullOwnPtr<CalculationNode>);
    ~TanCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    TanCalculationNode(NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_value;
};

class AsinCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<AsinCalculationNode> create(NonnullOwnPtr<CalculationNode>);
    ~AsinCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    AsinCalculationNode(NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_value;
};

class AcosCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<AcosCalculationNode> create(NonnullOwnPtr<CalculationNode>);
    ~AcosCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    AcosCalculationNode(NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_value;
};

class AtanCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<AtanCalculationNode> create(NonnullOwnPtr<CalculationNode>);
    ~AtanCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    AtanCalculationNode(NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_value;
};

class Atan2CalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<Atan2CalculationNode> create(NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>);
    ~Atan2CalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    Atan2CalculationNode(NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_y;
    NonnullOwnPtr<CalculationNode> m_x;
};

class PowCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<PowCalculationNode> create(NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>);
    ~PowCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override { return false; }
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    PowCalculationNode(NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_x;
    NonnullOwnPtr<CalculationNode> m_y;
};

class SqrtCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<SqrtCalculationNode> create(NonnullOwnPtr<CalculationNode>);
    ~SqrtCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override { return false; }
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    SqrtCalculationNode(NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_value;
};

class HypotCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<HypotCalculationNode> create(Vector<NonnullOwnPtr<CalculationNode>>);
    ~HypotCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    HypotCalculationNode(Vector<NonnullOwnPtr<CalculationNode>>, Optional<CSSNumericType>);
    Vector<NonnullOwnPtr<CalculationNode>> m_values;
};

class LogCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<LogCalculationNode> create(NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>);
    ~LogCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override { return false; }
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    LogCalculationNode(NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_x;
    NonnullOwnPtr<CalculationNode> m_y;
};

class ExpCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<ExpCalculationNode> create(NonnullOwnPtr<CalculationNode>);
    ~ExpCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override { return false; }
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    ExpCalculationNode(NonnullOwnPtr<CalculationNode>);
    NonnullOwnPtr<CalculationNode> m_value;
};

class RoundCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<RoundCalculationNode> create(RoundingStrategy, NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>);
    ~RoundCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    RoundCalculationNode(RoundingStrategy, NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>, Optional<CSSNumericType>);
    RoundingStrategy m_strategy;
    NonnullOwnPtr<CalculationNode> m_x;
    NonnullOwnPtr<CalculationNode> m_y;
};

class ModCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<ModCalculationNode> create(NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>);
    ~ModCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    ModCalculationNode(NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>, Optional<CSSNumericType>);
    NonnullOwnPtr<CalculationNode> m_x;
    NonnullOwnPtr<CalculationNode> m_y;
};

class RemCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<RemCalculationNode> create(NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>);
    ~RemCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    RemCalculationNode(NonnullOwnPtr<CalculationNode>, NonnullOwnPtr<CalculationNode>, Optional<CSSNumericType>);
    NonnullOwnPtr<CalculationNode> m_x;
    NonnullOwnPtr<CalculationNode> m_y;
};

}
