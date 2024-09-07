/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSLayerBlockRule.h"
#include <LibWeb/Bindings/CSSLayerBlockRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(CSSLayerBlockRule);

JS::NonnullGCPtr<CSSLayerBlockRule> CSSLayerBlockRule::create(JS::Realm& realm, FlyString name, CSSRuleList& rules)
{
    return realm.heap().allocate<CSSLayerBlockRule>(realm, realm, move(name), rules);
}

FlyString CSSLayerBlockRule::next_unique_anonymous_layer_name()
{
    static u64 s_anonymous_layer_id = 0;
    return MUST(String::formatted("#{}", ++s_anonymous_layer_id));
}

CSSLayerBlockRule::CSSLayerBlockRule(JS::Realm& realm, FlyString name, CSSRuleList& rules)
    : CSSGroupingRule(realm, rules)
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
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSLayerBlockRule);
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

FlyString CSSLayerBlockRule::internal_qualified_name(Badge<StyleComputer>) const
{
    auto const& parent_name = parent_layer_internal_qualified_name();
    if (parent_name.is_empty())
        return m_name_internal;
    return MUST(String::formatted("{}.{}", parent_name, m_name_internal));
}

}
