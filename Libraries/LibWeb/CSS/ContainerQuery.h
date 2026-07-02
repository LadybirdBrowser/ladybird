/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>
#include <LibWeb/CSS/BooleanExpression.h>
#include <LibWeb/CSS/FeatureQuery.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/PropertyNameAndID.h>

namespace Web::CSS {

enum class SizeFeatureID : u8 {
    AspectRatio,
    BlockSize,
    Height,
    InlineSize,
    Orientation,
    Width,
};

// https://drafts.csswg.org/css-conditional-5/#size-container
class SizeFeature final : public FeatureQuery<SizeFeature, SizeFeatureID> {
public:
    using Base = FeatureQuery<SizeFeature, SizeFeatureID>;

    virtual MatchResult evaluate(BooleanExpressionEvaluationContext const&) const override;
    virtual void collect_container_query_feature_requirements(ContainerQueryFeatureRequirements&) const override;
    virtual void dump(StringBuilder&, int indent_levels = 0) const override;

    static StringView serialize_feature_id(SizeFeatureID);
    static bool keyword_is_falsey(SizeFeatureID, Keyword);

private:
    friend Base;

    SizeFeature(Type type, SizeFeatureID id, Variant<Empty, FeatureValue, Range> value = {})
        : Base(type, id, move(value))
    {
    }
};

// https://drafts.csswg.org/css-conditional-5/#typedef-style-feature
class StyleFeature final : public BooleanExpression {
public:
    static NonnullOwnPtr<StyleFeature> create_boolean(PropertyNameAndID);
    static NonnullOwnPtr<StyleFeature> create_plain(PropertyNameAndID, Vector<Parser::ComponentValue> value);

    virtual MatchResult evaluate(BooleanExpressionEvaluationContext const&) const override;
    virtual void collect_container_query_feature_requirements(ContainerQueryFeatureRequirements&) const override;
    virtual String to_string() const override;
    virtual void dump(StringBuilder&, int indent_levels = 0) const override;

private:
    StyleFeature(PropertyNameAndID property, Optional<Vector<Parser::ComponentValue>> value)
        : m_property(move(property))
        , m_value(move(value))
    {
    }

    PropertyNameAndID m_property;
    Optional<Vector<Parser::ComponentValue>> m_value;
};

// https://drafts.csswg.org/css-conditional-5/#container-rule
class WEB_API ContainerQuery final : public RefCounted<ContainerQuery> {
public:
    static NonnullRefPtr<ContainerQuery> create(NonnullOwnPtr<BooleanExpression>&&);

    bool matches() const { return m_matches; }
    ContainerQueryFeatureRequirements const& feature_requirements() const { return m_feature_requirements; }
    bool contains_size_feature() const { return m_feature_requirements.contains_size_feature(); }
    MatchResult evaluate(DOM::AbstractElement const&, Optional<FlyString> const& container_name) const;
    String to_string() const;

    void dump(StringBuilder&, int indent_levels = 0) const;

private:
    explicit ContainerQuery(NonnullOwnPtr<BooleanExpression>&&);

    NonnullOwnPtr<BooleanExpression> m_condition;
    ContainerQueryFeatureRequirements m_feature_requirements;
    bool m_matches { false };
};

Optional<SizeFeatureID> size_feature_id_from_string(StringView);
StringView string_from_size_feature_id(SizeFeatureID);
bool size_feature_type_is_range(SizeFeatureID);
bool container_name_matches(DOM::Element const&, Optional<FlyString> const& container_name);

}
