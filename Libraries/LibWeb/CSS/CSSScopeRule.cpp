/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSScopeRule.h"
#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSScopeRule);

GC::Ref<CSSScopeRule> CSSScopeRule::create(RustRule rule, CSSRuleList& rules)
{
    return GC::Heap::the().allocate<CSSScopeRule>(move(rule), rules);
}

CSSScopeRule::CSSScopeRule(RustRule rule, CSSRuleList& rules)
    : CSSGroupingRule(rules, move(rule))
    , m_selectors(RustScopeSelectors::create(native_rule().payload().scope))
{
}

CSSScopeRule::~CSSScopeRule() = default;

void CSSScopeRule::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

Optional<Utf16String> CSSScopeRule::start() const
{
    if (start_selectors().has_value())
        return serialize_a_group_of_selectors(*start_selectors());
    return {};
}

Optional<Utf16String> CSSScopeRule::end() const
{
    if (end_selectors().has_value())
        return serialize_a_group_of_selectors(*end_selectors());
    return {};
}

// https://drafts.csswg.org/cssom-1/#serialize-a-css-rule
Utf16String CSSScopeRule::serialized() const
{
    // AD-HOC: There is no spec for this yet.
    Utf16StringBuilder builder;
    builder.append_ascii("@scope"sv);

    if (auto start = this->start(); start.has_value())
        builder.appendff(" ({})", *start);

    if (auto end = this->end(); end.has_value())
        builder.appendff(" to ({})", *end);

    builder.append_ascii(" {\n"sv);

    for (size_t i = 0; i < css_rules().length(); i++) {
        auto rule = css_rules().item(i);
        auto result = rule->serialized();

        if (result.is_empty())
            continue;

        builder.append_ascii("  "sv);
        builder.append(result);
        builder.append_ascii('\n');
    }

    builder.append_ascii('}');

    return builder.to_string();
}

void CSSScopeRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    if (start_selectors().has_value()) {
        builder.appendff("Start selectors ({}):\n", start_selectors()->size());
        for (auto& selector : *start_selectors())
            dump_selector(builder, selector, indent_levels + 2);

        dump_indent(builder, indent_levels + 1);
        builder.appendff("Absolutized start selectors:\n");
        auto matching_selectors = scope_start_selectors_for_rule(native_rule());
        for (auto& selector : matching_selectors.value()) {
            dump_selector(builder, selector, indent_levels + 2);
        }
    } else {
        builder.append("Start selectors: <none>\n"sv);
    }

    dump_indent(builder, indent_levels + 1);
    if (end_selectors().has_value()) {
        builder.appendff("End selectors ({}):\n", end_selectors()->size());
        for (auto& selector : *end_selectors())
            dump_selector(builder, selector, indent_levels + 2);

        dump_indent(builder, indent_levels + 1);
        builder.appendff("Absolutized end selectors:\n");
        auto matching_selectors = scope_end_selectors_for_rule(native_rule());
        for (auto& selector : matching_selectors.value()) {
            dump_selector(builder, selector, indent_levels + 2);
        }
    } else {
        builder.append("End selectors: <none>\n"sv);
    }

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Rules ({}):\n", css_rules().length());
    for (auto& rule : css_rules())
        dump_rule(builder, rule, indent_levels + 2);
}

}
