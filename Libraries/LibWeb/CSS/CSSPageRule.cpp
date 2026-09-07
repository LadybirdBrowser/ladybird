/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/CSSPageRule.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/Dump.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#include <AK/Utf16StringBuilder.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSPageRule);

GC::Ref<CSSPageRule> CSSPageRule::create(RustRule rule, CSSRuleList& rules)
{
    return GC::Heap::the().allocate<CSSPageRule>(move(rule), rules);
}

CSSPageRule::CSSPageRule(RustRule rule, CSSRuleList& rules)
    : CSSGroupingRule(rules, move(rule))
    , m_descriptors(Parser::ValueParserFFI::rust_descriptor_block_retain(native_rule().payload().descriptors))
{
}

size_t CSSPageRule::external_memory_size() const
{
    return JS::saturating_add_external_memory_size(Base::external_memory_size(), m_descriptors.external_memory_size());
}

GC::Ref<CSSPageDescriptors> CSSPageRule::style() const
{
    if (!m_style) {
        m_style = CSSPageDescriptors::create(m_descriptors.retain());
        m_style->set_parent_rule(const_cast<CSSPageRule&>(*this));
    }
    return *m_style;
}

// https://drafts.csswg.org/cssom/#dom-csspagerule-selectortext
Utf16String CSSPageRule::selector_text() const
{
    return RustPageSelectors { native_rule().payload().page_selectors }.serialize();
}

// https://drafts.csswg.org/cssom/#dom-csspagerule-selectortext
void CSSPageRule::set_selector_text(Utf16View text)
{
    // On setting the selectorText attribute these steps must be run:
    // 1. Run the parse a list of CSS page selectors algorithm on the given value.
    auto page_selector_list = RustPageSelectors::parse(text);

    // 2. If the algorithm returns a non-null value replace the associated selector list with the returned value.
    if (page_selector_list.has_value())
        Parser::ValueParserFFI::rust_rule_set_page_selectors(native_rule().handle(), page_selector_list->handle());

    // 3. Otherwise, if the algorithm returns a null value, do nothing.
}

// https://drafts.csswg.org/cssom/#ref-for-csspagerule
Utf16String CSSPageRule::serialized() const
{
    Utf16StringBuilder builder;

    // AD-HOC: There's no spec for this yet, but Chrome puts declarations before margin rules.
    builder.append_ascii("@page "sv);
    if (auto selector = selector_text(); !selector.is_empty()) {
        builder.append(selector);
        builder.append_ascii(' ');
    }
    builder.append_ascii("{ "sv);
    if (m_descriptors.size() > 0) {
        builder.append(style()->serialized());
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

void CSSPageRule::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_style);
}

void CSSPageRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Selector: {}\n", selector_text().to_utf8());
    dump_descriptors(builder, style(), indent_levels + 1);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Rules ({}):\n", css_rules().length());
    for (auto& rule : css_rules())
        dump_rule(builder, rule, indent_levels + 2);
}

}
