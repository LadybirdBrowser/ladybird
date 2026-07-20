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
#include <LibWeb/CSS/NumericRange.h>
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/StyleValues/AbstractNonMathCalcFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

class CalcNodeRef;

// https://drafts.csswg.org/css-values-4/#calc-context
// Contains the context available at parse-time.
struct CalculationContext {
    Optional<ValueType> percentages_resolve_as {};
    bool resolve_numbers_as_integers = false;
    // FIXME: Once calc() parsing knows the target numeric type, pass a single NumericRange instead of the full accepted range set.
    NumericRangesByValueType accepted_ranges_by_type {};

    static CalculationContext for_property(PropertyNameAndID const&);
};

class CalculatedStyleValue : public StyleValue {
public:
    // The numeric value alternatives a calculation leaf can hold.
    using NumericValue = Variant<Number, Angle, Flex, Frequency, Length, Percentage, Resolution, Time>;

    static ValueComparingNonnullRefPtr<CalculatedStyleValue const> create(NonnullRefPtr<CalculationNode const> calculation, NumericType resolved_type, CalculationContext context)
    {
        return adopt_ref(*new (nothrow) CalculatedStyleValue(move(calculation), move(resolved_type), move(context)));
    }
    static ValueComparingNonnullRefPtr<CalculatedStyleValue const> create(CalcNodeRef root, NumericType resolved_type, CalculationContext context);

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const& other) const;
    StyleValueFFI::CalcNode const* rust_calculation_root() const { return m_value->calculated.rust_calculation.node; }
    CalculationContext calculation_context() const;

    bool resolves_to_angle() const { return resolved_type().matches_angle(calculation_context().percentages_resolve_as); }
    bool resolves_to_angle_percentage() const { return resolved_type().matches_angle_percentage(calculation_context().percentages_resolve_as); }
    Optional<Angle> resolve_angle(CalculationResolutionContext const&) const;

    bool resolves_to_flex() const { return resolved_type().matches_flex(calculation_context().percentages_resolve_as); }
    Optional<Flex> resolve_flex(CalculationResolutionContext const&) const;

    bool resolves_to_frequency() const { return resolved_type().matches_frequency(calculation_context().percentages_resolve_as); }
    bool resolves_to_frequency_percentage() const { return resolved_type().matches_frequency_percentage(calculation_context().percentages_resolve_as); }
    Optional<Frequency> resolve_frequency(CalculationResolutionContext const&) const;

    bool resolves_to_length() const { return resolved_type().matches_length(calculation_context().percentages_resolve_as); }
    bool resolves_to_length_percentage() const { return resolved_type().matches_length_percentage(calculation_context().percentages_resolve_as); }
    Optional<Length> resolve_length(CalculationResolutionContext const&) const;
    Optional<double> resolve_raw_length(CalculationResolutionContext const&) const;

    bool resolves_to_percentage() const { return resolved_type().matches_percentage(); }
    Optional<Percentage> resolve_percentage(CalculationResolutionContext const&) const;

    bool resolves_to_resolution() const { return resolved_type().matches_resolution(calculation_context().percentages_resolve_as); }
    Optional<Resolution> resolve_resolution(CalculationResolutionContext const&) const;

    bool resolves_to_time() const { return resolved_type().matches_time(calculation_context().percentages_resolve_as); }
    bool resolves_to_time_percentage() const { return resolved_type().matches_time_percentage(calculation_context().percentages_resolve_as); }
    Optional<Time> resolve_time(CalculationResolutionContext const&) const;

    bool resolves_to_number() const { return resolved_type().matches_number(calculation_context().percentages_resolve_as); }
    Optional<double> resolve_number(CalculationResolutionContext const&) const;
    Optional<i32> resolve_integer(CalculationResolutionContext const&) const;

    RefPtr<StyleValue const> resolve_as_style_value(CalculationResolutionContext const&) const;

    bool resolves_to_dimension() const { return resolved_type().matches_dimension(); }

    bool contains_percentage() const;
    bool is_fully_simplified() const;

    String dump() const;

    GC::Ref<CSSStyleValue> reify(JS::Realm&, Utf16FlyString const& associated_property) const;

    // Whether any node of the calculation is an anchor() function, which layout must
    // resolve with the anchor context of the box being positioned.
    bool contains_anchor_function() const;

private:
    explicit CalculatedStyleValue(NonnullRefPtr<CalculationNode const> calculation, NumericType resolved_type, CalculationContext context)
        : StyleValue(Type::Calculated, make_calculated_data(calculation, resolved_type, context))
    {
    }

    // Takes ownership of a transferred Rust calculation root.
    explicit CalculatedStyleValue(StyleValueFFI::CalcNode const* rust_root, NumericType resolved_type, CalculationContext context)
        : StyleValue(Type::Calculated, make_calculated_data_from_rust_root(rust_root, resolved_type, context))
    {
    }

    struct ResolvedValue {
        double value;
        Optional<NumericType> type;
    };
    // FIXME: Calculations should be simplified apart from percentages by the absolutized method prior to this method
    //        being called so we can take just the percentage_basis rather than a full CalculationResolutionContext.
    //        There are still some CalculatedStyleValues which we don't call absolutized for (i.e. sub-values of other
    //        StyleValue classes which lack their own absolutized method) which will need to be fixed beforehand.
    Optional<ResolvedValue> resolve_value(CalculationResolutionContext const&, bool apply_censoring_and_clamping = true) const;
    Optional<ResolvedValue> resolve_value(CalculationContext const&, CalculationResolutionContext const&, bool apply_censoring_and_clamping = true) const;

    Optional<ValueType> percentage_resolved_type() const;

    static StyleValueFFI::StyleValueData* make_calculated_data(NonnullRefPtr<CalculationNode const> const&, NumericType const&, CalculationContext const&);
    static StyleValueFFI::StyleValueData* make_calculated_data_from_rust_root(StyleValueFFI::CalcNode const*, NumericType const&, CalculationContext const&);

    NumericType resolved_type() const;
};

#define ENUMERATE_CALCULATION_NODE_TYPES(X) \
    X(Numeric)                              \
    X(ChannelKeyword)                       \
    X(Min)                                  \
    X(Max)                                  \
    X(Clamp)                                \
    X(Sum)                                  \
    X(Product)                              \
    X(Progress)                             \
    X(Negate)                               \
    X(Invert)                               \
    X(Abs)                                  \
    X(Sign)                                 \
    X(Sin)                                  \
    X(Cos)                                  \
    X(Tan)                                  \
    X(Asin)                                 \
    X(Acos)                                 \
    X(Atan)                                 \
    X(Atan2)                                \
    X(Pow)                                  \
    X(Sqrt)                                 \
    X(Hypot)                                \
    X(Log)                                  \
    X(Exp)                                  \
    X(Round)                                \
    X(Mod)                                  \
    X(Rem)                                  \
    X(Random)                               \
    X(NonMathFunction)

// https://www.w3.org/TR/css-values-4/#calculation-tree
class CalculationNode : public RefCounted<CalculationNode> {
public:
    // NOTE: Currently, any value with a `var()` or `attr()` function in it is always an
    //       UnresolvedStyleValue so we do not have to implement them as CalculationNodes.

    enum class Type {
#define ENUMERATE_TYPE(name) name,
        ENUMERATE_CALCULATION_NODE_TYPES(ENUMERATE_TYPE)
#undef ENUMERATE_TYPE
    };

    template<typename T>
    bool fast_is() const = delete;

    using NumericValue = CalculatedStyleValue::NumericValue;

    virtual ~CalculationNode();

    static NonnullRefPtr<CalculationNode const> from_style_value(NonnullRefPtr<StyleValue const> const&, CalculationContext const&);

    Type type() const { return m_type; }

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const = 0;

    Optional<NumericType> const& numeric_type() const { return m_numeric_type; }

    virtual void dump(StringBuilder&, int indent) const = 0;

protected:
    CalculationNode(Type, Optional<NumericType>);

private:
    Type m_type;
    Optional<NumericType> m_numeric_type;
};

class NumericCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<NumericCalculationNode const> create(NumericValue, CalculationContext const&);
    static RefPtr<NumericCalculationNode const> from_keyword(Keyword, CalculationContext const&);
    ~NumericCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return {}; }
    NumericValue const& value() const { return m_value; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    NumericCalculationNode(NumericValue, NumericType);
    NumericValue m_value;
};

// https://drafts.csswg.org/css-color-5/#relative-color
class ChannelKeywordCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ChannelKeywordCalculationNode const> create(ChannelKeyword, CalculationContext const&);
    ~ChannelKeywordCalculationNode();

    ChannelKeyword channel() const { return m_channel; }

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return {}; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    ChannelKeywordCalculationNode(ChannelKeyword);
    ChannelKeyword m_channel;
};

class SumCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<SumCalculationNode const> create(Vector<NonnullRefPtr<CalculationNode const>>);
    ~SumCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    SumCalculationNode(Vector<NonnullRefPtr<CalculationNode const>>, Optional<NumericType>);
    Vector<NonnullRefPtr<CalculationNode const>> m_values;
};

class ProductCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ProductCalculationNode const> create(Vector<NonnullRefPtr<CalculationNode const>>);
    ~ProductCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    ProductCalculationNode(Vector<NonnullRefPtr<CalculationNode const>>, Optional<NumericType>);
    Vector<NonnullRefPtr<CalculationNode const>> m_values;
};

class ProgressCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ProgressCalculationNode const> create(bool no_clamp, NonnullRefPtr<CalculationNode const> value, NonnullRefPtr<CalculationNode const> start_value, NonnullRefPtr<CalculationNode const> end_value);
    ~ProgressCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value, m_start_value, m_end_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;
    bool no_clamp() const { return m_no_clamp; }

private:
    ProgressCalculationNode(bool no_clamp, NonnullRefPtr<CalculationNode const> value, NonnullRefPtr<CalculationNode const> start_value, NonnullRefPtr<CalculationNode const> end_value, Optional<NumericType> numeric_type);

    bool m_no_clamp;
    NonnullRefPtr<CalculationNode const> m_value;
    NonnullRefPtr<CalculationNode const> m_start_value;
    NonnullRefPtr<CalculationNode const> m_end_value;
};

class NegateCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<NegateCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~NegateCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }
    CalculationNode const& child() const { return m_value; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    explicit NegateCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class InvertCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<InvertCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~InvertCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }
    CalculationNode const& child() const { return m_value; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    InvertCalculationNode(NonnullRefPtr<CalculationNode const>, Optional<NumericType>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class MinCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<MinCalculationNode const> create(Vector<NonnullRefPtr<CalculationNode const>>);
    ~MinCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    MinCalculationNode(Vector<NonnullRefPtr<CalculationNode const>>, Optional<NumericType>);
    Vector<NonnullRefPtr<CalculationNode const>> m_values;
};

class MaxCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<MaxCalculationNode const> create(Vector<NonnullRefPtr<CalculationNode const>>);
    ~MaxCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    MaxCalculationNode(Vector<NonnullRefPtr<CalculationNode const>>, Optional<NumericType>);
    Vector<NonnullRefPtr<CalculationNode const>> m_values;
};

class ClampCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ClampCalculationNode const> create(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~ClampCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_min_value, m_center_value, m_max_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;

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

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    explicit AbsCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class SignCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<SignCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~SignCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    explicit SignCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class SinCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<SinCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~SinCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    explicit SinCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class CosCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<CosCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~CosCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    explicit CosCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class TanCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<TanCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~TanCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    explicit TanCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class AsinCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<AsinCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~AsinCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    explicit AsinCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class AcosCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<AcosCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~AcosCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    explicit AcosCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class AtanCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<AtanCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~AtanCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    explicit AtanCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class Atan2CalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<Atan2CalculationNode const> create(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~Atan2CalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_y, m_x } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    Atan2CalculationNode(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_y;
    NonnullRefPtr<CalculationNode const> m_x;
};

class PowCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<PowCalculationNode const> create(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~PowCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_x, m_y } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    PowCalculationNode(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_x;
    NonnullRefPtr<CalculationNode const> m_y;
};

class SqrtCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<SqrtCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~SqrtCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    explicit SqrtCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class HypotCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<HypotCalculationNode const> create(Vector<NonnullRefPtr<CalculationNode const>>);
    ~HypotCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return m_values; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    HypotCalculationNode(Vector<NonnullRefPtr<CalculationNode const>>, Optional<NumericType>);
    Vector<NonnullRefPtr<CalculationNode const>> m_values;
};

class LogCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<LogCalculationNode const> create(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~LogCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_x, m_y } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    LogCalculationNode(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_x;
    NonnullRefPtr<CalculationNode const> m_y;
};

class ExpCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<ExpCalculationNode const> create(NonnullRefPtr<CalculationNode const>);
    ~ExpCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_value } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    explicit ExpCalculationNode(NonnullRefPtr<CalculationNode const>);
    NonnullRefPtr<CalculationNode const> m_value;
};

class RoundCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<RoundCalculationNode const> create(RoundingStrategy, NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~RoundCalculationNode();

    // NOTE: This excludes the rounding strategy!
    RoundingStrategy rounding_strategy() const { return m_strategy; }
    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_x, m_y } }; }

    virtual void dump(StringBuilder&, int indent) const override;

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

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_x, m_y } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    ModCalculationNode(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>, Optional<NumericType>);
    NonnullRefPtr<CalculationNode const> m_x;
    NonnullRefPtr<CalculationNode const> m_y;
};

class RandomCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<RandomCalculationNode const> create(NonnullRefPtr<RandomValueSharingStyleValue const>, NonnullRefPtr<CalculationNode const> minimum, NonnullRefPtr<CalculationNode const> maximum, RefPtr<CalculationNode const> step);
    ~RandomCalculationNode();

    // NOTE: We don't return children here as serialization is handled ad-hoc
    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return {}; }

    virtual void dump(StringBuilder&, int indent) const override;
    RandomValueSharingStyleValue const& random_value_sharing() const { return m_random_value_sharing; }
    CalculationNode const& minimum() const { return m_minimum; }
    CalculationNode const& maximum() const { return m_maximum; }
    RefPtr<CalculationNode const> step() const { return m_step; }

private:
    RandomCalculationNode(NonnullRefPtr<RandomValueSharingStyleValue const>, NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>, RefPtr<CalculationNode const>, Optional<NumericType>);
    ValueComparingNonnullRefPtr<RandomValueSharingStyleValue const> m_random_value_sharing;
    ValueComparingNonnullRefPtr<CalculationNode const> m_minimum;
    ValueComparingNonnullRefPtr<CalculationNode const> m_maximum;
    ValueComparingRefPtr<CalculationNode const> m_step;
};

class RemCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<RemCalculationNode const> create(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>);
    ~RemCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return { { m_x, m_y } }; }

    virtual void dump(StringBuilder&, int indent) const override;

private:
    RemCalculationNode(NonnullRefPtr<CalculationNode const>, NonnullRefPtr<CalculationNode const>, Optional<NumericType>);
    NonnullRefPtr<CalculationNode const> m_x;
    NonnullRefPtr<CalculationNode const> m_y;
};

class NonMathFunctionCalculationNode final : public CalculationNode {
public:
    static NonnullRefPtr<NonMathFunctionCalculationNode const> create(AbstractNonMathCalcFunctionStyleValue const&, NumericType);
    ~NonMathFunctionCalculationNode();

    virtual Vector<NonnullRefPtr<CalculationNode const>> children() const override { return {}; }

    virtual void dump(StringBuilder&, int indent) const override;
    ValueComparingNonnullRefPtr<AbstractNonMathCalcFunctionStyleValue const> function() const { return m_function; }

private:
    NonMathFunctionCalculationNode(AbstractNonMathCalcFunctionStyleValue const& function, NumericType);
    ValueComparingNonnullRefPtr<AbstractNonMathCalcFunctionStyleValue const> m_function;
};

#define ENUMERATE_TYPE(name) \
    template<>               \
    inline bool CalculationNode::fast_is<name##CalculationNode>() const { return type() == Type::name; }
ENUMERATE_CALCULATION_NODE_TYPES(ENUMERATE_TYPE)
#undef ENUMERATE_TYPE

// https://drafts.csswg.org/css-values-4/#calc-simplification
NonnullRefPtr<CalculationNode const> simplify_a_calculation_tree(CalculationNode const& root, CalculationContext const& context, CalculationResolutionContext const& resolution_context);
CalcNodeRef simplify_a_calculation_tree(CalcNodeRef const& root, CalculationContext const& context, CalculationResolutionContext const& resolution_context);

}
