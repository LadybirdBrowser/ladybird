/*
 * Copyright (c) 2023, Preston Taylor <PrestonLeeTaylor@proton.me>
 * Copyright (c) 2025, Lorenz Ackermann <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
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
    if (type_attribute.has_value() && !type_attribute->is_empty() && !type_attribute->bytes_as_string_view().equals_ignoring_ascii_case("text/css"sv))
        return;

    // 5. If the Should element's inline behavior be blocked by Content Security Policy? algorithm returns "Blocked" when executed upon the style element, "style", and the style element's child text content, then return. [CSP]
    if (ContentSecurityPolicy::should_elements_inline_type_behavior_be_blocked_by_content_security_policy(style_element.realm(), style_element, ContentSecurityPolicy::Directives::Directive::InlineType::Style, style_element.child_text_content().to_utf8_but_should_be_ported_to_utf16()) == ContentSecurityPolicy::Directives::Directive::Result::Blocked)
        return;

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
        style_element.text_content().value_or({}).to_utf8_but_should_be_ported_to_utf16(),
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

    // https://html.spec.whatwg.org/multipage/semantics.html#the-style-element:critical-subresources
    auto attempts_to_fetch_subresources_finished = [](GC::Ptr<CSS::CSSStyleSheet> style_sheet) {
        // 1. Let element be the style element associated with the style sheet in question.
        auto& element = *style_sheet->owner_node();
        // 2. Let success be true.
        auto success = true;
        // FIXME: 3. If the attempts to obtain any of the style sheet's critical subresources failed for any reason
        // (e.g., DNS error, HTTP 404 response, a connection being prematurely closed, unsupported Content-Type), set success to false.
        // Note: that content-specific errors, e.g., CSS parse errors or PNG decoding errors, do not affect success.
        // 4. Queue an element task on the networking task source given element and the following steps:
        element.queue_an_element_task(HTML::Task::Source::UserInteraction, [&element, success] {
            // 1. If success is true, fire an event named load at element.
            // AD-HOC: these should call fire an event this is not implemented anywhere so we dispatch ourself
            if (success)
                element.dispatch_event(DOM::Event::create(element.realm(), HTML::EventNames::load));
            // 2. Otherwise, fire an event named error at element.
            else
                element.dispatch_event(DOM::Event::create(element.realm(), HTML::EventNames::error));
            // 3. If element contributes a script-blocking style sheet:
            if (element.contributes_a_script_blocking_style_sheet()) {
                // 1. Assert: element's node document's script-blocking style sheet set contains element.
                VERIFY(element.document().script_blocking_style_sheet_set().contains(element));
                // 2. Remove element from its node document's script-blocking style sheet set.
                element.document().script_blocking_style_sheet_set().remove(element);
            }
            // 4. Unblock rendering on element.
            element.unblock_rendering();
        });
    };
    // FIXME: The element must delay the load event of the element's node document until all the attempts to obtain the style sheet's critical subresources, if any, are complete.
    attempts_to_fetch_subresources_finished(m_associated_css_style_sheet);
}

void StyleElementUtils::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_associated_css_style_sheet);
    visitor.visit(m_style_sheet_list);
}

}
