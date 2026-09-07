/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/CSSFontFeatureValuesRule.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

CSSRuleList::~CSSRuleList() = default;

GC_DEFINE_ALLOCATOR(CSSRuleList);

GC::Ref<CSSRuleList> CSSRuleList::create(ReadonlySpan<GC::Ref<CSSRule>> rules)
{
    auto rule_list = GC::Heap::the().allocate<CSSRuleList>();
    for (auto rule : rules)
        rule_list->insert(rule_list->length(), rule);
    return rule_list;
}

CSSRuleList::CSSRuleList()
{
}

GC::Ref<CSSRuleList> CSSRuleList::create(RustRuleList rules, GC::Ptr<DOM::Document> document)
{
    return GC::Heap::the().allocate<CSSRuleList>(move(rules), document);
}

CSSRuleList::CSSRuleList(RustRuleList rules, GC::Ptr<DOM::Document> document)
    : m_rules(move(rules))
    , m_document(document)
{
}

CSSRule* CSSRuleList::existing_wrapper(u64 identity) const
{
    auto iterator = m_wrappers.find(identity);
    return iterator == m_wrappers.end() ? nullptr : iterator->value.ptr();
}

GC::Ref<CSSRule> const& CSSRuleList::wrapper_at(size_t index) const
{
    auto identity = m_rules.identity_at(index);
    if (auto iterator = m_wrappers.find(identity); iterator != m_wrappers.end())
        return iterator->value;

    auto rule = [&]() -> GC::Ref<CSSRule> {
        if (m_parent_style_sheet) {
            if (auto* import = m_parent_style_sheet->import_for_rule(identity))
                return import->cssom_rule();
        }
        return CSSRule::create(m_rules.at(index), m_document);
    }();
    m_wrappers.set(identity, rule);
    if (m_owner_rule)
        rule->set_parent_rule(m_owner_rule.ptr());
    else
        rule->set_parent_style_sheet(m_parent_style_sheet.ptr());
    return m_wrappers.find(identity)->value;
}

void CSSRuleList::for_each_existing_rule(Function<void(CSSRule&)> const& callback) const
{
    for (auto const& entry : m_wrappers)
        callback(*entry.value);
}

void CSSRuleList::set_owner_rule(GC::Ref<CSSRule> owner)
{
    m_owner_rule = owner;
    for_each_existing_rule([&](CSSRule& rule) { rule.set_parent_rule(owner.ptr()); });
}

void CSSRuleList::set_parent_style_sheet(StyleSheetState* sheet)
{
    m_parent_style_sheet = sheet;
    m_parent_cssom_sheet = sheet ? &sheet->cssom_sheet() : nullptr;
    for_each_existing_rule([&](CSSRule& rule) { rule.set_parent_style_sheet(sheet); });
}

void CSSRuleList::insert(size_t index, GC::Ref<CSSRule> rule)
{
    m_rules.insert(index, rule->native_rule());
    m_wrappers.set(rule->native_rule().identity(), rule);
}

CSSRule const* CSSRuleList::rule_for_identity(u64 identity) const
{
    auto path = m_rules.path_to_rule(identity);
    auto const* list = this;
    CSSRule const* rule = nullptr;
    for (size_t depth = 0; depth < path.size(); ++depth) {
        rule = list->item(path[depth]);
        VERIFY(rule);
        if (depth + 1 < path.size())
            list = &as<CSSGroupingRule>(*rule).css_rules();
    }
    return rule;
}

void CSSRuleList::set_rules(Badge<StyleSheetState>, RustRuleList rules, GC::Ptr<DOM::Document> document)
{
    for_each_existing_rule([](CSSRule& rule) { rule.set_parent_style_sheet(nullptr); });

    m_rules.replace(rules);
    m_wrappers.clear();
    m_document = document;
}

void CSSRuleList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_wrappers);
    visitor.visit(m_owner_rule);
    visitor.visit(m_parent_style_sheet);
    visitor.visit(m_parent_cssom_sheet);
    visitor.visit(m_document);
}

size_t CSSRuleList::external_memory_size() const
{
    return JS::saturating_add_external_memory_size(Base::external_memory_size(), JS::saturating_add_external_memory_size(m_rules.external_memory_size(), JS::hash_map_external_memory_size(m_wrappers)));
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

// AD-HOC: The spec doesn't include a declared_namespaces parameter, but we need it to handle parsing of namespaced selectors.
// https://drafts.csswg.org/cssom/#insert-a-css-rule
WebIDL::ExceptionOr<unsigned> CSSRuleList::insert_a_css_rule(Utf16View rule, u32 index, Nested nested, RustNamespaceContext const& declared_namespaces)
{
    // 1. Set length to the number of items in list.
    auto length = m_rules.size();

    // 2. If index is greater than length, then throw an IndexSizeError exception.
    if (index > length)
        return WebIDL::IndexSizeError::create("CSS rule index out of bounds."_utf16);

    // 3. Set new rule to the results of performing parse a CSS rule on argument rule.
    Parser::ParsingParams parsing_params {};
    parsing_params.rule_context = rule_context();
    parsing_params.declared_namespaces = declared_namespaces;
    auto new_rule = parse_css_rule(parsing_params, rule, nested == Nested::Yes);
    if (!new_rule.has_value() && nested == Nested::Yes) {
        auto trimmed_rule = rule.trim(" \t\n\f\r"sv, TrimMode::Left);
        if (trimmed_rule.starts_with("@import"sv) || trimmed_rule.starts_with("@namespace"sv)) {
            auto top_level_params = parsing_params;
            top_level_params.rule_context.clear();
            new_rule = parse_css_rule(top_level_params, rule);
        }
    }

    // 4. If new rule is a syntax error, and nested is set, perform the following substeps:
    if (!new_rule.has_value() && nested == Nested::Yes) {
        // - Set declarations to the results of performing parse a CSS declaration block, on argument rule.
        auto declarations = parse_css_property_declaration_block(parsing_params, rule);

        // - If declarations is empty, throw a SyntaxError exception.
        if (declarations.is_empty())
            return WebIDL::SyntaxError::create("Unable to parse CSS declarations block."_utf16);

        // - Otherwise, set new rule to a new nested declarations rule with declarations as it contents.
        new_rule = RustRule { declarations };
    }

    // 5. If new rule is a syntax error, throw a SyntaxError exception.
    if (!new_rule.has_value())
        return WebIDL::SyntaxError::create("Unable to parse CSS rule."_utf16);

    auto result = TRY(insert_a_css_rule(m_rules, *new_rule, index, nested));
    if (on_change)
        on_change();
    return result;
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

WebIDL::ExceptionOr<RustRule> CSSRuleList::remove_a_css_rule(u32 index)
{
    TRY(validate_rule_removal(m_rules, index));
    auto old_rule = m_rules.at(index);
    remove_a_css_rule_without_validation(index);
    return old_rule;
}

void CSSRuleList::remove_a_css_rule_without_validation(Badge<StyleSheetState>, u32 index)
{
    remove_a_css_rule_without_validation(index);
}

void CSSRuleList::remove_a_css_rule_without_validation(u32 index)
{
    VERIFY(index < m_rules.size());

    auto old_native_rule = m_rules.at(index);
    auto old_rule = GC::make_root(existing_wrapper(old_native_rule.identity()));

    // https://drafts.csswg.org/css-font-loading/#font-face-css-connection
    // If a @font-face rule is removed from the document, its corresponding FontFace object is no longer CSS-connected.
    // The connection is not restorable by any means (but adding the @font-face back to the stylesheet will create a
    // brand new FontFace object which is CSS-connected).
    if (m_parent_style_sheet)
        m_parent_style_sheet->disconnect_font_faces_in_rule(old_native_rule);

    // 5. Remove rule old rule from list at the zero-indexed position index.
    m_rules.remove(index);
    m_wrappers.remove(old_native_rule.identity());

    // 6. Set old rule’s parent CSS rule and parent CSS style sheet to null.
    // NOTE: We set the parent stylesheet to null within set_parent_rule.
    if (old_rule)
        old_rule->set_parent_rule(nullptr);

    if (on_change)
        on_change();
}

Vector<Parser::RuleContext> CSSRuleList::rule_context() const
{
    Vector<Parser::RuleContext> context;
    for (auto* rule = m_owner_rule.ptr(); rule; rule = rule->parent_rule())
        context.append(Parser::rule_context_type_for_rule(rule->type()));
    context.reverse();
    return context;
}

}
