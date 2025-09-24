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
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Flex.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

class CalculationNode;

// https://drafts.csswg.org/css-values-4/#calc-context
// Contains the context available at parse-time.
struct CalculationContext {
    Optional<ValueType> percentages_resolve_as {};
    bool resolve_numbers_as_integers = false;
    AcceptedTypeRangeMap accepted_type_ranges {};
};

class CalculatedStyleValue : public StyleValue {
public:
    class CalculationResult {
    public:
        using Value = Variant<Number, Angle, Flex, Frequency, Length, Percentage, Resolution, Time>;
        static CalculationResult from_value(Value const&, CalculationResolutionContext const&, Optional<NumericType>);

        CalculationResult(double value, Optional<NumericType> type)
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
        Optional<NumericType> const& type() const { return m_type; }

        [[nodiscard]] bool operator==(CalculationResult const&) const = default;

    private:
        double m_value;
        Optional<NumericType> m_type;
    };

    static ValueComparingNonnullRefPtr<CalculatedStyleValue const> create(NonnullRefPtr<CalculationNode const> calculation, NumericType resolved_type, CalculationContext context)
    {
        return adopt_ref(*new (nothrow) CalculatedStyleValue(move(calculation), move(resolved_type), move(context)));
    }

    virtual String to_string(SerializationMode) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const override;
    virtual bool equals(StyleValue const& other) const override;

    NonnullRefPtr<CalculationNode const> calculation() const { return m_calculation; }

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

    virtual GC::Ref<CSSStyleValue> reify(JS::Realm&, String const& associated_property) const override;

private:
    explicit CalculatedStyleValue(NonnullRefPtr<CalculationNode const> calculation, NumericType resolved_type, CalculationContext context)
        : StyleValue(Type::Calculated)
        , m_resolved_type(move(resolved_type))
        , m_calculation(move(calculation))
        , m_context(move(context))
    {
    }

    struct ResolvedValue {
        double value;
        Optional<NumericType> type;
    };
    Optional<ResolvedValue> resolve_value(CalculationResolutionContext const&) const;

    Optional<ValueType> percentage_resolved_type() const;

    NumericType m_resolved_type;
    NonnullRefPtr<CalculationNode const> m_calculation;
    CalculationContext m_context;
};

// https://www.w3.org/TR/css-values-4/#calculation-tree
class CalculationNode : public RefCounted<CalculationNode> {
public:
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

    StringView name() const;
    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const = 0;

    Optional<NumericType> const& numeric_type() const { return m_numeric_type; }
    virtual bool contains_percentage() const = 0;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const = 0;
    // Step 4 of simpliRfy_a_calculation_tree(). Only valid for math-function nodes.
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const { VERIFY_NOT_REACHED(); }

    virtual void dump(StringBuilder&, int indent) const = 0;
    virtual bool equals(CalculationNode const&) const = 0;
    virtual GC::Ptr<CSSNumericValue> reify(JS::Realm&) const { return nullptr; }

protected:
    CalculationNode(Type, Optional<NumericType>);

private:
    Type m_type;
    Optional<NumericType> m_numeric_type;
};

enum class NonFiniteValue {
    Infinity,
    NegativeInfinity,
    NaN,
};

class NumericCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<NumericCalculationNode const> create(NumericValue, CalculationContext const&);
    static RefPtr<NumericCalculationNode const> from_keyword(Keyword, CalculationContext const&);
    ~NumericCalculationNode();

    virtual bool contains_percentage() const override;
    bool is_in_canonical_unit() const;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override { return *this; }

    RefPtr<StyleValue const> to_style_value(CalculationContext const&) const;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return {}; }
    NumericValue const& value() const { return m_value; }
    String value_to_string() const;

    Optional<NonFiniteValue> infinite_or_nan_value() const;
    bool is_negative() const;
    NonnullRefPtr<NumericCalculationNode const> negated(CalculationContext const&) const;

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;
    virtual GC::Ptr<CSSNumericValue> reify(JS::Realm&) const override;

private:
    NumericCalculationNode(NumericValue, NumericType);
    NumericValue m_value;
};

class SumCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<SumCalculationNode const> create(Vector<NonnullRefPtr<CalculationNode const>>);
    ~SumCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;
    virtual GC::Ptr<CSSNumericValue> reify(JS::Realm&) const override;

private:
    SumCalculationNode(Vector<NonnullRefPtr<CalculationNode const>>, Optional<NumericType>);
    Vector<NonnullRefPtr<CalculationNode const>> m_values;
};

class ProductCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ProductCalculationNode const> create(Vector<NonnullRefPtr<CalculationNode const>>);
    ~ProductCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;
    virtual GC::Ptr<CSSNumericValue> reify(JS::Realm&) const override;

private:
    ProductCalculationNode(Vector<NonnullRefPtr<CalculationNode const>>, Optional<NumericType>);
    Vector<NonnullRefPtr<CalculationNode const>> m_values;
};

class NegateCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<NegateCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~NegateCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }
    CalculationNode const& child() const { return m_value; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;
    virtual GC::Ptr<CSSNumericValue> reify(JS::Realm&) const override;

private:
    explicit NegateCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class InvertCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<InvertCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~InvertCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }
    CalculationNode const& child() const { return m_value; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;
    virtual GC::Ptr<CSSNumericValue> reify(JS::Realm&) const override;

private:
    InvertCalculationNode(NonnullRefPtr<CalculationNode const>, Optional<NumericType>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class MinCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<MinCalculationNode const> create(Vector<NonnullRefPtr<CalculationNode const>>);
    ~MinCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;
    virtual GC::Ptr<CSSNumericValue> reify(JS::Realm&) const override;

private:
    MinCalculationNode(Vector<NonnullRefPtr<CalculationNode const>>, Optional<NumericType>);
    Vector<NonnullRefPtr<CalculationNode const>> m_values;
};

class MaxCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<MaxCalculationNode const> create(Vector<NonnullRefPtr<CalculationNode const>>);
    ~MaxCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;
    virtual GC::Ptr<CSSNumericValue> reify(JS::Realm&) const override;

private:
    MaxCalculationNode(Vector<NonnullRefPtr<CalculationNode const>>, Optional<NumericType>);
    Vector<NonnullRefPtr<CalculationNode const>> m_values;
};

class ClampCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ClampCalculationNode const> create(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~ClampCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_min_value, m_center_value, m_max_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;
    virtual GC::Ptr<CSSNumericValue> reify(JS::Realm&) const override;

private:
    ClampCalculationNode(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>, Optional<NumericType>);
    NonnullRefPtr<CalculationNode const> m_min_value;
    NonnullRefPtr<CalculationNode const> m_center_value;
    NonnullRefPtr<CalculationNode const> m_max_value;
};

class AbsCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<AbsCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~AbsCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    explicit AbsCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class SignCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<SignCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~SignCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    explicit SignCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class SinCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<SinCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~SinCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    explicit SinCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class CosCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<CosCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~CosCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    explicit CosCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class TanCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<TanCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~TanCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    explicit TanCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class AsinCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<AsinCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~AsinCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    explicit AsinCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class AcosCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<AcosCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~AcosCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    explicit AcosCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class AtanCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<AtanCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~AtanCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    explicit AtanCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class Atan2CalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<Atan2CalculationNode const> create(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~Atan2CalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_y, m_x } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    Atan2CalculationNode(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_y;
    NonnullRefPtr<CalculationNode const> m_x;
};

class PowCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<PowCalculationNode const> create(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~PowCalculationNode();

    virtual bool contains_percentage() const override { return false; }
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_x, m_y } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    PowCalculationNode(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_x;
    NonnullRefPtr<CalculationNode const> m_y;
};

class SqrtCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<SqrtCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~SqrtCalculationNode();

    virtual bool contains_percentage() const override { return false; }
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    explicit SqrtCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class HypotCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<HypotCalculationNode const> create(Vector<NonnullRefPtr<CalculationNode const>>);
    ~HypotCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    HypotCalculationNode(Vector<NonnullRefPtr<CalculationNode const>>, Optional<NumericType>);
    Vector<NonnullRefPtr<CalculationNode const>> m_values;
};

class LogCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<LogCalculationNode const> create(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~LogCalculationNode();

    virtual bool contains_percentage() const override { return false; }
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_x, m_y } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    LogCalculationNode(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_x;
    NonnullRefPtr<CalculationNode const> m_y;
};

class ExpCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ExpCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~ExpCalculationNode();

    virtual bool contains_percentage() const override { return false; }
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    explicit ExpCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class RoundCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<RoundCalculationNode const> create(RoundingStrategy, NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~RoundCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    // NOTE: This excludes the rounding strategy!
    RoundingStrategy rounding_strategy() const { return m_strategy; }
    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_x, m_y } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    RoundCalculationNode(RoundingStrategy, NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>, Optional<NumericType>);
    RoundingStrategy m_strategy;
    NonnullRefPtr<CalculationNode const> m_x;
    NonnullRefPtr<CalculationNode const> m_y;
};

class ModCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ModCalculationNode const> create(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~ModCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_x, m_y } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    ModCalculationNode(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>, Optional<NumericType>);
    NonnullRefPtr<CalculationNode const> m_x;
    NonnullRefPtr<CalculationNode const> m_y;
};

class RemCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<RemCalculationNode const> create(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~RemCalculationNode();

    virtual bool contains_percentage() const override;
    virtual NonnullRefPtr<CalculationNode const> with_simplified_children(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual Optional<CalculatedStyleValue::CalculationResult> run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_x, m_y } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    virtual bool equals(CalculationNode const&) const override;

private:
    RemCalculationNode(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>, Optional<NumericType>);
    NonnullRefPtr<CalculationNode const> m_x;
    NonnullRefPtr<CalculationNode const> m_y;
};

// https://drafts.csswg.org/css-values-4/#calc-simplification
NonnullRefPtr<CalculationNode const> simplify_a_calculation_tree(CalculationNode const& root, CalculationContext const& context, CalculationResolutionContext const& resolution_context);

}
