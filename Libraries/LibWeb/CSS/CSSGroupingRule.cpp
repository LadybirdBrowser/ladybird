/*
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSGroupingRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

CSSGroupingRule::CSSGroupingRule(JS::Realm& realm, CSSRuleList& rules, Type type)
    : CSSRule(realm, type)
    , m_rules(rules)
{
    m_rules->set_owner_rule(*this);
    for (auto& rule : *m_rules)
        rule->set_parent_rule(this);
}

void CSSGroupingRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSGroupingRule);
    Base::initialize(realm);
}

void CSSGroupingRule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rules);
}

void CSSGroupingRule::clear_caches()
{
    Base::clear_caches();
    for (auto& rule : *m_rules)
        rule->clear_caches();
}

// https://drafts.csswg.org/cssom/#dom-cssgroupingrule-insertrule
WebIDL::ExceptionOr<u32> CSSGroupingRule::insert_rule(StringView rule, u32 index)
{
    // The insertRule(rule, index) method must return the result of invoking insert a CSS rule rule into the child CSS
    // rules at index, with the nested flag set.
    TRY(m_rules->insert_a_css_rule(rule, index, CSSRuleList::Nested::Yes, m_parent_style_sheet->declared_namespaces()));

    // AD-HOC: The spec doesn't say where to set the parent rule, so we'll do it here.
    m_rules->item(index)->set_parent_rule(this);
    return index;
}

WebIDL::ExceptionOr<void> CSSGroupingRule::delete_rule(u32 index)
{
    return m_rules->remove_a_css_rule(index);
}

void CSSGroupingRule::for_each_effective_rule(TraversalOrder order, Function<void(Web::CSS::CSSRule const&)> const& callback) const
{
    m_rules->for_each_effective_rule(order, callback);
}

void CSSGroupingRule::set_parent_style_sheet(CSSStyleSheet* parent_style_sheet)
{
    CSSRule::set_parent_style_sheet(parent_style_sheet);
    for (auto& rule : *m_rules)
        rule->set_parent_style_sheet(parent_style_sheet);
}

}
