/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/InputEventsTarget.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/TextReplacement.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Editing {

// How much text before the word goes with a request, in UTF-16 code units, so the platform sees the word in context.
static constexpr size_t CONTEXT_SIZE = 512;

static bool ends_a_word(u32 code_point)
{
    switch (code_point) {
    case ' ':
    case '\t':
    case '\n':
    case 0xA0:
    case '.':
    case ',':
    case ';':
    case ':':
    case '!':
    case '?':
        return true;
    default:
        return false;
    }
}

// NB: A request holds its text node weakly, so a function can capture it without an edge to trace.
struct Request {
    GC_HOLDS_NO_CELL_EDGES;
    GC::Weak<DOM::Text> text;
    // Where the sent text starts in the text node, and the character that ended the word, right after the sent text.
    size_t context_start { 0 };
    Utf16String sent_text;
    char16_t word_ender { 0 };
    // The page's count of replacements when the request went out.
    u64 serial { 0 };
};

struct Caret {
    GC::Ref<DOM::Text> text;
    size_t offset { 0 };
};

// Where the caret is now, if it's a collapsed one in a text node.
static Optional<Caret> caret_in(DOM::Document& document)
{
    auto* target = document.active_input_events_target();
    if (!target)
        return {};
    if (auto* control = as_if<HTML::FormAssociatedTextControlElement>(*target)) {
        auto text = control->form_associated_element_to_text_node();
        if (!text || control->selection_start() != control->selection_end())
            return {};
        return Caret { *text, control->selection_end() };
    }
    auto selection = document.get_selection();
    if (!selection || !selection->is_collapsed())
        return {};
    auto* text = as_if<DOM::Text>(selection->focus_node().ptr());
    if (!text)
        return {};
    return Caret { *text, selection->focus_offset() };
}

// The text control the text is in, for text in its user-agent shadow tree, or else the element the text is in.
static GC::Ptr<HTML::HTMLElement const> element_for(DOM::Text const& text)
{
    GC::Ptr<DOM::Node const> node = text;
    while (auto shadow_root = node->containing_shadow_root()) {
        if (!shadow_root->is_user_agent_internal())
            break;
        node = shadow_root->host();
    }
    if (auto const* element = as_if<HTML::HTMLElement>(*node))
        return element;
    return as_if<HTML::HTMLElement>(node->parent_element().ptr());
}

static bool is_word_code_unit(char16_t code_unit)
{
    return !ends_a_word(code_unit) && code_unit != '\r';
}

void replace_word_after_typing(DOM::Document& document, InputEventsTarget const& typed_into, u32 typed_code_point)
{
    if (!ends_a_word(typed_code_point) || document.active_input_events_target() != &typed_into)
        return;
    auto caret = caret_in(document);
    if (!caret.has_value())
        return;

    // A replacement is an autocorrection, so it stays out of password fields and of whatever the page turned it off for.
    auto element = element_for(caret->text);
    if (!element || element->used_autocorrection_state() == HTML::HTMLElement::AutocorrectionState::Off)
        return;

    // The character just typed is right before the caret, and the word it ended is right before that. A space typed in
    // an editing host can go in as a no-break space.
    auto data = caret->text->data().utf16_view();
    if (caret->offset < 2 || caret->offset > data.length_in_code_units())
        return;
    auto word_ender = data.code_unit_at(caret->offset - 1);
    if (word_ender != typed_code_point && !(typed_code_point == ' ' && word_ender == 0xA0))
        return;
    auto word_end = caret->offset - 1;
    if (!is_word_code_unit(data.code_unit_at(word_end - 1)))
        return;
    auto context_start = word_end > CONTEXT_SIZE ? word_end - CONTEXT_SIZE : 0;
    auto sent_text = Utf16String::from_utf16(data.substring_view(context_start, word_end - context_start));

    Request request {
        .text = caret->text,
        .context_start = context_start,
        .sent_text = sent_text,
        .word_ender = word_ender,
        .serial = document.page().text_replacement_serial(),
    };
    document.page().request_text_replacement(sent_text, GC::create_function(document.heap(), GC::captures(move(request)), [](auto& request, Optional<TextReplacement> replacement) {
        auto sent_length = request.sent_text.length_in_code_units();
        if (!replacement.has_value() || replacement->start + replacement->length != sent_length || replacement->length == 0)
            return;
        auto text = request.text.ptr();
        if (!text)
            return;

        // The user may have typed on while the answer was on its way, and that's fine, as long as they're still typing
        // in that text with nothing selected: A replacement never takes a selection or the focus from them.
        auto& document = text->document();
        auto* navigable = as_if<HTML::LocalNavigable>(document.navigable().ptr());
        auto* target = document.active_input_events_target();
        auto caret = caret_in(document);
        if (!navigable || !target || !caret.has_value() || caret->text.ptr() != text.ptr())
            return;
        if (auto element = element_for(*text); !element || element->used_autocorrection_state() == HTML::HTMLElement::AutocorrectionState::Off)
            return;

        // The word has to be where it was, or where a replacement of an earlier word moved it, with the character that
        // ended it still after it.
        auto word = request.sent_text.utf16_view().substring_view(replacement->start, replacement->length);
        auto word_length = word.length_in_code_units();
        auto typed_word_end = request.context_start + sent_length;
        auto shifted = static_cast<i64>(typed_word_end) + document.page().text_replacement_shift_since(*text, request.serial, typed_word_end - word_length);
        auto data = text->data().utf16_view();
        if (shifted < static_cast<i64>(word_length) || static_cast<size_t>(shifted) >= caret->offset)
            return;
        auto word_end = static_cast<size_t>(shifted);
        if (data.code_unit_at(word_end) != request.word_ender
            || data.substring_view(word_end - word_length, word_length) != word
            || (word_end > word_length && is_word_code_unit(data.code_unit_at(word_end - word_length - 1))))
            return;

        // The caret stays where it was in the text after the word, which is the only text the edit doesn't move.
        auto caret_after_word = caret->offset - word_end;
        auto restore_caret = [&](size_t offset) {
            target->set_selection_anchor(*text, offset);
            target->set_selection_focus(*text, offset);
        };
        target->set_selection_anchor(*text, word_end - word_length);
        target->set_selection_focus(*text, word_end);
        if (navigable->event_handler().insert_replacement_text(replacement->text) != EventResult::Handled) {
            // The page canceled the replacement, and the caret goes back where it was, or as near as what's left allows.
            restore_caret(min<size_t>(caret->offset, text->length_in_utf16_code_units()));
            return;
        }
        document.page().record_text_replacement(*text, word_end, static_cast<i64>(replacement->text.length_in_code_units()) - static_cast<i64>(word_length));

        if (auto replaced = caret_in(document); replaced.has_value() && replaced->text.ptr() == text.ptr())
            restore_caret(min<size_t>(replaced->offset + caret_after_word, text->length_in_utf16_code_units()));
    }));
}

}
