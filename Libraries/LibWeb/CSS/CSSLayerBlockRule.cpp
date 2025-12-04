/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSLayerBlockRule.h"
#include <LibWeb/Bindings/CSSLayerBlockRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSLayerBlockRule);

GC::Ref<CSSLayerBlockRule> CSSLayerBlockRule::create(JS::Realm& realm, FlyString name, CSSRuleList& rules)
{
    return realm.create<CSSLayerBlockRule>(realm, move(name), rules);
}

FlyString CSSLayerBlockRule::next_unique_anonymous_layer_name()
{
    static u64 s_anonymous_layer_id = 0;
    return MUST(String::formatted("#{}", ++s_anonymous_layer_id));
}

CSSLayerBlockRule::CSSLayerBlockRule(JS::Realm& realm, FlyString name, CSSRuleList& rules)
    : CSSGroupingRule(realm, rules, Type::LayerBlock)
    , m_name(move(name))
{
    if (m_name.is_empty()) {
        m_name_internal = next_unique_anonymous_layer_name();
    } else {
        m_name_internal = m_name;
    }
}

void CSSLayerBlockRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSLayerBlockRule);
    Base::initialize(realm);
}

String CSSLayerBlockRule::serialized() const
{
    // AD-HOC: No spec yet, so this is based on the @media serialization algorithm.
    StringBuilder builder;
    builder.append("@layer"sv);
    if (!m_name.is_empty())
        builder.appendff(" {}", m_name);

    builder.append(" {\n"sv);
    // AD-HOC: All modern browsers omit the ending newline if there are no CSS rules, so let's do the same.
    if (css_rules().length() == 0) {
        builder.append('}');
        return builder.to_string_without_validation();
    }

    for (size_t i = 0; i < css_rules().length(); i++) {
        auto rule = css_rules().item(i);
        if (i != 0)
            builder.append("\n"sv);
        builder.append("  "sv);
        builder.append(rule->css_text());
    }

    builder.append("\n}"sv);

    return builder.to_string_without_validation();
}

FlyString CSSLayerBlockRule::internal_qualified_name(Badge<StyleScope>) const
{
    auto const& parent_name = parent_layer_internal_qualified_name();
    if (parent_name.is_empty())
        return m_name_internal;
    return MUST(String::formatted("{}.{}", parent_name, m_name_internal));
}

void CSSLayerBlockRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Name: `{}` (internal `{}`)\n", m_name, m_name_internal);
    dump_indent(builder, indent_levels + 1);
    builder.appendff("Rules ({}):\n", css_rules().length());
    for (auto& rule : css_rules())
        dump_rule(builder, rule, indent_levels + 2);
}

}
