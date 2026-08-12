/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/DOM/AdoptedStyleSheets.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(StyleSheetList);

// https://www.w3.org/TR/cssom/#remove-a-css-style-sheet
void StyleSheetList::remove_a_css_style_sheet(CSS::CSSStyleSheet& sheet, StyleEngineUpdate style_engine_update)
{
    // 1. Remove the CSS style sheet from the list of document or shadow root CSS style sheets.
    remove_sheet(sheet, style_engine_update);

    // 2. Set the CSS style sheet’s parent CSS style sheet, owner node and owner CSS rule to null.
    sheet.set_parent_css_style_sheet(nullptr);
    sheet.set_owner_node(nullptr);
    sheet.set_owner_css_rule(nullptr);
}

// https://www.w3.org/TR/cssom/#add-a-css-style-sheet
void StyleSheetList::add_a_css_style_sheet(CSS::CSSStyleSheet& sheet, StyleEngineUpdate style_engine_update)
{
    // 1. Add the CSS style sheet to the list of document or shadow root CSS style sheets at the appropriate location. The remainder of these steps deal with the disabled flag.
    add_sheet(sheet, style_engine_update);

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
        || (!m_last_css_style_sheet_set_name.has_value() && sheet.title().equals_ignoring_ascii_case(m_preferred_css_style_sheet_set_name))
        || (m_last_css_style_sheet_set_name.has_value() && sheet.title().equals_ignoring_ascii_case(m_last_css_style_sheet_set_name.value()))) {
        sheet.set_disabled(false);
        return;
    }

    // 5. Set the disabled flag.
    sheet.set_disabled(true);
}

// https://www.w3.org/TR/cssom/#create-a-css-style-sheet
GC::Ref<CSSStyleSheet> StyleSheetList::create_a_css_style_sheet(Utf16View css_text, DOM::Element* owner_node, Utf16View media, Utf16String title, Alternate alternate, OriginClean origin_clean, Optional<::URL::URL> location, CSSStyleSheet* parent_style_sheet, CSSRule* owner_rule, StyleEngineUpdate style_engine_update)
{
    // 1. Create a new CSS style sheet object and set its properties as specified.
    // AD-HOC: The spec never tells us when to parse this style sheet, but the most logical place is here.
    auto sheet = parse_css_stylesheet(Parser::ParsingParams { document() }, css_text, location);

    sheet->set_parent_css_style_sheet(parent_style_sheet);
    sheet->set_owner_css_rule(owner_rule);
    sheet->set_owner_node(owner_node);
    sheet->set_media(move(media));
    sheet->set_title(move(title));
    sheet->set_alternate(alternate == Alternate::Yes);
    sheet->set_origin_clean(origin_clean == OriginClean::Yes);

    // 2. Then run the add a CSS style sheet steps for the newly created CSS style sheet.
    add_a_css_style_sheet(*sheet, style_engine_update);

    return sheet;
}

void StyleSheetList::add_sheet(CSSStyleSheet& sheet, StyleEngineUpdate style_engine_update)
{
    sheet.add_owning_document_or_shadow_root(document_or_shadow_root());

    sheet.load_pending_image_resources(document());

    insert_sheet_in_tree_order(sheet);

    if (style_engine_update == StyleEngineUpdate::Record)
        record_stylesheet_attached(sheet, document_or_shadow_root(), following_sheet(sheet));

    // NOTE: We evaluate media queries immediately when adding a new sheet.
    //       This coalesces the full document style invalidations.
    //       If we don't do this, we invalidate now, and then again when Document updates media rules.
    sheet.evaluate_media_queries(document());
    // A sheet whose media does not match contributes nothing, so its rules can reach no element.
    if (style_engine_update == StyleEngineUpdate::Record)
        record_stylesheet_conditions(sheet, document_or_shadow_root(), !sheet.disabled() && sheet.media()->matches());

    invalidate_rule_cache_after_style_sheet_change(document_or_shadow_root(), sheet);
}

void StyleSheetList::insert_sheet_in_tree_order(CSSStyleSheet& sheet)
{
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
}

CSSStyleSheet* StyleSheetList::following_sheet(CSSStyleSheet& sheet)
{
    // The list is kept in DOM tree order, so the sheet that now follows this one is its successor
    // in cascade order too.
    auto position = m_sheets.find_first_index(sheet);
    CSSStyleSheet* following = nullptr;
    if (position.has_value() && *position + 1 < m_sheets.size())
        following = m_sheets[*position + 1].ptr();

    // https://drafts.csswg.org/cssom/#documentorshadowroot-final-css-style-sheets
    // Adopted sheets cascade after every sheet in the list, whenever either arrived, so the last
    // sheet of the list is followed by the first adopted one rather than by nothing.
    if (!following) {
        auto& scope = document_or_shadow_root();
        auto adopted = is<DOM::Document>(scope)
            ? DOM::AdoptedStyleSheetsAccess::adopted_style_sheets(static_cast<DOM::Document&>(scope))
            : DOM::AdoptedStyleSheetsAccess::adopted_style_sheets(static_cast<DOM::ShadowRoot&>(scope));
        DOM::for_each_adopted_style_sheet(adopted, [&](CSSStyleSheet& adopted_sheet) {
            if (!following)
                following = &adopted_sheet;
        });
    }
    return following;
}

void StyleSheetList::remove_sheet(CSSStyleSheet& sheet, StyleEngineUpdate style_engine_update)
{
    sheet.remove_owning_document_or_shadow_root(document_or_shadow_root());
    bool did_remove = m_sheets.remove_first_matching([&](auto& entry) { return entry.ptr() == &sheet; });
    VERIFY(did_remove);
    if (style_engine_update == StyleEngineUpdate::Record)
        record_stylesheet_detached(sheet, document_or_shadow_root());

    invalidate_rule_cache_after_style_sheet_change(document_or_shadow_root(), sheet);
}

void StyleSheetList::move_sheet(CSSStyleSheet& sheet, StyleSheetList& destination)
{
    if (this != &destination) {
        remove_sheet(sheet, StyleEngineUpdate::Record);
        destination.add_sheet(sheet, StyleEngineUpdate::Record);
        return;
    }

    auto position = m_sheets.find_first_index(sheet);
    VERIFY(position.has_value());
    m_sheets.remove(*position);
    insert_sheet_in_tree_order(sheet);
    record_stylesheet_attached(sheet, document_or_shadow_root(), following_sheet(sheet));
    invalidate_rule_cache_after_style_sheet_change(document_or_shadow_root(), sheet);
}

GC::Ref<StyleSheetList> StyleSheetList::create(GC::Ref<DOM::Node> document_or_shadow_root)
{
    return GC::Heap::the().allocate<StyleSheetList>(document_or_shadow_root);
}

StyleSheetList::StyleSheetList(GC::Ref<DOM::Node> document_or_shadow_root)
    : m_document_or_shadow_root(document_or_shadow_root)
{
}

void StyleSheetList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document_or_shadow_root);
    visitor.visit(m_sheets);
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
