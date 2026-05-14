/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/StringBuilder.h>
#include <AK/Variant.h>
#include <LibWeb/CSS/BooleanExpression.h>
#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/mediaqueries-5/#typedef-mf-value
class FeatureValue {
public:
    enum class Type : u8 {
        Ident,
        Integer,
        Length,
        Ratio,
        Resolution,
        Unknown,
    };

    explicit FeatureValue(Type type, NonnullRefPtr<StyleValue const> value)
        : m_type(type)
        , m_value(move(value))
    {
    }

    String to_string(SerializationMode mode) const;

    bool is_ident() const { return m_type == Type::Ident; }
    bool is_length() const { return m_type == Type::Length; }
    bool is_integer() const { return m_type == Type::Integer; }
    bool is_ratio() const { return m_type == Type::Ratio; }
    bool is_resolution() const { return m_type == Type::Resolution; }
    bool is_unknown() const { return m_type == Type::Unknown; }
    bool is_same_type(FeatureValue const& other) const { return m_type == other.m_type; }

    Keyword ident() const
    {
        VERIFY(is_ident());
        return m_value->to_keyword();
    }

    Length length(ComputationContext const& computation_context) const
    {
        VERIFY(is_length());
        return Length::from_style_value(m_value->absolutized(computation_context), {});
    }

    Ratio ratio(ComputationContext const& computation_context) const
    {
        VERIFY(is_ratio());
        return m_value->absolutized(computation_context)->as_ratio().resolved();
    }

    Resolution resolution(ComputationContext const& computation_context) const
    {
        VERIFY(is_resolution());
        return Resolution::from_style_value(m_value->absolutized(computation_context));
    }

    i32 integer(ComputationContext const& computation_context) const
    {
        VERIFY(is_integer());
        return int_from_style_value(m_value->absolutized(computation_context));
    }

private:
    Type m_type;
    NonnullRefPtr<StyleValue const> m_value;
};

enum class FeatureComparison : u8 {
    Equal,
    LessThan,
    LessThanOrEqual,
    GreaterThan,
    GreaterThanOrEqual,
};

StringView string_from_feature_comparison(FeatureComparison);
bool feature_comparisons_match(FeatureComparison, FeatureComparison);

MatchResult compare_feature_values(FeatureValue const& left, FeatureComparison comparison, FeatureValue const& right, ComputationContext const&);

template<typename Derived, typename FeatureID>
class FeatureQuery : public BooleanExpression {
public:
    enum class Type : u8 {
        IsTrue,
        ExactValue,
        MinValue,
        MaxValue,
        Range,
    };

    struct Range {
        Optional<FeatureValue> left_value {};
        Optional<FeatureComparison> left_comparison {};
        Optional<FeatureComparison> right_comparison {};
        Optional<FeatureValue> right_value {};
    };

    static NonnullOwnPtr<Derived> boolean(FeatureID id)
    {
        return adopt_own(*new Derived(Type::IsTrue, id));
    }

    static NonnullOwnPtr<Derived> plain(FeatureID id, FeatureValue&& value)
    {
        return adopt_own(*new Derived(Type::ExactValue, id, move(value)));
    }

    static NonnullOwnPtr<Derived> min(FeatureID id, FeatureValue&& value)
    {
        return adopt_own(*new Derived(Type::MinValue, id, move(value)));
    }

    static NonnullOwnPtr<Derived> max(FeatureID id, FeatureValue&& value)
    {
        return adopt_own(*new Derived(Type::MaxValue, id, move(value)));
    }

    static NonnullOwnPtr<Derived> half_range(FeatureValue&& value, FeatureComparison comparison, FeatureID id)
    {
        return adopt_own(*new Derived(Type::Range, id,
            Range {
                .left_value = move(value),
                .left_comparison = comparison,
            }));
    }

    static NonnullOwnPtr<Derived> half_range(FeatureID id, FeatureComparison comparison, FeatureValue&& value)
    {
        return adopt_own(*new Derived(Type::Range, id,
            Range {
                .right_comparison = comparison,
                .right_value = move(value),
            }));
    }

    static NonnullOwnPtr<Derived> range(FeatureValue&& left_value, FeatureComparison left_comparison, FeatureID id, FeatureComparison right_comparison, FeatureValue&& right_value)
    {
        return adopt_own(*new Derived(Type::Range, id,
            Range {
                .left_value = move(left_value),
                .left_comparison = left_comparison,
                .right_comparison = right_comparison,
                .right_value = move(right_value),
            }));
    }

    Type type() const { return m_type; }
    FeatureID id() const { return m_id; }
    FeatureValue const& value() const { return m_value.template get<FeatureValue>(); }
    Range const& range() const { return m_value.template get<Range>(); }

    virtual String to_string() const override
    {
        // NB: Even though the surrounding boolean-expression grammar owns the parentheses, feature serialization
        //     includes them so callers do not need a wrapper node just for serialization.
        switch (m_type) {
        case Type::IsTrue:
            return MUST(String::formatted("({})", Derived::serialize_feature_id(m_id)));
        case Type::ExactValue:
            return MUST(String::formatted("({}: {})", Derived::serialize_feature_id(m_id), value().to_string(SerializationMode::Normal)));
        case Type::MinValue:
            return MUST(String::formatted("(min-{}: {})", Derived::serialize_feature_id(m_id), value().to_string(SerializationMode::Normal)));
        case Type::MaxValue:
            return MUST(String::formatted("(max-{}: {})", Derived::serialize_feature_id(m_id), value().to_string(SerializationMode::Normal)));
        case Type::Range: {
            auto& range = this->range();
            StringBuilder builder;
            builder.append('(');
            if (range.left_comparison.has_value())
                builder.appendff("{} {} ", range.left_value->to_string(SerializationMode::Normal), string_from_feature_comparison(*range.left_comparison));
            builder.append(Derived::serialize_feature_id(m_id));
            if (range.right_comparison.has_value())
                builder.appendff(" {} {}", string_from_feature_comparison(*range.right_comparison), range.right_value->to_string(SerializationMode::Normal));
            builder.append(')');
            return builder.to_string_without_validation();
        }
        }
        VERIFY_NOT_REACHED();
    }

protected:
    FeatureQuery(Type type, FeatureID id, Variant<Empty, FeatureValue, Range> value = {})
        : m_type(type)
        , m_id(id)
        , m_value(move(value))
    {
    }

    MatchResult evaluate_internal(FeatureValue const& queried_value, ComputationContext const& computation_context) const
    {
        switch (type()) {
        case Type::IsTrue:
            if (queried_value.is_integer())
                return as_match_result(queried_value.integer(computation_context) != 0);
            if (queried_value.is_length()) {
                auto length = queried_value.length(computation_context);
                return as_match_result(length.raw_value() != 0);
            }
            // FIXME: I couldn't figure out from the spec how ratios should be evaluated in a boolean context.
            if (queried_value.is_ratio())
                return as_match_result(!queried_value.ratio(computation_context).is_degenerate());
            if (queried_value.is_resolution())
                return as_match_result(queried_value.resolution(computation_context).to_dots_per_pixel() != 0);
            if (queried_value.is_ident()) {
                if (Derived::keyword_is_falsey(id(), queried_value.ident()))
                    return MatchResult::False;
                return MatchResult::True;
            }
            return MatchResult::False;

        case Type::ExactValue:
            return compare_feature_values(value(), FeatureComparison::Equal, queried_value, computation_context);

        case Type::MinValue:
            return compare_feature_values(queried_value, FeatureComparison::GreaterThanOrEqual, value(), computation_context);

        case Type::MaxValue:
            return compare_feature_values(queried_value, FeatureComparison::LessThanOrEqual, value(), computation_context);

        case Type::Range: {
            auto const& range = this->range();
            if (range.left_comparison.has_value()) {
                if (auto const left_result = compare_feature_values(*range.left_value, *range.left_comparison, queried_value, computation_context); left_result != MatchResult::True)
                    return left_result;
            }

            if (range.right_comparison.has_value()) {
                if (auto const right_result = compare_feature_values(queried_value, *range.right_comparison, *range.right_value, computation_context); right_result != MatchResult::True)
                    return right_result;
            }

            return MatchResult::True;
        }
        }

        VERIFY_NOT_REACHED();
    }

    Type m_type;
    FeatureID m_id;
    Variant<Empty, FeatureValue, Range> m_value {};
};

}
