/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSSupportsRule.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSSupportsRule);

GC::Ref<CSSSupportsRule> CSSSupportsRule::create(RustQueryHandle supports, CSSRuleList& rules)
{
    return GC::Heap::the().allocate<CSSSupportsRule>(move(supports), rules);
}

CSSSupportsRule::CSSSupportsRule(RustQueryHandle supports, CSSRuleList& rules)
    : CSSConditionRule(rules, Type::Supports)
    , m_supports(move(supports))
{
}

Utf16String CSSSupportsRule::serialized_condition_text() const
{
    return serialize_supports_condition(m_supports);
}

// https://www.w3.org/TR/cssom-1/#serialize-a-css-rule
Utf16String CSSSupportsRule::serialized() const
{
    // Note: The spec doesn't cover this yet, so I'm roughly following the spec for the @media rule.
    // It should be pretty close!

    Utf16StringBuilder builder;

    builder.append_ascii("@supports "sv);
    builder.append(serialized_condition_text());
    builder.append_ascii(" {\n"sv);
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

void CSSSupportsRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_supports_condition(builder, supports(), indent_levels + 1);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Rules ({}):\n", css_rules().length());
    for (auto& rule : css_rules())
        dump_rule(builder, rule, indent_levels + 2);
}

}
