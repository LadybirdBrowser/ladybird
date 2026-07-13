/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/CSSMediaRule.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMediaRule);

GC::Ref<CSSMediaRule> CSSMediaRule::create(JS::Realm& realm, MediaList& media_queries, CSSRuleList& rules)
{
    return realm.create<CSSMediaRule>(realm, media_queries, rules);
}

CSSMediaRule::CSSMediaRule(JS::Realm& realm, MediaList& media, CSSRuleList& rules)
    : CSSConditionRule(realm, rules, Type::Media)
    , m_media(media)
{
}

void CSSMediaRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSMediaRule);
    Base::initialize(realm);
}

void CSSMediaRule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media);
}

Utf16String CSSMediaRule::serialized_condition_text() const
{
    return m_media->media_text();
}

// https://www.w3.org/TR/cssom-1/#serialize-a-css-rule
Utf16String CSSMediaRule::serialized() const
{
    // The result of concatenating the following:
    Utf16StringBuilder builder;

    // 1. The string "@media", followed by a single SPACE (U+0020).
    builder.append_ascii("@media "sv);
    // 2. The result of performing serialize a media query list on rule’s media query list.
    builder.append(serialized_condition_text());
    // 3. A single SPACE (U+0020), followed by the string "{", i.e., LEFT CURLY BRACKET (U+007B), followed by a newline.
    builder.append_ascii(" {\n"sv);
    // 4. The result of performing serialize a CSS rule on each rule in the rule’s cssRules list,
    //    filtering out empty strings, indenting each item with two spaces, all joined with newline.
    for (size_t i = 0; i < css_rules().length(); i++) {
        auto rule = css_rules().item(i);
        auto result = rule->serialized();

        if (result.is_empty())
            continue;

        builder.append_ascii("  "sv);
        builder.append(result);
        builder.append_ascii('\n');
    }
    // 5. A newline, followed by the string "}", i.e., RIGHT CURLY BRACKET (U+007D)
    // AD-HOC: All modern browsers omit the ending newline if there are no CSS rules, so let's do the same.
    //         If there are rules, the required newline will be appended in the for-loop above.
    builder.append_ascii('}');

    return builder.to_string();
}

void CSSMediaRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    m_media->dump(builder, indent_levels + 1);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Rules ({}):\n", css_rules().length());
    for (auto& rule : css_rules())
        dump_rule(builder, rule, indent_levels + 2);
}

}
