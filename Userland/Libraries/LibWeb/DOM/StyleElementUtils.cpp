/*
 * Copyright (c) 2023, Preston Taylor <PrestonLeeTaylor@proton.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StyleElementUtils.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::DOM {

// https://w3c.github.io/webappsec-csp/#should-block-inline
static ErrorOr<ShouldBeBlockedByContentSecurityPolicy> should_elements_inline_type_behavior_be_blocked_by_content_security_policy(
    Element const& element, InlineType type, String const& source)
{
    (void)source;
    (void)type;
    // FIXME: 1. Assert: element is not null.
    //        Ptr type?
    // VERIFY(element);

    // 2. Let result be "Allowed".
    auto result = ShouldBeBlockedByContentSecurityPolicy::Allowed;

    // FIXME: 3. For each policy of element’s Document's global object’s CSP list:
    auto& documents_global_object = element.realm().global_object();
    for (auto const& policy : HTML::retrieve_the_csp_list_of_an_object(documents_global_object).value()) {
        //     FIXME: 1. For each directive of policy’s directive set:
        for (auto directive : policy.directive_set) {
            //         FIXME: 1. If directive’s inline check returns "Allowed" when executed upon element, type, policy and source, skip to the next directive.

            //         FIXME: 2. Let directive-name be the result of executing § 6.8.2 Get the effective directive for inline checks on type.

            //         FIXME: 3. Otherwise, let violation be the result of executing § 2.4.1 Create a violation object for global, policy, and directive on the current settings object’s global object, policy, and directive-name.

            //         FIXME: 4. Set violation’s resource to "inline".

            //         FIXME: 5. Set violation’s element to element.

            //         FIXME: 6. If directive’s value contains the expression "'report-sample'", then set violation’s sample to the substring of source containing its first 40 characters.

            //         FIXME: 7. Execute § 5.5 Report a violation on violation.

            //         FIXME: 8. If policy’s disposition is "enforce", then set result to "Blocked".
            //
        }
    }
    // 4. Return result.
    return result;
}

// The user agent must run the "update a style block" algorithm whenever one of the following conditions occur:
// FIXME: The element is popped off the stack of open elements of an HTML parser or XML parser.
//        Implement by wrapping this and pop in a helper function in the HTML parser?
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

    // FIXME: 5. If the Should element's inline behavior be blocked by Content Security Policy?
    // algorithm returns "Blocked" when executed upon the style element, "style", and the style
    // element's child text content, then return. [CSP]
    if (should_elements_inline_type_behavior_be_blocked_by_content_security_policy(style_element, InlineType::Style, style_element.child_text_content()).value()
        == ShouldBeBlockedByContentSecurityPolicy::Blocked)
        return;

    // FIXME: This is a bit awkward, as the spec doesn't actually tell us when to parse the CSS text,
    //        so we just do it here and pass the parsed sheet to create_a_css_style_sheet().
    auto* sheet = parse_css_stylesheet(CSS::Parser::ParsingContext(style_element.document()), style_element.text_content().value_or(String {}));
    if (!sheet)
        return;

    // FIXME: This should probably be handled by StyleSheet::set_owner_node().
    m_associated_css_style_sheet = sheet;

    // 6. Create a CSS style sheet with the following properties...
    m_style_sheet_list = style_element.document_or_shadow_root_style_sheets();
    m_style_sheet_list->create_a_css_style_sheet(
        "text/css"_string,
        &style_element,
        style_element.attribute(HTML::AttributeNames::media).value_or({}),
        style_element.in_a_document_tree()
            ? style_element.attribute(HTML::AttributeNames::title).value_or({})
            : String {},
        false,
        true,
        {},
        nullptr,
        nullptr,
        *sheet);
}

void StyleElementUtils::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_associated_css_style_sheet);
    visitor.visit(m_style_sheet_list);
}

}
