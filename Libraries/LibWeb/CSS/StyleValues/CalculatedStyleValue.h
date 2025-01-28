/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
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
// Contains the context available at parse-time.
struct CalculationContext {
    Optional<ValueType> percentages_resolve_as {};
    bool resolve_numbers_as_integers = false;
};
// Contains the context for resolving the calculation.
struct CalculationResolutionContext {
    Variant<Empty, Angle, Frequency, Length, Time> percentage_basis {};
    Optional<Length::ResolutionContext> length_resolution_context;
};

class CalculatedStyleValue : public CSSStyleValue {
public:
    class CalculationResult {
    public:
        using Value = Variant<Number, Angle, Flex, Frequency, Length, Percentage, Resolution, Time>;
        static CalculationResult from_value(Value const&, CalculationResolutionContext const&, Optional<CSSNumericType>);

        CalculationResult(double value, Optional<CSSNumericType> type)
            : m_value(value)
            , m_type(move(type))
        {
        }

        void add(CalculationResult const& other);
        void subtract(CalculationResult const& other);
        void multiply_by(CalculationResult const& other);
        void divide_by(CalculationResult const& other);
        void negate();
        void invert();

        double value() const { return m_value; }
        Optional<CSSNumericType> const& type() const { return m_type; }

        [[nodiscard]] bool operator==(CalculationResult const&) const = default;

    private:
        double m_value;
        Optional<CSSNumericType> m_type;
    };

    static ValueComparingNonnullRefPtr<CalculatedStyleValue> create(NonnullRefPtr<CalculationNode> calculation, CSSNumericType resolved_type, CalculationContext context)
    {
        return adopt_ref(*new (nothrow) CalculatedStyleValue(move(calculation), move(resolved_type), move(context)));
    }

    virtual String to_string(SerializationMode) const override;
    virtual bool equals(CSSStyleValue const& other) const override;

    bool resolves_to_angle() const { return m_resolved_type.matches_angle(m_context.percentages_resolve_as); }
    bool resolves_to_angle_percentage() const { return m_resolved_type.matches_angle_percentage(m_context.percentages_resolve_as); }
    Optional<Angle> resolve_angle(CalculationResolutionContext const&) const;

    bool resolves_to_flex() const { return m_resolved_type.matches_flex(m_context.percentages_resolve_as); }
    Optional<Flex> resolve_flex(CalculationResolutionContext const&) const;

    bool resolves_to_frequency() const { return m_resolved_type.matches_frequency(m_context.percentages_resolve_as); }
    bool resolves_to_frequency_percentage() const { return m_resolved_type.matches_frequency_percentage(m_context.percentages_resolve_as); }
    Optional<Frequency> resolve_frequency(CalculationResolutionContext const&) const;

    bool resolves_to_length() const { return m_resolved_type.matches_length(m_context.percentages_resolve_as); }
    bool resolves_to_length_percentage() const { return m_resolved_type.matches_length_percentage(m_context.percentages_resolve_as); }
    Optional<Length> resolve_length(CalculationResolutionContext const&) const;

    bool resolves_to_percentage() const { return m_resolved_type.matches_percentage(); }
    Optional<Percentage> resolve_percentage(CalculationResolutionContext const&) const;

    bool resolves_to_resolution() const { return m_resolved_type.matches_resolution(m_context.percentages_resolve_as); }
    Optional<Resolution> resolve_resolution(CalculationResolutionContext const&) const;

    bool resolves_to_time() const { return m_resolved_type.matches_time(m_context.percentages_resolve_as); }
    bool resolves_to_time_percentage() const { return m_resolved_type.matches_time_percentage(m_context.percentages_resolve_as); }
    Optional<Time> resolve_time(CalculationResolutionContext const&) const;

    bool resolves_to_number() const { return m_resolved_type.matches_number(m_context.percentages_resolve_as); }
    Optional<double> resolve_number(CalculationResolutionContext const&) const;
    Optional<i64> resolve_integer(CalculationResolutionContext const&) const;

    bool resolves_to_dimension() const { return m_resolved_type.matches_dimension(); }

    bool contains_percentage() const;

    String dump() const;

private:
    explicit CalculatedStyleValue(NonnullRefPtr<CalculationNode> calculation, CSSNumericType resolved_type, CalculationContext context)
        : CSSStyleValue(Type::Calculated)
        , m_resolved_type(move(resolved_type))
        , m_calculation(move(calculation))
        , m_context(move(context))
    {
    }

    Optional<ValueType> percentage_resolved_type() const;

    CSSNumericType m_resolved_type;
    NonnullRefPtr<CalculationNode> m_calculation;
    CalculationContext m_context;
};

// https://www.w3.org/TR/css-values-4/#calculation-tree
class CalculationNode : public RefCounted<CalculationNode> {
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
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const = 0;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const = 0;
    // Step 4 of simplify_a_calculation_tree(). Only valid for math-function nodes.
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const { VERIFY_NOT_REACHED(); }

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
    static NonnullRefPtr<NumericCalculationNode> create(NumericValue, CalculationContext const&);
    ~NumericCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    bool is_in_canonical_unit() const;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override { return *this; }

    RefPtr<CSSStyleValue> to_style_value(CalculationContext const&) const;

    NumericValue const& value() const { return m_value; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    NumericCalculationNode(NumericValue, CSSNumericType);
    NumericValue m_value;
};

class SumCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<SumCalculationNode> create(Vector<NonnullRefPtr<CalculationNode>>);
    ~SumCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;

    Vector<NonnullRefPtr<CalculationNode>> const& children() const { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    SumCalculationNode(Vector<NonnullRefPtr<CalculationNode>>, Optional<CSSNumericType>);
    Vector<NonnullRefPtr<CalculationNode>> m_values;
};

class ProductCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ProductCalculationNode> create(Vector<NonnullRefPtr<CalculationNode>>);
    ~ProductCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;

    Vector<NonnullRefPtr<CalculationNode>> const& children() const { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    ProductCalculationNode(Vector<NonnullRefPtr<CalculationNode>>, Optional<CSSNumericType>);
    Vector<NonnullRefPtr<CalculationNode>> m_values;
};

class NegateCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<NegateCalculationNode> create(NonnullRefPtr<CalculationNode>);
    ~NegateCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;

    CalculationNode const& child() const { return m_value; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    NegateCalculationNode(NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_value;
};

class InvertCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<InvertCalculationNode> create(NonnullRefPtr<CalculationNode>);
    ~InvertCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;

    CalculationNode const& child() const { return m_value; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    InvertCalculationNode(NonnullRefPtr<CalculationNode>, Optional<CSSNumericType>);
    NonnullRefPtr<CalculationNode> m_value;
};

class MinCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<MinCalculationNode> create(Vector<NonnullRefPtr<CalculationNode>>);
    ~MinCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    Vector<NonnullRefPtr<CalculationNode>> const& children() const { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    MinCalculationNode(Vector<NonnullRefPtr<CalculationNode>>, Optional<CSSNumericType>);
    Vector<NonnullRefPtr<CalculationNode>> m_values;
};

class MaxCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<MaxCalculationNode> create(Vector<NonnullRefPtr<CalculationNode>>);
    ~MaxCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    Vector<NonnullRefPtr<CalculationNode>> const& children() const { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    MaxCalculationNode(Vector<NonnullRefPtr<CalculationNode>>, Optional<CSSNumericType>);
    Vector<NonnullRefPtr<CalculationNode>> m_values;
};

class ClampCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ClampCalculationNode> create(NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>);
    ~ClampCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    ClampCalculationNode(NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>, Optional<CSSNumericType>);
    NonnullRefPtr<CalculationNode> m_min_value;
    NonnullRefPtr<CalculationNode> m_center_value;
    NonnullRefPtr<CalculationNode> m_max_value;
};

class AbsCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<AbsCalculationNode> create(NonnullRefPtr<CalculationNode>);
    ~AbsCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    AbsCalculationNode(NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_value;
};

class SignCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<SignCalculationNode> create(NonnullRefPtr<CalculationNode>);
    ~SignCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    SignCalculationNode(NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_value;
};

class ConstantCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ConstantCalculationNode> create(CalculationNode::ConstantType);
    ~ConstantCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override { return false; }
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override { return *this; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    ConstantCalculationNode(ConstantType);
    CalculationNode::ConstantType m_constant;
};

class SinCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<SinCalculationNode> create(NonnullRefPtr<CalculationNode>);
    ~SinCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    SinCalculationNode(NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_value;
};

class CosCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<CosCalculationNode> create(NonnullRefPtr<CalculationNode>);
    ~CosCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    CosCalculationNode(NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_value;
};

class TanCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<TanCalculationNode> create(NonnullRefPtr<CalculationNode>);
    ~TanCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    TanCalculationNode(NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_value;
};

class AsinCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<AsinCalculationNode> create(NonnullRefPtr<CalculationNode>);
    ~AsinCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    AsinCalculationNode(NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_value;
};

class AcosCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<AcosCalculationNode> create(NonnullRefPtr<CalculationNode>);
    ~AcosCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    AcosCalculationNode(NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_value;
};

class AtanCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<AtanCalculationNode> create(NonnullRefPtr<CalculationNode>);
    ~AtanCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    AtanCalculationNode(NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_value;
};

class Atan2CalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<Atan2CalculationNode> create(NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>);
    ~Atan2CalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    Atan2CalculationNode(NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_y;
    NonnullRefPtr<CalculationNode> m_x;
};

class PowCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<PowCalculationNode> create(NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>);
    ~PowCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override { return false; }
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    PowCalculationNode(NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_x;
    NonnullRefPtr<CalculationNode> m_y;
};

class SqrtCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<SqrtCalculationNode> create(NonnullRefPtr<CalculationNode>);
    ~SqrtCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override { return false; }
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    SqrtCalculationNode(NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_value;
};

class HypotCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<HypotCalculationNode> create(Vector<NonnullRefPtr<CalculationNode>>);
    ~HypotCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    Vector<NonnullRefPtr<CalculationNode>> const& children() const { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    HypotCalculationNode(Vector<NonnullRefPtr<CalculationNode>>, Optional<CSSNumericType>);
    Vector<NonnullRefPtr<CalculationNode>> m_values;
};

class LogCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<LogCalculationNode> create(NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>);
    ~LogCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override { return false; }
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    LogCalculationNode(NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_x;
    NonnullRefPtr<CalculationNode> m_y;
};

class ExpCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ExpCalculationNode> create(NonnullRefPtr<CalculationNode>);
    ~ExpCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override { return false; }
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    ExpCalculationNode(NonnullRefPtr<CalculationNode>);
    NonnullRefPtr<CalculationNode> m_value;
};

class RoundCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<RoundCalculationNode> create(RoundingStrategy, NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>);
    ~RoundCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    RoundCalculationNode(RoundingStrategy, NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>, Optional<CSSNumericType>);
    RoundingStrategy m_strategy;
    NonnullRefPtr<CalculationNode> m_x;
    NonnullRefPtr<CalculationNode> m_y;
};

class ModCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ModCalculationNode> create(NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>);
    ~ModCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    ModCalculationNode(NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>, Optional<CSSNumericType>);
    NonnullRefPtr<CalculationNode> m_x;
    NonnullRefPtr<CalculationNode> m_y;
};

class RemCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<RemCalculationNode> create(NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>);
    ~RemCalculationNode();

    virtual String to_string() const override;
    virtual bool contains_percentage() const override;
    virtual CalculatedStyleValue::CalculationResult resolve(CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CalculationNode> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    RemCalculationNode(NonnullRefPtr<CalculationNode>, NonnullRefPtr<CalculationNode>, Optional<CSSNumericType>);
    NonnullRefPtr<CalculationNode> m_x;
    NonnullRefPtr<CalculationNode> m_y;
};

// https://drafts.csswg.org/css-values-4/#calc-simplification
NonnullRefPtr<CalculationNode> simplify_a_calculation_tree(CalculationNode const& root, CalculationContext const& context, CalculationResolutionContext const& resolution_context);

}
