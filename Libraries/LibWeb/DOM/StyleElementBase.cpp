/*
 * Copyright (c) 2023, Preston Taylor <PrestonLeeTaylor@proton.me>
 * Copyright (c) 2025, Lorenz Ackermann <me@lorenzackermann.xyz>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StyleElementBase.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::DOM {

void StyleElementBase::set_parser_document(Badge<HTML::HTMLParser>, GC::Ref<Document> document)
{
    m_parser_document = document;
    m_is_on_parser_stack_of_open_elements = true;
}

void StyleElementBase::did_pop_off_parser_stack_of_open_elements()
{
    m_is_on_parser_stack_of_open_elements = false;
    update_a_style_block(UpdateSource::ParserPop);
}

void StyleElementBase::update_a_style_block_for_dynamic_change()
{
    if (m_is_on_parser_stack_of_open_elements)
        return;
    update_a_style_block();
}

void StyleElementBase::style_element_attribute_changed(FlyString const& name, Optional<String> const& value)
{
    if (name == HTML::AttributeNames::media) {
        if (auto* sheet = this->sheet()) {
            sheet->set_media(value.value_or({}));
            associated_style_sheet_media_attribute_changed();
        }
    } else if (name == HTML::AttributeNames::type) {
        update_a_style_block_for_dynamic_change();
    }
}

// The user agent must run the "update a style block" algorithm whenever one of the following conditions occur:
// The element is popped off the stack of open elements of an HTML parser or XML parser.
//
// NOTE: This is basically done by children_changed() today:
// The element's children changed steps run.
//
// NOTE: This is basically done by inserted() and removed_from() today:
// The element is not on the stack of open elements of an HTML parser or XML parser, and it becomes connected or disconnected.
//
// https://html.spec.whatwg.org/multipage/semantics.html#update-a-style-block
void StyleElementBase::update_a_style_block(UpdateSource update_source)
{
    auto& style_element = as_element();

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

    // AD-HOC: The script-blocking style sheet set tracks elements, but a style element's associated sheet can be
    //         replaced before that sheet's critical subresources finish loading. Keep that set and any queued
    //         completions in sync with the sheet removed above.
    ++m_style_sheet_update_generation;
    remove_from_script_blocking_style_sheet_set_if_needed();
    clear_associated_css_style_sheet_parser_blocking_state();

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
        {},
        nullptr,
        nullptr);

    evaluate_associated_style_sheet_media_queries();
    if (update_source == UpdateSource::ParserPop) {
        m_associated_css_style_sheet_was_created_by_parser = true;
        m_associated_css_style_sheet_was_enabled_when_created_by_parser = !m_associated_css_style_sheet->disabled();
        m_associated_css_style_sheet_load_is_pending_for_script_blocking = m_associated_css_style_sheet_was_enabled_when_created_by_parser
            && m_associated_css_style_sheet_media_matches_environment
            && m_associated_css_style_sheet->loading_state() == CSS::CSSStyleSheet::LoadingState::Loading;
    }

    // 7. If element contributes a script-blocking style sheet, append element to its node document's script-blocking style sheet set.
    if (style_element.contributes_a_script_blocking_style_sheet()) {
        m_document_load_event_delayer.emplace(style_element.document());
        style_element.document().script_blocking_style_sheet_set().set(style_element);
        m_associated_css_style_sheet_is_blocking_scripts = true;
    }

    // FIXME: 8. If element's media attribute's value matches the environment and element is potentially render-blocking, then block rendering on element.

    // AD-HOC: Check if we have already loaded the sheet's resources.
    auto loading_state = m_associated_css_style_sheet->loading_state();
    if (loading_state == CSS::CSSStyleSheet::LoadingState::Loaded || loading_state == CSS::CSSStyleSheet::LoadingState::Error) {
        finished_loading_critical_subresources(loading_state == CSS::CSSStyleSheet::LoadingState::Error ? AnyFailed::Yes : AnyFailed::No);
    }
}

// https://html.spec.whatwg.org/multipage/semantics.html#the-style-element:critical-subresources
void StyleElementBase::finished_loading_critical_subresources(AnyFailed any_failed)
{
    // 1. Let element be the style element associated with the style sheet in question.
    auto& element = as_element();
    auto const style_sheet_update_generation = m_style_sheet_update_generation;

    // 2. Let success be true.
    auto success = true;

    // 3. If the attempts to obtain any of the style sheet's critical subresources failed for any reason
    //    (e.g., DNS error, HTTP 404 response, a connection being prematurely closed, unsupported Content-Type),
    //    set success to false.
    //    Note that content-specific errors, e.g., CSS parse errors or PNG decoding errors, do not affect success.
    if (any_failed == AnyFailed::Yes)
        success = false;

    // 4. Queue an element task on the networking task source given element and the following steps:
    element.queue_an_element_task(HTML::Task::Source::UserInteraction, [&element, success, style_sheet_update_generation] {
        // AD-HOC: If the associated stylesheet has been replaced or removed since this task was queued, ignore it.
        auto* style_element_base = as_if<StyleElementBase>(element);
        if (!style_element_base || style_element_base->m_style_sheet_update_generation != style_sheet_update_generation)
            return;

        // 1. If success is true, fire an event named load at element.
        // AD-HOC: these should call fire an event this is not implemented anywhere so we dispatch it ourselves
        if (success)
            element.dispatch_event(DOM::Event::create(element.realm(), HTML::EventNames::load));
        // 2. Otherwise, fire an event named error at element.
        else
            element.dispatch_event(DOM::Event::create(element.realm(), HTML::EventNames::error));
        // 3. If element contributes a script-blocking style sheet:
        if (element.contributes_a_script_blocking_style_sheet()) {
            // 1. Assert: element's node document's script-blocking style sheet set contains element.
            VERIFY(element.document().script_blocking_style_sheet_set().contains(element));
            VERIFY(style_element_base->m_associated_css_style_sheet_is_blocking_scripts);
            // 2. Remove element from its node document's script-blocking style sheet set.
            element.document().script_blocking_style_sheet_set().remove(element);
            element.document().schedule_html_parser_end_check();
            style_element_base->m_associated_css_style_sheet_is_blocking_scripts = false;
        }
        // 4. Unblock rendering on element.
        element.unblock_rendering();
        style_element_base->m_associated_css_style_sheet_load_is_pending_for_script_blocking = false;
    });
    m_document_load_event_delayer.clear();
}

void StyleElementBase::associated_style_sheet_media_attribute_changed()
{
    evaluate_associated_style_sheet_media_queries();
    if (!as_element().contributes_a_script_blocking_style_sheet()) {
        remove_from_script_blocking_style_sheet_set_if_needed();
        m_associated_css_style_sheet_load_is_pending_for_script_blocking = false;
    }
}

// https://html.spec.whatwg.org/multipage/semantics.html#contributes-a-script-blocking-style-sheet
bool StyleElementBase::style_element_contributes_a_script_blocking_style_sheet() const
{
    // An element el in the context of a Document of an HTML parser or XML parser contributes a script-blocking style
    // sheet if all of the following are true:
    auto& element = as_element();

    // el was created by that Document's parser.
    if (m_parser_document != &element.document())
        return false;

    if (!m_associated_css_style_sheet_was_created_by_parser)
        return false;

    // el is either a style element or a link element that was an external resource link that contributes to the
    // styling processing model when the el was created by the parser.
    // NB: This is a style element, so all good!

    // el's media attribute's value matches the environment.
    if (!m_associated_css_style_sheet_media_matches_environment)
        return false;

    // el's style sheet was enabled when the element was created by the parser.
    if (!m_associated_css_style_sheet_was_enabled_when_created_by_parser)
        return false;

    // FIXME: The last time the event loop reached step 1, el's root was that Document.

    // FIXME: The user agent hasn't given up on loading that particular style sheet yet.
    //        A user agent may give up on loading a style sheet at any time.

    // AD-HOC: Once this parser-created stylesheet has finished loading or stopped blocking scripts, it no longer
    //         contributes a script-blocking stylesheet even though it is still the current associated stylesheet.
    // FIXME: May not be needed once the other clauses above are implemented.
    if (!m_associated_css_style_sheet_load_is_pending_for_script_blocking)
        return false;

    return true;
}

void StyleElementBase::evaluate_associated_style_sheet_media_queries()
{
    if (!m_associated_css_style_sheet) {
        m_associated_css_style_sheet_media_matches_environment = false;
        return;
    }

    m_associated_css_style_sheet->evaluate_media_queries(as_element().document());
    m_associated_css_style_sheet_media_matches_environment = m_associated_css_style_sheet->media()->matches();
}

void StyleElementBase::remove_from_script_blocking_style_sheet_set_if_needed()
{
    if (!m_associated_css_style_sheet_is_blocking_scripts)
        return;

    auto& element = as_element();
    auto& script_blocking_style_sheet_set = element.document().script_blocking_style_sheet_set();
    if (script_blocking_style_sheet_set.contains(element)) {
        script_blocking_style_sheet_set.remove(element);
        element.document().schedule_html_parser_end_check();
    }

    m_associated_css_style_sheet_is_blocking_scripts = false;
    m_document_load_event_delayer.clear();
}

void StyleElementBase::clear_associated_css_style_sheet_parser_blocking_state()
{
    m_associated_css_style_sheet_was_created_by_parser = false;
    m_associated_css_style_sheet_was_enabled_when_created_by_parser = false;
    m_associated_css_style_sheet_media_matches_environment = false;
    m_associated_css_style_sheet_load_is_pending_for_script_blocking = false;
    m_associated_css_style_sheet_is_blocking_scripts = false;
}

// https://www.w3.org/TR/cssom/#dom-linkstyle-sheet
CSS::CSSStyleSheet* StyleElementBase::sheet()
{
    // The sheet attribute must return the associated CSS style sheet for the node or null if there is no associated CSS style sheet.
    return m_associated_css_style_sheet;
}

// https://www.w3.org/TR/cssom/#dom-linkstyle-sheet
CSS::CSSStyleSheet const* StyleElementBase::sheet() const
{
    // The sheet attribute must return the associated CSS style sheet for the node or null if there is no associated CSS style sheet.
    return m_associated_css_style_sheet;
}

void StyleElementBase::visit_style_element_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_associated_css_style_sheet);
    visitor.visit(m_style_sheet_list);
}

}
