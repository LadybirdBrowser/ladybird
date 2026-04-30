/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StyleSheetList.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(StyleSheetList);

// https://www.w3.org/TR/cssom/#remove-a-css-style-sheet
void StyleSheetList::remove_a_css_style_sheet(CSS::CSSStyleSheet& sheet)
{
    // 1. Remove the CSS style sheet from the list of document or shadow root CSS style sheets.
    remove_sheet(sheet);

    // 2. Set the CSS style sheet’s parent CSS style sheet, owner node and owner CSS rule to null.
    sheet.set_parent_css_style_sheet(nullptr);
    sheet.set_owner_node(nullptr);
    sheet.set_owner_css_rule(nullptr);
}

// https://www.w3.org/TR/cssom/#add-a-css-style-sheet
void StyleSheetList::add_a_css_style_sheet(CSS::CSSStyleSheet& sheet)
{
    // 1. Add the CSS style sheet to the list of document or shadow root CSS style sheets at the appropriate location. The remainder of these steps deal with the disabled flag.
    add_sheet(sheet);

    // 2. If the disabled flag is set, then return.
    if (sheet.disabled())
        return;

    // 3. If the title is not the empty string, the alternate flag is unset, and preferred CSS style sheet set name is the empty string change the preferred CSS style sheet set name to the title.
    if (!sheet.title().is_empty() && !sheet.is_alternate() && m_preferred_css_style_sheet_set_name.is_empty()) {
        m_preferred_css_style_sheet_set_name = sheet.title();
    }

    // 4. If any of the following is true, then unset the disabled flag and return:
    //    - The title is the empty string.
    //    - The last CSS style sheet set name is null and the title is a case-sensitive match for the preferred CSS style sheet set name.
    //    - The title is a case-sensitive match for the last CSS style sheet set name.
    // NOTE: We don't enable alternate sheets with an empty title.  This isn't directly mentioned in the algorithm steps, but the
    // HTML specification says that the title element must be specified with a non-empty value for alternative style sheets.
    // See: https://html.spec.whatwg.org/multipage/links.html#the-link-is-an-alternative-stylesheet
    if ((sheet.title().is_empty() && !sheet.is_alternate())
        || (!m_last_css_style_sheet_set_name.has_value() && sheet.title().equals_ignoring_case(m_preferred_css_style_sheet_set_name))
        || (m_last_css_style_sheet_set_name.has_value() && sheet.title().equals_ignoring_case(m_last_css_style_sheet_set_name.value()))) {
        sheet.set_disabled(false);
        return;
    }

    // 5. Set the disabled flag.
    sheet.set_disabled(true);
}

// https://www.w3.org/TR/cssom/#create-a-css-style-sheet
GC::Ref<CSSStyleSheet> StyleSheetList::create_a_css_style_sheet(String const& css_text, String type, DOM::Element* owner_node, String media, String title, Alternate alternate, OriginClean origin_clean, Optional<::URL::URL> location, CSSStyleSheet* parent_style_sheet, CSSRule* owner_rule)
{
    // 1. Create a new CSS style sheet object and set its properties as specified.
    // AD-HOC: The spec never tells us when to parse this style sheet, but the most logical place is here.
    auto sheet = parse_css_stylesheet(Parser::ParsingParams { document() }, css_text, location);

    sheet->set_parent_css_style_sheet(parent_style_sheet);
    sheet->set_owner_css_rule(owner_rule);
    sheet->set_owner_node(owner_node);
    sheet->set_type(move(type));
    sheet->set_media(move(media));
    sheet->set_title(move(title));
    sheet->set_alternate(alternate == Alternate::Yes);
    sheet->set_origin_clean(origin_clean == OriginClean::Yes);

    // 2. Then run the add a CSS style sheet steps for the newly created CSS style sheet.
    add_a_css_style_sheet(*sheet);

    return sheet;
}

static StyleSheetInvalidationSet build_invalidation_set_for_stylesheet(CSSStyleSheet const& sheet)
{
    StyleSheetInvalidationSet result;

    sheet.for_each_effective_style_producing_rule([&](CSSRule const& rule) {
        if (result.invalidation_set.needs_invalidate_whole_subtree())
            return;
        if (auto const* style_rule = as_if<CSSStyleRule>(rule))
            extend_style_sheet_invalidation_set_with_style_rule(result, *style_rule);
    });

    return result;
}

static bool rule_requires_broad_add_or_remove_invalidation(CSSRule const& rule)
{
    switch (rule.type()) {
    case CSSRule::Type::Property:
    case CSSRule::Type::FontFace:
    case CSSRule::Type::FontFeatureValues:
    case CSSRule::Type::CounterStyle:
    case CSSRule::Type::Keyframes:
    case CSSRule::Type::LayerBlock:
    case CSSRule::Type::LayerStatement:
        return true;
    default:
        break;
    }

    return false;
}

static bool stylesheet_requires_broad_add_or_remove_invalidation(CSSStyleSheet const& sheet)
{
    bool requires_broad_invalidation = false;
    sheet.for_each_effective_rule(TraversalOrder::Preorder, [&](CSSRule const& rule) {
        if (requires_broad_invalidation)
            return;
        // Add/remove invalidation still has to look at effective non-style rules such as @keyframes or @layer, but
        // inactive top-level media sheets should contribute nothing at all here.
        requires_broad_invalidation = rule_requires_broad_add_or_remove_invalidation(rule);
    });
    return requires_broad_invalidation;
}

void StyleSheetList::add_sheet(CSSStyleSheet& sheet)
{
    sheet.add_owning_document_or_shadow_root(document_or_shadow_root());

    sheet.load_pending_image_resources(document());

    if (m_sheets.is_empty()) {
        // This is the first sheet, append it to the list.
        m_sheets.append(sheet);
    } else {
        // We have sheets from before. Insert the new sheet in the correct position (DOM tree order).
        bool did_insert = false;
        for (ssize_t i = m_sheets.size() - 1; i >= 0; --i) {
            auto& existing_sheet = *m_sheets[i];
            auto position = existing_sheet.owner_node()->compare_document_position(sheet.owner_node());
            if (position & DOM::Node::DocumentPosition::DOCUMENT_POSITION_FOLLOWING) {
                m_sheets.insert(i + 1, sheet);
                did_insert = true;
                break;
            }
        }
        if (!did_insert)
            m_sheets.prepend(sheet);
    }

    // NOTE: We evaluate media queries immediately when adding a new sheet.
    //       This coalesces the full document style invalidations.
    //       If we don't do this, we invalidate now, and then again when Document updates media rules.
    sheet.evaluate_media_queries(document());

    if (sheet.rules().length() == 0) {
        // NOTE: If the added sheet has no rules, we don't have to invalidate anything.
        return;
    }

    if (auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root())) {
        shadow_root->style_scope().invalidate_rule_cache();
    } else {
        document_or_shadow_root().document().style_scope().invalidate_rule_cache();
    }

    if (!document_or_shadow_root().is_shadow_root() && document_or_shadow_root().entire_subtree_needs_style_update()) {
        // NOTE: If the entire subtree is already marked for style update,
        //       there's no point spending time building invalidation sets.
        //       Shadow roots are special: :host(...) and ::slotted(...) can
        //       still require follow-up invalidation outside that subtree.
        return;
    }

    auto invalidation_set_result = build_invalidation_set_for_stylesheet(sheet);
    bool requires_broad_invalidation = invalidation_set_result.invalidation_set.needs_invalidate_whole_subtree()
        || stylesheet_requires_broad_add_or_remove_invalidation(sheet);
    if (requires_broad_invalidation) {
        if (auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root())) {
            auto effects = determine_shadow_root_stylesheet_effects(*shadow_root);
            invalidation_set_result.may_match_shadow_host |= effects.may_match_shadow_host;
            invalidation_set_result.may_match_light_dom_under_shadow_host |= effects.may_match_light_dom_under_shadow_host;
            invalidation_set_result.may_match_light_dom_outside_shadow_host |= effects.may_match_light_dom_outside_shadow_host;
        }
    }

    invalidate_root_for_style_sheet_change(document_or_shadow_root(), invalidation_set_result, DOM::StyleInvalidationReason::StyleSheetListAddSheet, requires_broad_invalidation);
}

void StyleSheetList::remove_sheet(CSSStyleSheet& sheet)
{
    auto invalidation_set_result = build_invalidation_set_for_stylesheet(sheet);
    bool requires_broad_invalidation = invalidation_set_result.invalidation_set.needs_invalidate_whole_subtree()
        || stylesheet_requires_broad_add_or_remove_invalidation(sheet);

    sheet.remove_owning_document_or_shadow_root(document_or_shadow_root());
    bool did_remove = m_sheets.remove_first_matching([&](auto& entry) { return entry.ptr() == &sheet; });
    VERIFY(did_remove);

    if (sheet.rules().length() == 0) {
        // NOTE: If the removed sheet had no rules, we don't have to invalidate anything.
        return;
    }

    if (auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root())) {
        shadow_root->style_scope().invalidate_rule_cache();
    } else {
        document_or_shadow_root().document().style_scope().invalidate_rule_cache();
    }

    if (!document_or_shadow_root().is_shadow_root() && document_or_shadow_root().entire_subtree_needs_style_update())
        return;

    if (requires_broad_invalidation) {
        if (auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root())) {
            auto effects = determine_shadow_root_stylesheet_effects(*shadow_root);
            invalidation_set_result.may_match_shadow_host |= effects.may_match_shadow_host;
            invalidation_set_result.may_match_light_dom_under_shadow_host |= effects.may_match_light_dom_under_shadow_host;
            invalidation_set_result.may_match_light_dom_outside_shadow_host |= effects.may_match_light_dom_outside_shadow_host;
        }
    }

    invalidate_root_for_style_sheet_change(document_or_shadow_root(), invalidation_set_result, DOM::StyleInvalidationReason::StyleSheetListRemoveSheet, requires_broad_invalidation);
}

GC::Ref<StyleSheetList> StyleSheetList::create(GC::Ref<DOM::Node> document_or_shadow_root)
{
    auto& realm = document_or_shadow_root->realm();
    return realm.create<StyleSheetList>(document_or_shadow_root);
}

StyleSheetList::StyleSheetList(GC::Ref<DOM::Node> document_or_shadow_root)
    : Bindings::PlatformObject(document_or_shadow_root->realm())
    , m_document_or_shadow_root(document_or_shadow_root)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .supports_indexed_properties = true };
}

void StyleSheetList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(StyleSheetList);
    Base::initialize(realm);
}

void StyleSheetList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document_or_shadow_root);
    visitor.visit(m_sheets);
}

Optional<JS::Value> StyleSheetList::item_value(size_t index) const
{
    if (index >= m_sheets.size())
        return {};

    return m_sheets[index].ptr();
}

DOM::Document& StyleSheetList::document()
{
    return m_document_or_shadow_root->document();
}

DOM::Document const& StyleSheetList::document() const
{
    return m_document_or_shadow_root->document();
}

}
