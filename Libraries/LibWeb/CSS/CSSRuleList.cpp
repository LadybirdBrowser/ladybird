/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibWeb/Bindings/CSSRuleListPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSSupportsRule.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSRuleList);

GC::Ref<CSSRuleList> CSSRuleList::create(JS::Realm& realm, ReadonlySpan<GC::Ref<CSSRule>> rules)
{
    auto rule_list = realm.create<CSSRuleList>(realm);
    for (auto rule : rules)
        rule_list->m_rules.append(rule);
    return rule_list;
}

CSSRuleList::CSSRuleList(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .supports_indexed_properties = 1 };
}

void CSSRuleList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSRuleList);
    Base::initialize(realm);
}

void CSSRuleList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rules);
    visitor.visit(m_owner_rule);
}

// AD-HOC: The spec doesn't include a declared_namespaces parameter, but we need it to handle parsing of namespaced selectors.
// https://drafts.csswg.org/cssom/#insert-a-css-rule
WebIDL::ExceptionOr<unsigned> CSSRuleList::insert_a_css_rule(Variant<StringView, CSSRule*> rule, u32 index, Nested nested, HashTable<FlyString> const& declared_namespaces)
{
    // 1. Set length to the number of items in list.
    auto length = m_rules.size();

    // 2. If index is greater than length, then throw an IndexSizeError exception.
    if (index > length)
        return WebIDL::IndexSizeError::create(realm(), "CSS rule index out of bounds."_utf16);

    // 3. Set new rule to the results of performing parse a CSS rule on argument rule.
    // NOTE: The insert-a-css-rule spec expects `rule` to be a string, but the CSSStyleSheet.insertRule()
    //       spec calls this algorithm with an already-parsed CSSRule. So, we use a Variant and skip step 3
    //       if that variant holds a CSSRule already.

    CSSRule* new_rule = nullptr;
    if (rule.has<StringView>()) {
        Parser::ParsingParams parsing_params { realm() };
        parsing_params.declared_namespaces = declared_namespaces;

        new_rule = parse_css_rule(parsing_params, rule.get<StringView>());
    } else {
        new_rule = rule.get<CSSRule*>();
    }

    // 4. If new rule is a syntax error, and nested is set, perform the following substeps:
    if (!new_rule && nested == Nested::Yes) {
        Parser::ParsingParams parsing_params { realm() };
        parsing_params.rule_context = rule_context();
        parsing_params.declared_namespaces = declared_namespaces;

        // - Set declarations to the results of performing parse a CSS declaration block, on argument rule.
        auto declarations = parse_css_property_declaration_block(parsing_params, rule.get<StringView>());

        // - If declarations is empty, throw a SyntaxError exception.
        if (declarations.custom_properties.is_empty() && declarations.properties.is_empty())
            return WebIDL::SyntaxError::create(realm(), "Unable to parse CSS declarations block."_utf16);

        // - Otherwise, set new rule to a new nested declarations rule with declarations as it contents.
        new_rule = CSSNestedDeclarations::create(realm(), CSSStyleProperties::create(realm(), move(declarations.properties), move(declarations.custom_properties)));
    }

    // 5. If new rule is a syntax error, throw a SyntaxError exception.
    if (!new_rule)
        return WebIDL::SyntaxError::create(realm(), "Unable to parse CSS rule."_utf16);

    auto has_rule_of_type_other_than_specified_before_index = [&](Vector<CSSRule::Type> types, size_t index) {
        for (size_t i = 0; i < index; i++) {
            if (!any_of(types, [&](auto type) { return m_rules[i]->type() == type; }))
                return true;
        }
        return false;
    };

    auto has_rule_of_type_at_or_after_index = [&](CSSRule::Type type, size_t index) {
        for (size_t i = index; i < m_rules.size(); i++) {
            if (m_rules[i]->type() == type)
                return true;
        }
        return false;
    };

    // 6. If new rule cannot be inserted into list at the zero-indexed position index due to constraints specified by CSS, then throw a HierarchyRequestError exception. [CSS21]
    // "Any @import rules must precede all other valid at-rules and style rules in a style sheet
    // (ignoring @charset and @layer statement rules) and must not have any other valid at-rules
    // or style rules between it and previous @import rules, or else the @import rule is invalid."
    // https://drafts.csswg.org/css-cascade-5/#at-import
    //
    // "Any @namespace rules must follow all @charset and @import rules and precede all other
    // non-ignored at-rules and style rules in a style sheet.

    auto rule_is_disallowed = false;
    switch (new_rule->type()) {
    case CSSRule::Type::LayerStatement:
        break;
    case CSSRule::Type::Import:
        rule_is_disallowed = has_rule_of_type_other_than_specified_before_index({ CSSRule::Type::Import, CSSRule::Type::LayerStatement }, index);
        break;
    case CSSRule::Type::Namespace:
        rule_is_disallowed = has_rule_of_type_at_or_after_index(CSSRule::Type::Import, index) || has_rule_of_type_other_than_specified_before_index({ CSSRule::Type::Import, CSSRule::Type::Namespace, CSSRule::Type::LayerStatement }, index);
        break;
    default:
        rule_is_disallowed = has_rule_of_type_at_or_after_index(CSSRule::Type::Import, index) || has_rule_of_type_at_or_after_index(CSSRule::Type::Namespace, index);
        break;
    }

    // FIXME: There are more constraints that we should check here - Parser::is_valid_in_the_current_context is probably a good reference for that.
    if (rule_is_disallowed || (nested == Nested::Yes && first_is_one_of(new_rule->type(), CSSRule::Type::Import, CSSRule::Type::Namespace)))
        return WebIDL::HierarchyRequestError::create(realm(), "Cannot insert rule at specified index."_utf16);

    // 7. If new rule is an @namespace at-rule, and list contains anything other than @import at-rules, and @namespace at-rules, throw an InvalidStateError exception.
    if (new_rule->type() == CSSRule::Type::Namespace && any_of(m_rules, [](auto existing_rule) { return existing_rule->type() != CSSRule::Type::Import && existing_rule->type() != CSSRule::Type::Namespace; }))
        return WebIDL::InvalidStateError::create(realm(), "Cannot insert @namespace rule into a stylesheet with non-namespace/import rules"_utf16);

    // 8. Insert new rule into list at the zero-indexed position index.
    m_rules.insert(index, *new_rule);

    // 9. Return index.
    if (on_change)
        on_change();
    return index;
}

// https://www.w3.org/TR/cssom/#remove-a-css-rule
WebIDL::ExceptionOr<void> CSSRuleList::remove_a_css_rule(u32 index)
{
    // 1. Set length to the number of items in list.
    auto length = m_rules.size();

    // 2. If index is greater than or equal to length, then throw an IndexSizeError exception.
    if (index >= length)
        return WebIDL::IndexSizeError::create(realm(), "CSS rule index out of bounds."_utf16);

    // 3. Set old rule to the indexth item in list.
    CSSRule& old_rule = m_rules[index];

    // 4. If old rule is an @namespace at-rule, and list contains anything other than @import at-rules, and @namespace at-rules, throw an InvalidStateError exception.
    if (old_rule.type() == CSSRule::Type::Namespace) {
        for (auto& rule : m_rules) {
            if (rule->type() != CSSRule::Type::Import && rule->type() != CSSRule::Type::Namespace)
                return WebIDL::InvalidStateError::create(realm(), "Cannot remove @namespace rule from a stylesheet with non-namespace/import rules."_utf16);
        }
    }

    // https://drafts.csswg.org/css-font-loading/#font-face-css-connection
    // If a @font-face rule is removed from the document, its connected FontFace object is no longer CSS-connected.
    if (auto* font_face_rule = as_if<CSSFontFaceRule>(old_rule))
        font_face_rule->disconnect_font_face();

    // 5. Remove rule old rule from list at the zero-indexed position index.
    m_rules.remove(index);

    // 6. Set old rule’s parent CSS rule and parent CSS style sheet to null.
    // NOTE: We set the parent stylesheet to null within set_parent_rule.
    old_rule.set_parent_rule(nullptr);

    if (on_change)
        on_change();
    return {};
}

void CSSRuleList::for_each_effective_rule(TraversalOrder order, Function<void(Web::CSS::CSSRule const&)> const& callback) const
{
    for (auto const& rule : m_rules) {
        if (order == TraversalOrder::Preorder)
            callback(rule);

        switch (rule->type()) {
        case CSSRule::Type::Import: {
            auto const& import_rule = static_cast<CSSImportRule const&>(*rule);
            if (import_rule.loaded_style_sheet())
                import_rule.loaded_style_sheet()->for_each_effective_rule(order, callback);
            break;
        }

        case CSSRule::Type::LayerBlock:
        case CSSRule::Type::Media:
        case CSSRule::Type::Page:
        case CSSRule::Type::Style:
        case CSSRule::Type::Supports:
            static_cast<CSSGroupingRule const&>(*rule).for_each_effective_rule(order, callback);
            break;

        case CSSRule::Type::CounterStyle:
        case CSSRule::Type::FontFace:
        case CSSRule::Type::Keyframe:
        case CSSRule::Type::Keyframes:
        case CSSRule::Type::LayerStatement:
        case CSSRule::Type::Margin:
        case CSSRule::Type::Namespace:
        case CSSRule::Type::NestedDeclarations:
        case CSSRule::Type::Property:
            break;
        }

        if (order == TraversalOrder::Postorder)
            callback(rule);
    }
}

bool CSSRuleList::evaluate_media_queries(DOM::Document const& document)
{
    bool any_media_queries_changed_match_state = false;

    for (auto& rule : m_rules) {
        switch (rule->type()) {
        case CSSRule::Type::Import: {
            auto& import_rule = as<CSSImportRule>(*rule);
            if (import_rule.loaded_style_sheet() && import_rule.loaded_style_sheet()->evaluate_media_queries(document))
                any_media_queries_changed_match_state = true;
            break;
        }
        case CSSRule::Type::LayerBlock: {
            auto& layer_rule = as<CSSLayerBlockRule>(*rule);
            if (layer_rule.css_rules().evaluate_media_queries(document))
                any_media_queries_changed_match_state = true;
            break;
        }
        case CSSRule::Type::Media: {
            auto& media_rule = as<CSSMediaRule>(*rule);
            bool did_match = media_rule.condition_matches();
            bool now_matches = media_rule.evaluate(document);
            if (did_match != now_matches)
                any_media_queries_changed_match_state = true;
            if (now_matches && media_rule.css_rules().evaluate_media_queries(document))
                any_media_queries_changed_match_state = true;
            break;
        }
        case CSSRule::Type::Supports: {
            auto& supports_rule = as<CSSSupportsRule>(*rule);
            if (supports_rule.condition_matches() && supports_rule.css_rules().evaluate_media_queries(document))
                any_media_queries_changed_match_state = true;
            break;
        }
        case CSSRule::Type::Style: {
            auto& style_rule = as<CSSStyleRule>(*rule);
            if (style_rule.css_rules().evaluate_media_queries(document))
                any_media_queries_changed_match_state = true;
            break;
        }
        case CSSRule::Type::CounterStyle:
        case CSSRule::Type::FontFace:
        case CSSRule::Type::Keyframe:
        case CSSRule::Type::Keyframes:
        case CSSRule::Type::LayerStatement:
        case CSSRule::Type::Margin:
        case CSSRule::Type::Namespace:
        case CSSRule::Type::NestedDeclarations:
        case CSSRule::Type::Property:
        case CSSRule::Type::Page:
            break;
        }
    }

    return any_media_queries_changed_match_state;
}

Optional<JS::Value> CSSRuleList::item_value(size_t index) const
{
    if (auto value = item(index))
        return value;
    return {};
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
