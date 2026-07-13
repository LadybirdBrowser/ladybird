/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSPageRule.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSPageRule.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/Dump.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#include <AK/Utf16StringBuilder.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSPageRule);

GC::Ref<CSSPageRule> CSSPageRule::create(JS::Realm& realm, PageSelectorList&& selectors, GC::Ref<CSSPageDescriptors> style, CSSRuleList& rules)
{
    return realm.create<CSSPageRule>(realm, move(selectors), style, rules);
}

CSSPageRule::CSSPageRule(JS::Realm& realm, PageSelectorList&& selectors, GC::Ref<CSSPageDescriptors> style, CSSRuleList& rules)
    : CSSGroupingRule(realm, rules, Type::Page)
    , m_selectors(move(selectors))
    , m_style(style)
{
    m_style->set_parent_rule(*this);
}

void CSSPageRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSPageRule);
    Base::initialize(realm);
}

// https://drafts.csswg.org/cssom/#dom-csspagerule-selectortext
Utf16String CSSPageRule::selector_text() const
{
    Utf16StringBuilder builder;

    bool first = true;
    for (auto const& selector : m_selectors) {
        if (!first)
            builder.append_ascii(", "sv);
        first = false;
        selector.serialize_to(builder);
    }

    return builder.to_string();
}

// https://drafts.csswg.org/cssom/#dom-csspagerule-selectortext
void CSSPageRule::set_selector_text(Utf16View text)
{
    // On setting the selectorText attribute these steps must be run:
    // 1. Run the parse a list of CSS page selectors algorithm on the given value.
    auto page_selector_list = parse_page_selector_list(Parser::ParsingParams {}, text);

    // 2. If the algorithm returns a non-null value replace the associated selector list with the returned value.
    if (page_selector_list.has_value())
        m_selectors = page_selector_list.release_value();

    // 3. Otherwise, if the algorithm returns a null value, do nothing.
}

// https://drafts.csswg.org/cssom/#ref-for-csspagerule
Utf16String CSSPageRule::serialized() const
{
    auto& descriptors = *m_style;

    Utf16StringBuilder builder;

    // AD-HOC: There's no spec for this yet, but Chrome puts declarations before margin rules.
    builder.append_ascii("@page "sv);
    if (auto selector = selector_text(); !selector.is_empty()) {
        builder.append(selector);
        builder.append_ascii(' ');
    }
    builder.append_ascii("{ "sv);
    if (descriptors.length() > 0) {
        builder.append(descriptors.serialized());
        builder.append_ascii(' ');
    }
    for (size_t i = 0; i < css_rules().length(); i++) {
        auto rule = css_rules().item(i);
        auto result = rule->serialized();

        if (result.is_empty())
            continue;

        builder.appendff("{} ", result);
    }
    builder.append_ascii("}"sv);

    return builder.to_string();
}

void CSSPageRule::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_style);
}

void CSSPageRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Selector: {}\n", selector_text().to_utf8());
    dump_descriptors(builder, descriptors(), indent_levels + 1);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Rules ({}):\n", css_rules().length());
    for (auto& rule : css_rules())
        dump_rule(builder, rule, indent_levels + 2);
}

}
