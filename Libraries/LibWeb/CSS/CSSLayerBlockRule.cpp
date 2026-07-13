/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSLayerBlockRule.h"
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/Bindings/CSSLayerBlockRule.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSLayerBlockRule);

GC::Ref<CSSLayerBlockRule> CSSLayerBlockRule::create(JS::Realm& realm, Utf16FlyString name, CSSRuleList& rules)
{
    return realm.create<CSSLayerBlockRule>(realm, move(name), rules);
}

Utf16FlyString CSSLayerBlockRule::next_unique_anonymous_layer_name()
{
    static u64 s_anonymous_layer_id = 0;
    Utf16StringBuilder builder;
    builder.appendff("#{}", ++s_anonymous_layer_id);
    auto name = builder.to_string();
    return Utf16FlyString::from_utf16(name.utf16_view());
}

CSSLayerBlockRule::CSSLayerBlockRule(JS::Realm& realm, Utf16FlyString name, CSSRuleList& rules)
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

Utf16String CSSLayerBlockRule::serialized() const
{
    // AD-HOC: No spec yet, so this is based on the @media serialization algorithm.
    Utf16StringBuilder builder;
    builder.append_ascii("@layer"sv);
    if (!m_name.is_empty())
        builder.appendff(" {}", m_name);

    builder.append_ascii(" {\n"sv);
    // AD-HOC: All modern browsers omit the ending newline if there are no CSS rules, so let's do the same.
    if (css_rules().length() == 0) {
        builder.append_ascii('}');
        return builder.to_string();
    }

    for (size_t i = 0; i < css_rules().length(); i++) {
        auto rule = css_rules().item(i);
        if (i != 0)
            builder.append_ascii("\n"sv);
        builder.append_ascii("  "sv);
        builder.append(rule->serialized());
    }

    builder.append_ascii("\n}"sv);

    return builder.to_string();
}

Utf16FlyString CSSLayerBlockRule::internal_qualified_name(Badge<StyleScope>) const
{
    auto const& parent_name = parent_layer_internal_qualified_name();
    if (parent_name.is_empty())
        return m_name_internal;
    Utf16StringBuilder builder;
    builder.append(parent_name);
    builder.append_ascii('.');
    builder.append(m_name_internal);
    auto qualified_name = builder.to_string();
    return Utf16FlyString::from_utf16(qualified_name.utf16_view());
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
