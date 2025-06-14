/*
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSSupportsRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSSupportsRule.h>
#include <LibWeb/CSS/Parser/Parser.h>

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

    builder.append("@supports "_sv);
    builder.append(condition_text());
    builder.append(" {\n"_sv);
    for (size_t i = 0; i < css_rules().length(); i++) {
        auto rule = css_rules().item(i);
        if (i != 0)
            builder.append("\n"_sv);
        builder.append("  "_sv);
        builder.append(rule->css_text());
    }
    builder.append("\n}"_sv);

    return MUST(builder.to_string());
}

}
