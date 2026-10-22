/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/WritingSuggestionController.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Page/Page.h>

namespace Web::Editing {

WritingSuggestionController::WritingSuggestionController(DOM::Document& document)
    : m_document(document)
{
}

void WritingSuggestionController::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_document);
}

// https://html.spec.whatwg.org/multipage/interaction.html#writing-suggestions
bool WritingSuggestionController::is_eligible(DOM::Node const& caret_container)
{
    // User agents should only offer suggestions within an element's scope if the result of running the following
    // algorithm given element returns true:

    // NB: The element is a text control, for text in its user-agent shadow tree, or else the element the caret is in.
    GC::Ptr<DOM::Node const> node = caret_container;
    while (auto shadow_root = node->containing_shadow_root()) {
        if (!shadow_root->is_user_agent_internal())
            break;
        node = shadow_root->host();
    }
    GC::Ptr<DOM::Element const> element = is<DOM::Element>(*node) ? as<DOM::Element>(node.ptr()) : node->parent_element();
    if (!element)
        return false;

    // 1. If the user has disabled writing suggestions, then return false.
    // NB: The UI process asks the platform only if the user has them enabled, and answers with no suggestion otherwise.

    // 2. If none of the following conditions are true:
    //    - element is an input element whose type attribute is in either the Text, Search, Telephone, URL, or Email
    //      state and element is mutable;
    //    - element is a textarea element that is mutable; or
    //    - element is an editing host or is editable,
    //    then return false.
    auto is_eligible_element = [&] {
        if (auto const* input = as_if<HTML::HTMLInputElement>(*element)) {
            using enum HTML::HTMLInputElement::TypeAttributeState;
            return first_is_one_of(input->type_state(), Text, Search, Telephone, URL, Email) && input->is_mutable();
        }
        if (auto const* text_area = as_if<HTML::HTMLTextAreaElement>(*element))
            return text_area->is_mutable();
        return element->is_editable_or_editing_host();
    };
    if (!is_eligible_element())
        return false;

    // 3. If element has an inclusive ancestor with a writingsuggestions content attribute that's not in the Default and
    //    the nearest such ancestor's writingsuggestions content attribute is in the False state, then return false.
    auto const* html_element = as_if<HTML::HTMLElement>(*element);
    if (!html_element)
        html_element = element->first_ancestor_of_type<HTML::HTMLElement>();
    if (html_element && html_element->writing_suggestions() == u"false"sv)
        return false;

    // AD-HOC: The spec doesn't consider autocomplete. But a page that turns autocomplete off on a field likely doesn't
    //         want text predicted in it either.
    if (auto autocomplete = element->attribute(HTML::AttributeNames::autocomplete); autocomplete.has_value() && autocomplete->equals_ignoring_ascii_case(u"off"sv))
        return false;

    // 4. Otherwise, return true.
    return true;
}

// Whether layout draws a suggestion in the text. Text that a ::first-letter may split gets none, so a suggestion there
// would be one the user can't see.
bool WritingSuggestionController::can_draw_at(DOM::Text const& text)
{
    if (text.style_node_id().value() == 0)
        return false;
    for (auto const* ancestor = text.parent_or_shadow_host_node(); ancestor; ancestor = ancestor->parent_or_shadow_host_node()) {
        if (auto const* element = as_if<DOM::Element>(*ancestor); element && element->has_style(CSS::PseudoElement::FirstLetter))
            return false;
    }
    return true;
}

// How much of the text before the caret is remembered, to notice a script changing it.
static constexpr size_t SHOWING_PREFIX_SIZE = 64;

static Utf16View text_before(DOM::Text const& text, size_t offset)
{
    auto data = text.data().utf16_view();
    offset = min(offset, data.length_in_code_units());
    auto start = offset > SHOWING_PREFIX_SIZE ? offset - SHOWING_PREFIX_SIZE : 0;
    return data.substring_view(start, offset - start);
}

void WritingSuggestionController::set_showing(Optional<ShowingSuggestion> showing)
{
    auto invalidate = [](Optional<ShowingSuggestion> const& suggestion) {
        if (!suggestion.has_value())
            return;
        if (auto text = suggestion->text.ptr(); text && text->parent())
            text->parent()->set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::WritingSuggestionChange);
    };
    invalidate(m_showing);
    m_showing = move(showing);
    GC::Ptr<DOM::Text> text = m_showing.has_value() ? m_showing->text.ptr() : nullptr;
    if (text) {
        m_showing_text_length = text->length_in_utf16_code_units();
        m_showing_prefix = Utf16String::from_utf16(text_before(*text, m_showing->offset));
    }
    Layout::publish_writing_suggestion(m_document, text.ptr(), text ? m_showing->offset : 0, text ? m_showing->remainder.utf16_view() : Utf16View {});
    invalidate(m_showing);
}

void WritingSuggestionController::dismiss()
{
    ++m_generation;
    if (m_showing.has_value())
        set_showing({});
}

Optional<Utf16String> WritingSuggestionController::accept()
{
    if (!m_showing.has_value())
        return {};
    Optional<Utf16String> remainder;
    if (caret_is_at_showing_suggestion())
        remainder = m_showing->remainder;
    dismiss();
    return remainder;
}

void WritingSuggestionController::did_focus(GC::Ptr<DOM::Node>)
{
    dismiss();
}

// Typing what the suggestion proposes keeps the rest of it showing right away, so it doesn't flicker while a new one
// is on its way. Any other edit drops it. Either way, the text at the caret gets a new prediction.
void WritingSuggestionController::did_user_edit(DOM::Node&)
{
    if (!m_document->page().writing_suggestions_available())
        return;
    ++m_generation;
    if (m_showing.has_value()) {
        auto showing = *m_showing;
        auto text = showing.text.ptr();
        auto caret = caret_text_position(m_document);
        auto typed_through = [&] {
            if (!text || !caret.has_value() || !caret->is_collapsed || caret->text.ptr() != text.ptr() || caret->offset <= showing.offset)
                return false;
            auto typed = caret->offset - showing.offset;
            auto data = text->data().utf16_view();
            return typed < showing.remainder.length_in_code_units()
                && data.length_in_code_units() == m_showing_text_length + typed
                && data.substring_view(showing.offset, typed) == showing.remainder.utf16_view().substring_view(0, typed);
        };
        if (typed_through()) {
            auto typed = caret->offset - showing.offset;
            set_showing(ShowingSuggestion { *text, caret->offset, Utf16String::from_utf16(showing.remainder.utf16_view().substring_view(typed)) });
        } else {
            set_showing({});
        }
    }
    // Committing a composition ends it, and did_end_input_method_composition() then sends the request.
    if (m_document->is_input_method_composing())
        return;
    schedule_request();
}

// When a script changes the text under the suggestion that's showing, the caret moves without a selection change — so
// the suggestion is checked against the text once the change is done.
void WritingSuggestionController::did_change_text(DOM::Text const& text)
{
    if (m_showing.has_value() && m_showing->text.ptr().ptr() == &text)
        schedule_selection_check();
}

void WritingSuggestionController::did_start_input_method_composition()
{
    dismiss();
}

void WritingSuggestionController::did_end_input_method_composition()
{
    if (m_document->page().writing_suggestions_available())
        schedule_request();
}

// Selection changes come in the middle of edits too, so whether the caret left the suggestion is decided once the
// edit is done.
void WritingSuggestionController::did_change_selection()
{
    if (m_showing.has_value() || m_requests_in_flight > 0 || m_request_scheduled)
        schedule_selection_check();
}

void WritingSuggestionController::schedule_selection_check()
{
    if (m_selection_check_scheduled)
        return;
    m_selection_check_scheduled = true;
    HTML::queue_global_task(HTML::Task::Source::UserInteraction, HTML::relevant_global_object(m_document), GC::create_function(m_document->heap(), [document = GC::Weak<DOM::Document> { m_document }] {
        if (auto strong_document = document.ptr()) {
            strong_document->writing_suggestion_controller().run_selection_check();
            strong_document->page().notify_writing_suggestion_progress();
        }
    }));
}

void WritingSuggestionController::run_selection_check()
{
    if (!exchange(m_selection_check_scheduled, false))
        return;
    if (m_showing.has_value() && !caret_is_at_showing_suggestion())
        dismiss();
}

bool WritingSuggestionController::caret_is_at_showing_suggestion() const
{
    if (!m_showing.has_value())
        return false;
    auto caret = caret_text_position(m_document);
    if (!caret.has_value() || !caret->is_collapsed || caret->text.ptr() != m_showing->text.ptr().ptr() || caret->offset != m_showing->offset)
        return false;
    auto& text = *caret->text;
    return text.length_in_utf16_code_units() == m_showing_text_length && text_before(text, caret->offset) == m_showing_prefix.utf16_view()
        && is_eligible(text) && can_draw_at(text);
}

void WritingSuggestionController::schedule_request()
{
    if (m_request_scheduled)
        return;
    m_request_scheduled = true;
    HTML::queue_global_task(HTML::Task::Source::UserInteraction, HTML::relevant_global_object(m_document), GC::create_function(m_document->heap(), [document = GC::Weak<DOM::Document> { m_document }] {
        if (auto strong_document = document.ptr()) {
            strong_document->writing_suggestion_controller().run_request();
            strong_document->page().notify_writing_suggestion_progress();
        }
    }));
}

void WritingSuggestionController::run_request()
{
    if (!exchange(m_request_scheduled, false))
        return;
    auto& page = m_document->page();
    if (!page.writing_suggestions_available() || m_document->is_input_method_composing())
        return;
    auto caret = caret_text_position(m_document);
    if (!caret.has_value() || !caret->is_collapsed || !caret->text->is_connected() || &caret->text->document() != m_document.ptr() || !is_eligible(*caret->text))
        return;

    auto data = caret->text->data().utf16_view();
    auto offset = min(caret->offset, data.length_in_code_units());
    auto start = offset > CONTEXT_SIZE ? offset - CONTEXT_SIZE : 0;
    auto end = min(data.length_in_code_units(), offset + CONTEXT_SIZE);
    Request request {
        .generation = m_generation,
        .text = *caret->text,
        .offset = offset,
        .sent_text = Utf16String::from_utf16(data.substring_view(start, end - start)),
        .caret_offset = static_cast<u32>(offset - start),
        .text_length = data.length_in_code_units(),
    };

    ++m_requests_in_flight;
    auto sent_text = request.sent_text;
    auto caret_offset = request.caret_offset;
    page.request_writing_suggestion(sent_text, caret_offset, GC::create_function(m_document->heap(), [document = GC::Weak<DOM::Document> { m_document }, request = move(request)](Optional<WritingSuggestion> suggestion) {
        auto strong_document = document.ptr();
        if (!strong_document)
            return;
        strong_document->writing_suggestion_controller().did_receive(request, suggestion);
        strong_document->page().notify_writing_suggestion_progress();
    }));
}

void WritingSuggestionController::did_receive(Request const& request, Optional<WritingSuggestion> const& suggestion)
{
    --m_requests_in_flight;
    // An answer to a request that an edit, a caret move, or a dismissal has overtaken describes text that's gone.
    if (request.generation != m_generation || !suggestion.has_value())
        return;
    auto text = request.text.ptr();
    auto caret = caret_text_position(m_document);
    if (!text || !caret.has_value() || !caret->is_collapsed || caret->text.ptr() != text.ptr() || caret->offset != request.offset)
        return;
    // A script may have changed the text, or the element, while the request was out.
    auto sent = request.sent_text.utf16_view();
    if (request.caret_offset > sent.length_in_code_units() || text->length_in_utf16_code_units() != request.text_length
        || text->data().utf16_view().substring_view(request.offset - request.caret_offset, request.caret_offset) != sent.substring_view(0, request.caret_offset)
        || !is_eligible(*text) || !can_draw_at(*text))
        return;

    // Only a suggestion that continues the word that ends at the caret shows. One that corrects it, or that would
    // replace text after the caret, shows nothing.
    auto typed_start = suggestion->replacement_start;
    if (typed_start > request.caret_offset || typed_start + suggestion->replacement_length != request.caret_offset)
        return;
    auto typed = sent.substring_view(typed_start, request.caret_offset - typed_start);
    auto suggested = suggestion->text.utf16_view();
    if (suggested.length_in_code_units() <= typed.length_in_code_units() || suggested.substring_view(0, typed.length_in_code_units()) != typed)
        return;

    set_showing(ShowingSuggestion { *text, request.offset, Utf16String::from_utf16(suggested.substring_view(typed.length_in_code_units())) });
}

bool WritingSuggestionController::is_settled() const
{
    return !m_request_scheduled && !m_selection_check_scheduled && m_requests_in_flight == 0;
}

}
