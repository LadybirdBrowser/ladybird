/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

using Parser::ValueParserFFI::NativeRuleList;

// NB: Implemented in Libraries/LibWeb/Bindings/Rust/src/css/exports.rs.
extern "C" {
void* web_css_rule_list_create(void* vm, NativeRuleList const* rules, void* document);
NativeRuleList const* web_css_rule_list_native_rules(void* list);
size_t web_css_rule_list_length(void* list);
void* web_css_rule_list_item(void* vm, void* list, size_t index);
void* web_css_rule_list_existing_rule(void* list, u64 identity);
void web_css_rule_list_set_parent_style_sheet(void* vm, void* list, void* sheet);
void web_css_rule_list_set_rules(void* vm, void* list, NativeRuleList const* rules, void* document);
void web_css_rule_list_remove_rule(void* vm, void* list, size_t index);
}

static void* vm_abi()
{
    return JS::HostABI::vm_to_abi(JS::VM::the());
}

static void* document_cell(GC::Ptr<DOM::Document> document)
{
    return document ? static_cast<GC::Cell*>(document.ptr()) : nullptr;
}

GC::Ref<CSSRuleList> CSSRuleList::create(RustRuleList const& rules, GC::Ptr<DOM::Document> document)
{
    return *static_cast<CSSRuleList*>(web_css_rule_list_create(vm_abi(), rules.handle(), document_cell(document)));
}

CSSRule* CSSRuleList::item(size_t index) const
{
    if (index >= length())
        return nullptr;
    return static_cast<CSSRule*>(web_css_rule_list_item(vm_abi(), const_cast<CSSRuleList*>(this), index));
}

size_t CSSRuleList::length() const
{
    return web_css_rule_list_length(const_cast<CSSRuleList*>(this));
}

RustRuleList CSSRuleList::native_rules() const
{
    return RustRuleList { Parser::ValueParserFFI::rust_rule_list_retain(web_css_rule_list_native_rules(const_cast<CSSRuleList*>(this))) };
}

CSSRule* CSSRuleList::existing_wrapper(u64 identity) const
{
    return static_cast<CSSRule*>(web_css_rule_list_existing_rule(const_cast<CSSRuleList*>(this), identity));
}

void CSSRuleList::set_parent_style_sheet(StyleSheetState* sheet)
{
    web_css_rule_list_set_parent_style_sheet(vm_abi(), this, sheet ? &sheet->cssom_sheet() : nullptr);
}

CSSRule* CSSRuleList::rule_for_identity(u64 identity) const
{
    auto path = native_rules().path_to_rule(identity);
    auto const* list = this;
    CSSRule* rule = nullptr;
    for (size_t depth = 0; depth < path.size(); ++depth) {
        rule = list->item(path[depth]);
        VERIFY(rule);
        if (depth + 1 < path.size())
            list = &as<CSSGroupingRule>(*rule).css_rules();
    }
    return rule;
}

void CSSRuleList::set_rules(Badge<StyleSheetState>, RustRuleList const& rules, GC::Ptr<DOM::Document> document)
{
    web_css_rule_list_set_rules(vm_abi(), this, rules.handle(), document_cell(document));
}

// https://drafts.csswg.org/cssom/#insert-a-css-rule
WebIDL::ExceptionOr<unsigned> CSSRuleList::insert_a_css_rule(RustRuleList& rules, RustRule const& rule, u32 index, Nested nested)
{
    using Result = Parser::ValueParserFFI::RuleInsertionResult;
    switch (Parser::ValueParserFFI::rust_rule_list_insert_css_rule(rules.handle(), index, rule.handle(), nested == Nested::Yes)) {
    case Result::Success:
        return index;
    case Result::IndexSizeError:
        return WebIDL::IndexSizeError::create("CSS rule index out of bounds."_utf16);
    case Result::HierarchyRequestError:
        return WebIDL::HierarchyRequestError::create("Cannot insert rule at specified index."_utf16);
    case Result::InvalidStateError:
        return WebIDL::InvalidStateError::create("Cannot insert @namespace rule into a stylesheet with non-namespace/import rules"_utf16);
    }
    VERIFY_NOT_REACHED();
}

// https://www.w3.org/TR/cssom/#remove-a-css-rule
WebIDL::ExceptionOr<void> CSSRuleList::validate_rule_removal(RustRuleList const& rules, u32 index)
{
    using Result = Parser::ValueParserFFI::RuleRemovalResult;
    switch (Parser::ValueParserFFI::rust_rule_list_validate_removal(rules.handle(), index)) {
    case Result::Success:
        return {};
    case Result::IndexSizeError:
        return WebIDL::IndexSizeError::create("CSS rule index out of bounds."_utf16);
    case Result::InvalidStateError:
        return WebIDL::InvalidStateError::create("Cannot remove @namespace rule from a stylesheet with non-namespace/import rules."_utf16);
    }
    VERIFY_NOT_REACHED();
}

void CSSRuleList::remove_a_css_rule_without_validation(Badge<StyleSheetState>, u32 index)
{
    web_css_rule_list_remove_rule(vm_abi(), this, index);
}

}
