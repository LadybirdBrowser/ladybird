/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <LibWeb/CSS/BooleanExpression.h>
#include <LibWeb/CSS/MediaFeatureID.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>

namespace Web::CSS {

// https://www.w3.org/TR/mediaqueries-4/#typedef-mf-value
class MediaFeatureValue {
public:
    enum class Type : u8 {
        Ident,
        Length,
        Ratio,
        Resolution,
        Integer,
        Unknown,
    };

    explicit MediaFeatureValue(Type type, NonnullRefPtr<StyleValue const> value)
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
    bool is_same_type(MediaFeatureValue const& other) const { return m_type == other.m_type; }

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

// https://www.w3.org/TR/mediaqueries-4/#mq-features
class MediaFeature final : public BooleanExpression {
public:
    enum class Comparison : u8 {
        Equal,
        LessThan,
        LessThanOrEqual,
        GreaterThan,
        GreaterThanOrEqual,
    };

    // Corresponds to `<mf-boolean>` grammar
    static NonnullOwnPtr<MediaFeature> boolean(MediaFeatureID id)
    {
        return adopt_own(*new MediaFeature(Type::IsTrue, id));
    }

    // Corresponds to `<mf-plain>` grammar
    static NonnullOwnPtr<MediaFeature> plain(MediaFeatureID id, MediaFeatureValue value)
    {
        return adopt_own(*new MediaFeature(Type::ExactValue, move(id), move(value)));
    }
    static NonnullOwnPtr<MediaFeature> min(MediaFeatureID id, MediaFeatureValue value)
    {
        return adopt_own(*new MediaFeature(Type::MinValue, id, move(value)));
    }
    static NonnullOwnPtr<MediaFeature> max(MediaFeatureID id, MediaFeatureValue value)
    {
        return adopt_own(*new MediaFeature(Type::MaxValue, id, move(value)));
    }

    static NonnullOwnPtr<MediaFeature> half_range(MediaFeatureValue value, Comparison comparison, MediaFeatureID id)
    {
        return adopt_own(*new MediaFeature(Type::Range, id,
            Range {
                .left_value = move(value),
                .left_comparison = comparison,
            }));
    }
    static NonnullOwnPtr<MediaFeature> half_range(MediaFeatureID id, Comparison comparison, MediaFeatureValue value)
    {
        return adopt_own(*new MediaFeature(Type::Range, id,
            Range {
                .right_comparison = comparison,
                .right_value = move(value),
            }));
    }

    // Corresponds to `<mf-range>` grammar, with two comparisons
    static NonnullOwnPtr<MediaFeature> range(MediaFeatureValue left_value, Comparison left_comparison, MediaFeatureID id, Comparison right_comparison, MediaFeatureValue right_value)
    {
        return adopt_own(*new MediaFeature(Type::Range, id,
            Range {
                .left_value = move(left_value),
                .left_comparison = left_comparison,
                .right_comparison = right_comparison,
                .right_value = move(right_value),
            }));
    }

    virtual MatchResult evaluate(DOM::Document const*) const override;
    virtual String to_string() const override;
    virtual void dump(StringBuilder&, int indent_levels = 0) const override;

private:
    enum class Type : u8 {
        IsTrue,
        ExactValue,
        MinValue,
        MaxValue,
        Range,
    };

    struct Range {
        Optional<MediaFeatureValue> left_value {};
        Optional<Comparison> left_comparison {};
        Optional<Comparison> right_comparison {};
        Optional<MediaFeatureValue> right_value {};
    };

    MediaFeature(Type type, MediaFeatureID id, Variant<Empty, MediaFeatureValue, Range> value = {})
        : m_type(type)
        , m_id(move(id))
        , m_value(move(value))
    {
    }

    static MatchResult compare(DOM::Document const& document, MediaFeatureValue const& left, Comparison comparison, MediaFeatureValue const& right);
    MediaFeatureValue const& value() const { return m_value.get<MediaFeatureValue>(); }
    Range const& range() const { return m_value.get<Range>(); }

    Type m_type;
    MediaFeatureID m_id;
    Variant<Empty, MediaFeatureValue, Range> m_value {};
};

class MediaQuery : public RefCounted<MediaQuery> {
    friend class Parser::Parser;

public:
    ~MediaQuery() = default;

    // https://www.w3.org/TR/mediaqueries-4/#media-types
    enum class KnownMediaType : u8 {
        All,
        Print,
        Screen,
    };
    struct MediaType {
        FlyString name;
        Optional<KnownMediaType> known_type;
    };

    static NonnullRefPtr<MediaQuery> create_not_all();
    static NonnullRefPtr<MediaQuery> create() { return adopt_ref(*new MediaQuery); }

    bool matches() const { return m_matches; }
    bool evaluate(DOM::Document const&);
    String to_string() const;

    void dump(StringBuilder&, int indent_levels = 0) const;

private:
    MediaQuery() = default;

    // https://www.w3.org/TR/mediaqueries-4/#mq-not
    bool m_negated { false };
    MediaType m_media_type { .name = "all"_fly_string, .known_type = KnownMediaType::All };
    OwnPtr<BooleanExpression> m_media_condition { nullptr };

    // Cached value, updated by evaluate()
    bool m_matches { false };
};

String serialize_a_media_query_list(Vector<NonnullRefPtr<MediaQuery>> const&);

Optional<MediaQuery::KnownMediaType> media_type_from_string(StringView);
StringView to_string(MediaQuery::KnownMediaType);

}

namespace AK {

template<>
struct Formatter<Web::CSS::MediaFeature> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::MediaFeature const& media_feature)
    {
        return Formatter<StringView>::format(builder, media_feature.to_string());
    }
};

template<>
struct Formatter<Web::CSS::MediaQuery> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::MediaQuery const& media_query)
    {
        return Formatter<StringView>::format(builder, media_query.to_string());
    }
};

}
