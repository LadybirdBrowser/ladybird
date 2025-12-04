/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSSupportsRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSSupportsRule.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSSupportsRule);

GC::Ref<CSSSupportsRule> CSSSupportsRule::create(JS::Realm& realm, NonnullRefPtr<Supports>&& supports, CSSRuleList& rules)
{
    return realm.create<CSSSupportsRule>(realm, move(supports), rules);
}

CSSSupportsRule::CSSSupportsRule(JS::Realm& realm, NonnullRefPtr<Supports>&& supports, CSSRuleList& rules)
    : CSSConditionRule(realm, rules, Type::Supports)
    , m_supports(move(supports))
{
}

void CSSSupportsRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSSupportsRule);
    Base::initialize(realm);
}

String CSSSupportsRule::condition_text() const
{
    return m_supports->to_string();
}

// https://www.w3.org/TR/cssom-1/#serialize-a-css-rule
String CSSSupportsRule::serialized() const
{
    // Note: The spec doesn't cover this yet, so I'm roughly following the spec for the @media rule.
    // It should be pretty close!

    StringBuilder builder;

    builder.append("@supports "sv);
    builder.append(condition_text());
    builder.append(" {\n"sv);
    for (size_t i = 0; i < css_rules().length(); i++) {
        auto rule = css_rules().item(i);
        if (i != 0)
            builder.append("\n"sv);
        builder.append("  "sv);
        builder.append(rule->css_text());
    }
    builder.append("\n}"sv);

    return MUST(builder.to_string());
}

void CSSSupportsRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    supports().dump(builder, indent_levels + 1);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Rules ({}):\n", css_rules().length());
    for (auto& rule : css_rules())
        dump_rule(builder, rule, indent_levels + 2);
}

}
