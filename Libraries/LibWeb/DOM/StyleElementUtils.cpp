/*
 * Copyright (c) 2023, Preston Taylor <PrestonLeeTaylor@proton.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StyleElementUtils.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::DOM {

// The user agent must run the "update a style block" algorithm whenever one of the following conditions occur:
// FIXME: The element is popped off the stack of open elements of an HTML parser or XML parser.
//
// NOTE: This is basically done by children_changed() today:
// The element's children changed steps run.
//
// NOTE: This is basically done by inserted() and removed_from() today:
// The element is not on the stack of open elements of an HTML parser or XML parser, and it becomes connected or disconnected.
//
// https://html.spec.whatwg.org/multipage/semantics.html#update-a-style-block
void StyleElementUtils::update_a_style_block(DOM::Element& style_element)
{
    // OPTIMIZATION: Skip parsing CSS if we're in the middle of parsing a HTML fragment.
    //               The style block will be parsed upon insertion into a proper document.
    if (style_element.document().is_temporary_document_for_fragment_parsing())
        return;

    // 1. Let element be the style element.
    // 2. If element has an associated CSS style sheet, remove the CSS style sheet in question.

    if (m_associated_css_style_sheet) {
        m_style_sheet_list->remove_a_css_style_sheet(*m_associated_css_style_sheet);
        m_style_sheet_list = nullptr;

        // FIXME: This should probably be handled by StyleSheet::set_owner_node().
        m_associated_css_style_sheet = nullptr;
    }

    // 3. If element is not connected, then return.
    if (!style_element.is_connected())
        return;

    // 4. If element's type attribute is present and its value is neither the empty string nor an ASCII case-insensitive match for "text/css", then return.
    auto type_attribute = style_element.attribute(HTML::AttributeNames::type);
    if (type_attribute.has_value() && !type_attribute->is_empty() && !Infra::is_ascii_case_insensitive_match(type_attribute->bytes_as_string_view(), "text/css"sv))
        return;

    // FIXME: 5. If the Should element's inline behavior be blocked by Content Security Policy? algorithm returns "Blocked" when executed upon the style element, "style", and the style element's child text content, then return. [CSP]

    // 6. Create a CSS style sheet with the following properties:
    //        type
    //            text/css
    //        owner node
    //            element
    //        media
    //            The media attribute of element.
    //        title
    //            The title attribute of element, if element is in a document tree, or the empty string otherwise.
    //        alternate flag
    //            Unset.
    //        origin-clean flag
    //            Set.
    //        location
    //        parent CSS style sheet
    //        owner CSS rule
    //            null
    //        disabled flag
    //            Left at its default value.
    //        CSS rules
    //          Left uninitialized.
    m_style_sheet_list = style_element.document_or_shadow_root_style_sheets();
    m_associated_css_style_sheet = m_style_sheet_list->create_a_css_style_sheet(
        style_element.text_content().value_or(String {}),
        "text/css"_string,
        &style_element,
        style_element.attribute(HTML::AttributeNames::media).value_or({}),
        style_element.in_a_document_tree()
            ? style_element.attribute(HTML::AttributeNames::title).value_or({})
            : String {},
        CSS::StyleSheetList::Alternate::No,
        CSS::StyleSheetList::OriginClean::Yes,
        // AD-HOC: Use the document's base URL as the location instead. Spec issue: https://github.com/whatwg/html/issues/11281
        style_element.document().base_url(),
        nullptr,
        nullptr);

    // 7. If element contributes a script-blocking style sheet, append element to its node document's script-blocking style sheet set.
    if (style_element.contributes_a_script_blocking_style_sheet())
        style_element.document().script_blocking_style_sheet_set().set(style_element);

    // FIXME: 8. If element's media attribute's value matches the environment and element is potentially render-blocking, then block rendering on element.
}

void StyleElementUtils::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_associated_css_style_sheet);
    visitor.visit(m_style_sheet_list);
}

}
