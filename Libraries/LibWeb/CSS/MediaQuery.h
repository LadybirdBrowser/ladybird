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
#include <LibWeb/CSS/FeatureQuery.h>
#include <LibWeb/CSS/MediaFeatureID.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>

namespace Web::CSS {

// https://www.w3.org/TR/mediaqueries-4/#mq-features
class MediaFeature final : public FeatureQuery<MediaFeature, MediaFeatureID> {
public:
    using Base = FeatureQuery<MediaFeature, MediaFeatureID>;

    virtual MatchResult evaluate(BooleanExpressionEvaluationContext const&) const override;
    virtual void dump(StringBuilder&, int indent_levels = 0) const override;

    static StringView serialize_feature_id(MediaFeatureID);
    static bool keyword_is_falsey(MediaFeatureID, Keyword);

private:
    friend Base;

    MediaFeature(Type type, MediaFeatureID id, Variant<Empty, FeatureValue, Range> value = {})
        : Base(type, id, move(value))
    {
    }
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
