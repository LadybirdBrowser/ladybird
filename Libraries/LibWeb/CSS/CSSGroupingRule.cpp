/*
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

CSSGroupingRule::CSSGroupingRule(CSSRuleList& rules, RustRule rule)
    : CSSRule(move(rule))
    , m_rules(rules)
{
    VERIFY(Parser::ValueParserFFI::rust_rule_children(native_rule().handle()) == rules.native_rules().handle());
    m_rules->set_owner_rule(*this);
}

void CSSGroupingRule::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rules);
}

void CSSGroupingRule::clear_caches()
{
    Base::clear_caches();
    m_rules->for_each_existing_rule([](CSSRule& rule) { rule.clear_caches(); });
}

// https://drafts.csswg.org/cssom/#dom-cssgroupingrule-insertrule
WebIDL::ExceptionOr<u32> CSSGroupingRule::insert_rule(Utf16View rule, u32 index)
{
    // The insertRule(rule, index) method must return the result of invoking insert a CSS rule rule into the child CSS
    // rules at index, with the nested flag set.
    RustNamespaceContext declared_namespaces;
    if (auto* sheet = parent_style_sheet())
        declared_namespaces = sheet->declared_namespaces();
    TRY(m_rules->insert_a_css_rule(rule, index, CSSRuleList::Nested::Yes, declared_namespaces));

    if (auto* sheet = parent_style_sheet()) {
        sheet->invalidate_image_resource_registration();
        if (auto* function = Parser::ValueParserFFI::rust_rule_containing_function(native_rule().handle()))
            record_style_rule_declarations_changed(RustRule { function }, *sheet);
        else
            record_style_rule_inserted(m_rules->native_rules().at(index), *sheet);
        sheet->invalidate_owners();
        sheet->synchronize_fonts_after_rule_change();
    }
    return index;
}

WebIDL::ExceptionOr<void> CSSGroupingRule::delete_rule(u32 index)
{
    auto removed_rule = TRY(m_rules->remove_a_css_rule(index));
    if (auto* sheet = parent_style_sheet()) {
        if (auto* function = Parser::ValueParserFFI::rust_rule_containing_function(native_rule().handle()))
            record_style_rule_declarations_changed(RustRule { function }, *sheet);
        else
            record_style_rule_removed(*sheet, removed_rule);
        sheet->invalidate_owners();
        sheet->synchronize_fonts_after_rule_change();
    }
    return {};
}

void CSSGroupingRule::set_parent_style_sheet(StyleSheetState* parent_style_sheet)
{
    CSSRule::set_parent_style_sheet(parent_style_sheet);
    m_rules->set_parent_style_sheet(parent_style_sheet);
}

}
