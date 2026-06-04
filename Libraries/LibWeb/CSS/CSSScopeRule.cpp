/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSScopeRule.h"
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/CSSScopeRule.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSScopeRule);

GC::Ref<CSSScopeRule> CSSScopeRule::create(JS::Realm& realm, Optional<SelectorList>&& start_selectors, Optional<SelectorList>&& end_selectors, CSSRuleList& rules)
{
    return realm.create<CSSScopeRule>(realm, move(start_selectors), move(end_selectors), rules);
}

CSSScopeRule::CSSScopeRule(JS::Realm& realm, Optional<SelectorList>&& start_selectors, Optional<SelectorList>&& end_selectors, CSSRuleList& rules)
    : CSSGroupingRule(realm, rules, Type::Scope)
    , m_start_selectors(move(start_selectors))
    , m_end_selectors(move(end_selectors))
{
}

CSSScopeRule::~CSSScopeRule() = default;

void CSSScopeRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSScopeRule);
    Base::initialize(realm);
}

void CSSScopeRule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_cached_nearest_ancestor_scope_rule);
}

Optional<String> CSSScopeRule::start() const
{
    if (m_start_selectors.has_value())
        return serialize_a_group_of_selectors(*m_start_selectors);
    return {};
}

Optional<String> CSSScopeRule::end() const
{
    if (m_end_selectors.has_value())
        return serialize_a_group_of_selectors(*m_end_selectors);
    return {};
}

Optional<SelectorList> const& CSSScopeRule::start_selectors_for_matching() const
{
    if (!m_start_selectors.has_value())
        return m_start_selectors;

    if (!m_cached_start_selectors_for_matching.has_value())
        m_cached_start_selectors_for_matching = absolutize_selectors_relative_to(*m_start_selectors, m_parent_rule);
    return m_cached_start_selectors_for_matching;
}

SelectorList adapt_scope_end_selectors_for_matching(SelectorList const& selectors)
{
    // NB: This is a modified version of adapt_nested_relative_selector_list().
    // End selectors are implicitly prepended with `:scope` if that doesn't already appear.
    // We also prepend it if the selector is relative.

    SelectorList new_list;
    new_list.ensure_capacity(selectors.size());
    for (auto const& selector : selectors) {
        auto first_combinator = selector->compound_selectors().first().combinator;

        if (!first_is_one_of(first_combinator, Selector::Combinator::None, Selector::Combinator::Descendant)
            || (!selector->contains_the_nesting_selector() && !selector->contains_pseudo_class(PseudoClass::Scope))) {
            new_list.append(selector->relative_to(Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::PseudoClass,
                .value = Selector::SimpleSelector::PseudoClassSelector {
                    .type = PseudoClass::Scope,
                } }));
        } else if (first_combinator == Selector::Combinator::Descendant) {
            // Replace leading descendant combinator (whitespace) with none, because we're not actually relative.
            auto copied_compound_selectors = selector->compound_selectors();
            copied_compound_selectors.first().combinator = Selector::Combinator::None;
            new_list.append(Selector::create(move(copied_compound_selectors)));
        } else {
            new_list.append(selector);
        }
    }
    return new_list;
}

Optional<SelectorList> const& CSSScopeRule::end_selectors_for_matching() const
{
    if (!m_end_selectors.has_value())
        return m_end_selectors;

    if (!m_cached_end_selectors_for_matching.has_value())
        m_cached_end_selectors_for_matching = adapt_scope_end_selectors_for_matching(*m_end_selectors);
    return m_cached_end_selectors_for_matching;
}

Optional<SelectorList> const& scope_start_selectors_for_matching(CSSRule const& scope_rule)
{
    if (auto const* css_scope_rule = as_if<CSSScopeRule const>(scope_rule))
        return css_scope_rule->start_selectors_for_matching();
    if (auto const* import_rule = as_if<CSSImportRule const>(scope_rule)) {
        VERIFY(import_rule->has_scope());
        return import_rule->scope_start_selectors_for_matching();
    }
    VERIFY_NOT_REACHED();
}

Optional<SelectorList> const& scope_end_selectors_for_matching(CSSRule const& scope_rule)
{
    if (auto const* css_scope_rule = as_if<CSSScopeRule const>(scope_rule))
        return css_scope_rule->end_selectors_for_matching();
    if (auto const* import_rule = as_if<CSSImportRule const>(scope_rule)) {
        VERIFY(import_rule->has_scope());
        return import_rule->scope_end_selectors_for_matching();
    }
    VERIFY_NOT_REACHED();
}

GC::Ptr<CSSImportRule const> nearest_scoped_owner_import(CSSStyleSheet const* style_sheet)
{
    for (auto const* current_style_sheet = style_sheet; current_style_sheet;) {
        auto owner_rule = current_style_sheet->owner_rule();
        if (auto const* import_rule = as_if<CSSImportRule const>(owner_rule.ptr()); import_rule && import_rule->has_scope())
            return import_rule;
        current_style_sheet = owner_rule ? owner_rule->parent_style_sheet() : nullptr;
    }
    return nullptr;
}

GC::Ptr<CSSRule const> nearest_ancestor_scope_rule_for_matching(CSSRule const& scope_rule)
{
    for (auto const* parent_rule = scope_rule.parent_rule(); parent_rule; parent_rule = parent_rule->parent_rule()) {
        if (is<CSSScopeRule>(*parent_rule))
            return parent_rule;
    }

    return nearest_scoped_owner_import(scope_rule.parent_style_sheet());
}

GC::Ptr<CSSScopeRule const> CSSScopeRule::nearest_ancestor_scope_rule() const
{
    if (m_cached_nearest_ancestor_scope_rule.has_value())
        return m_cached_nearest_ancestor_scope_rule.value();

    for (auto const* parent = parent_rule(); parent; parent = parent->parent_rule()) {
        if (auto const* scope_rule = as_if<CSSScopeRule const>(parent)) {
            m_cached_nearest_ancestor_scope_rule = scope_rule;
            return m_cached_nearest_ancestor_scope_rule.value();
        }
    }

    m_cached_nearest_ancestor_scope_rule = nullptr;
    return m_cached_nearest_ancestor_scope_rule.value();
}

void CSSScopeRule::clear_caches()
{
    Base::clear_caches();
    m_cached_start_selectors_for_matching.clear();
    m_cached_end_selectors_for_matching.clear();
    m_cached_nearest_ancestor_scope_rule.clear();
}

// https://drafts.csswg.org/cssom-1/#serialize-a-css-rule
String CSSScopeRule::serialized() const
{
    // AD-HOC: There is no spec for this yet.
    StringBuilder builder;
    builder.append("@scope"sv);

    if (auto start = this->start(); start.has_value())
        builder.appendff(" ({})", *start);

    if (auto end = this->end(); end.has_value())
        builder.appendff(" to ({})", *end);

    builder.append(" {\n"sv);

    for (size_t i = 0; i < css_rules().length(); i++) {
        auto rule = css_rules().item(i);
        auto result = rule->css_text();

        if (result.is_empty())
            continue;

        builder.append("  "sv);
        builder.append(result);
        builder.append('\n');
    }

    builder.append('}');

    return builder.to_string_without_validation();
}

void CSSScopeRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    if (m_start_selectors.has_value()) {
        builder.appendff("Start selectors ({}):\n", m_start_selectors->size());
        for (auto& selector : *m_start_selectors)
            dump_selector(builder, selector, indent_levels + 2);

        dump_indent(builder, indent_levels + 1);
        builder.appendff("Absolutized start selectors:\n");
        for (auto& selector : start_selectors_for_matching().value()) {
            dump_selector(builder, selector, indent_levels + 2);
        }
    } else {
        builder.append("Start selectors: <none>\n"sv);
    }

    dump_indent(builder, indent_levels + 1);
    if (m_end_selectors.has_value()) {
        builder.appendff("End selectors ({}):\n", m_end_selectors->size());
        for (auto& selector : *m_end_selectors)
            dump_selector(builder, selector, indent_levels + 2);

        dump_indent(builder, indent_levels + 1);
        builder.appendff("Absolutized end selectors:\n");
        for (auto& selector : end_selectors_for_matching().value()) {
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
