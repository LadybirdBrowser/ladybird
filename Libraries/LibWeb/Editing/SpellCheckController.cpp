/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/SpellCheckController.h>
#include <LibWeb/Editing/SpellingMarkers.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/Page/Page.h>

namespace Web::Editing {

// https://html.spec.whatwg.org/multipage/interaction.html#spelling-and-grammar-checking
static bool is_checkable_text_control(HTML::HTMLElement& element)
{
    // User agents should only consider the following pieces of text as checkable for the purposes of this feature:
    // - The value of input elements whose type attributes are in the Text, Search, URL, or Email states and that are
    //   mutable (i.e. that do not have the readonly attribute specified and that are not disabled).
    if (auto* input = as_if<HTML::HTMLInputElement>(element)) {
        using enum HTML::HTMLInputElement::TypeAttributeState;
        if (!first_is_one_of(input->type_state(), Text, Search, URL, Email))
            return false;
        return input->is_mutable() && input->spellcheck();
    }
    // - The value of textarea elements that do not have a readonly attribute and that are not disabled.
    if (auto* text_area = as_if<HTML::HTMLTextAreaElement>(element))
        return text_area->is_mutable() && text_area->spellcheck();
    return false;
}

// The editable a node's text belongs to for spellchecking: a checkable text control, for text in its user-agent shadow
// tree, or else the node's editing host.
static GC::Ptr<DOM::Node> checkable_editable_for(DOM::Node& node)
{
    GC::Ptr<DOM::Node> current = node;
    while (auto shadow_root = current->containing_shadow_root()) {
        if (!shadow_root->is_user_agent_internal())
            break;
        current = shadow_root->host();
    }
    if (auto* element = as_if<HTML::HTMLElement>(*current); element && is<HTML::FormAssociatedTextControlElement>(*element))
        return is_checkable_text_control(*element) ? current : nullptr;
    return current->editing_host();
}

static bool text_is_checkable(DOM::Text& text)
{
    auto editable = checkable_editable_for(text);
    if (!editable)
        return false;
    // For the values of input and textarea elements, it is the element itself.
    if (is<HTML::FormAssociatedTextControlElement>(*editable))
        return true;
    // - Text in Text nodes that are children of editing hosts or editable elements.
    // For text that is part of a Text node, the element with which the text is associated is the element that is the
    // immediate parent of the first character of the word, sentence, or other piece of text.
    auto* parent = as_if<HTML::HTMLElement>(text.parent());
    return parent && parent->is_editable_or_editing_host() && parent->spellcheck();
}

static bool is_separator(char16_t code_unit)
{
    return code_unit == ' ' || code_unit == '\t' || code_unit == '\n' || code_unit == '\r' || code_unit == 0xa0;
}

// The first node in tree order that comes after node and all of its descendants.
static GC::Ptr<DOM::Node> next_after_subtree(DOM::Node& node)
{
    for (GC::Ptr<DOM::Node> current = node; current; current = current->parent()) {
        if (auto* sibling = current->next_sibling())
            return sibling;
    }
    return nullptr;
}

// The first node in tree order at or after a boundary point.
static GC::Ptr<DOM::Node> node_at_boundary(DOM::Node& container, size_t offset)
{
    if (is<DOM::CharacterData>(container))
        return container;
    if (auto* child = container.child_at_index(offset))
        return child;
    return next_after_subtree(container);
}

SpellCheckController::SpellCheckController(DOM::Document& document)
    : m_document(document)
{
}

void SpellCheckController::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_document);
}

// Text the document still holds. A node that left it, for another document or for none, gets no checks or answers here.
bool SpellCheckController::owns(DOM::Text const& text) const
{
    return text.is_connected() && &text.document() == m_document.ptr();
}

SpellCheckController::Paragraph SpellCheckController::paragraph_containing(DOM::Text& text, size_t offset) const
{
    Paragraph paragraph;
    auto editable = checkable_editable_for(text);
    if (!editable)
        return paragraph;

    // A text control's value is one text node.
    if (is<HTML::FormAssociatedTextControlElement>(*editable)) {
        paragraph.append({ text, 0, text.length_in_utf16_code_units(), true });
        return paragraph;
    }

    // Visit only the nodes between the paragraph's boundary points.
    auto bounds = paragraph_bounds_containing(text, offset);
    auto stop = is<DOM::CharacterData>(*bounds.end_node) ? next_after_subtree(bounds.end_node) : node_at_boundary(bounds.end_node, bounds.end_offset);
    for (auto node = node_at_boundary(bounds.start_node, bounds.start_offset); node && node != stop; node = node->next_in_pre_order()) {
        auto* piece_text = as_if<DOM::Text>(*node);
        if (!piece_text || !editable->is_inclusive_ancestor_of(*piece_text))
            continue;
        size_t node_start = piece_text == bounds.start_node.ptr() ? bounds.start_offset : 0;
        size_t node_end = piece_text == bounds.end_node.ptr() ? bounds.end_offset : piece_text->length_in_utf16_code_units();
        if (node_start < node_end)
            paragraph.append({ *piece_text, node_start, node_end, text_is_checkable(*piece_text) });
    }
    return paragraph;
}

// The first text node of each of an editable's paragraphs, in order.
Vector<GC::Weak<DOM::Text>> SpellCheckController::paragraphs_of(DOM::Node& editable) const
{
    Vector<GC::Weak<DOM::Text>> paragraphs;
    if (auto* text_control = as_if<HTML::FormAssociatedTextControlElement>(editable)) {
        if (auto text = text_control->form_associated_element_to_text_node())
            paragraphs.append(*text);
        return paragraphs;
    }
    HashTable<DOM::Text const*> covered;
    editable.for_each_in_inclusive_subtree_of_type<DOM::Text>([&](DOM::Text& text) {
        if (covered.contains(&text))
            return TraversalDecision::Continue;
        auto paragraph = paragraph_containing(text, 0);
        for (auto const& piece : paragraph)
            covered.set(piece.text.ptr());
        if (!paragraph.is_empty())
            paragraphs.append(*paragraph.first().text);
        return TraversalDecision::Continue;
    });
    return paragraphs;
}

// Builds the request for a window of a paragraph's text, widened to whitespace so no word is cut. Text whose checking
// is disabled goes as spaces of the same length, so offsets stay put and the words on either side don't merge.
SpellCheckController::Request SpellCheckController::make_request(Paragraph const& paragraph, size_t window_start, size_t window_end) const
{
    size_t length = 0;
    for (auto const& piece : paragraph)
        length += piece.node_end - piece.node_start;
    auto code_unit_at = [&](size_t offset) -> char16_t {
        for (auto const& piece : paragraph) {
            auto piece_length = piece.node_end - piece.node_start;
            if (offset < piece_length)
                return piece.text->data().utf16_view().code_unit_at(piece.node_start + offset);
            offset -= piece_length;
        }
        VERIFY_NOT_REACHED();
    };
    window_start = min(window_start, length);
    window_end = min(window_end, length);
    while (window_start > 0 && !is_separator(code_unit_at(window_start - 1)))
        --window_start;
    while (window_end < length && !is_separator(code_unit_at(window_end)))
        ++window_end;

    Request request;
    request.window_end = window_end;
    Utf16StringBuilder text;
    size_t text_length = 0;
    size_t paragraph_offset = 0;
    for (auto const& piece : paragraph) {
        auto piece_length = piece.node_end - piece.node_start;
        auto overlap_start = max(paragraph_offset, window_start);
        auto overlap_end = min(paragraph_offset + piece_length, window_end);
        if (overlap_start < overlap_end) {
            auto node_start = piece.node_start + (overlap_start - paragraph_offset);
            auto node_end = piece.node_start + (overlap_end - paragraph_offset);
            auto data = piece.text->data().utf16_view().substring_view(node_start, node_end - node_start);
            request.pieces.append({
                .text = *piece.text,
                .snapshot = Utf16String::from_utf16(data),
                .node_start = node_start,
                .node_end = node_end,
                .offset = text_length,
                .checked = piece.checked,
            });
            if (piece.checked)
                text.append(data);
            else
                text.append_repeated_ascii(' ', data.length_in_code_units());
            text_length += data.length_in_code_units();
        }
        paragraph_offset += piece_length;
    }
    request.text = text.to_string();
    return request;
}

void SpellCheckController::send(Request request)
{
    auto text = request.text;
    ++m_requests_in_flight;
    m_document->page().request_spell_check(text, GC::create_function(m_document->heap(), [document = GC::Weak<DOM::Document> { m_document }, request = move(request)](Vector<SpellingError> errors) {
        auto strong_document = document.ptr();
        if (!strong_document)
            return;
        strong_document->spell_check_controller().did_receive_answer(request, errors);
        strong_document->page().notify_spell_check_progress();
    }));
}

void SpellCheckController::did_receive_answer(Request const& request, Vector<SpellingError> const& errors)
{
    --m_requests_in_flight;
    apply(request, errors);
    m_existing_text_chunk_in_flight = false;
    send_next_existing_text_chunk();
}

// Checking existing text goes one chunk at a time: The next chunk goes out once the previous one's answer is in.
// A chunk takes as many paragraphs as fit, so a document of many short paragraphs doesn't take a round trip for each.
void SpellCheckController::send_next_existing_text_chunk()
{
    if (!m_existing_text_pass.has_value() || m_existing_text_chunk_in_flight)
        return;
    auto& pass = *m_existing_text_pass;
    auto next_paragraph = [&] {
        ++pass.next_paragraph;
        pass.next_offset = 0;
    };

    Request chunk;
    Utf16StringBuilder chunk_text;
    size_t chunk_length = 0;
    while (pass.next_paragraph < pass.paragraphs.size() && chunk_length < EXISTING_TEXT_CHUNK_SIZE) {
        auto anchor = pass.paragraphs[pass.next_paragraph].ptr();
        if (!anchor || !owns(*anchor)) {
            next_paragraph();
            continue;
        }
        auto paragraph = paragraph_containing(*anchor, 0);
        size_t length = 0;
        for (auto const& piece : paragraph)
            length += piece.node_end - piece.node_start;
        if (pass.next_offset >= length) {
            next_paragraph();
            continue;
        }
        // A paragraph that doesn't fit in what's left of a chunk waits for the next one, unless the chunk is empty.
        auto room = EXISTING_TEXT_CHUNK_SIZE - chunk_length;
        if (chunk_length > 0 && length - pass.next_offset > room)
            break;
        auto request = make_request(paragraph, pass.next_offset, pass.next_offset + room);
        if (request.window_end <= pass.next_offset) {
            next_paragraph();
            continue;
        }
        pass.next_offset = request.window_end;
        if (pass.next_offset >= length)
            next_paragraph();

        // A line break keeps the paragraphs in one chunk apart, so their words don't run together.
        if (chunk_length > 0) {
            chunk_text.append_code_unit('\n');
            ++chunk_length;
        }
        for (auto& piece : request.pieces) {
            piece.offset += chunk_length;
            chunk.pieces.append(move(piece));
        }
        chunk_text.append(request.text.utf16_view());
        chunk_length += request.text.length_in_code_units();
    }

    if (chunk.pieces.is_empty()) {
        if (pass.next_paragraph >= pass.paragraphs.size())
            m_existing_text_pass.clear();
        return;
    }
    chunk.text = chunk_text.to_string();
    m_existing_text_chunk_in_flight = true;
    send(move(chunk));
}

bool SpellCheckController::overlaps_exempt_word(DOM::Text const& text, size_t start, size_t end) const
{
    if (!m_exempt_word.has_value() || m_exempt_word->text.ptr().ptr() != &text)
        return false;
    return start <= m_exempt_word->end && m_exempt_word->start <= end;
}

void SpellCheckController::apply(Request const& request, Vector<SpellingError> const& errors)
{
    auto& markers = m_document->spelling_markers();
    for (auto const& piece : request.pieces) {
        auto text = piece.text.ptr();
        // Text that left the document, or whose checking turned off, since the text was sent takes nothing from the
        // answer.
        if (!text || !piece.checked || !owns(*text) || !text_is_checkable(*text))
            continue;
        // A node edited since the text was sent is dirty again, and the next check takes it.
        auto data = text->data().utf16_view();
        if (piece.node_end > data.length_in_code_units()
            || data.substring_view(piece.node_start, piece.node_end - piece.node_start) != piece.snapshot.utf16_view())
            continue;
        markers.remove_overlapping(*text, piece.node_start, piece.node_end);
        auto piece_length = piece.node_end - piece.node_start;
        for (auto const& error : errors) {
            auto error_start = max<size_t>(error.offset, piece.offset);
            auto error_end = min<size_t>(error.offset + error.length, piece.offset + piece_length);
            if (error_start >= error_end)
                continue;
            auto start = piece.node_start + (error_start - piece.offset);
            auto end = piece.node_start + (error_end - piece.offset);
            if (!overlaps_exempt_word(*text, start, end))
                markers.add(*text, start, end);
        }
    }
}

// Checking existing text: When a checkable editable gains focus, its whole text is checked, one chunk at a time.
void SpellCheckController::did_focus(GC::Ptr<DOM::Node> node)
{
    m_existing_text_pass.clear();
    if (!node || !m_document->page().spell_checking_available())
        return;
    auto editable = checkable_editable_for(*node);
    if (!editable)
        return;
    m_existing_text_pass = ExistingTextPass { .paragraphs = paragraphs_of(*editable) };
    send_next_existing_text_chunk();
}

bool SpellCheckController::is_settled() const
{
    return !m_existing_text_pass.has_value() && m_requests_in_flight == 0;
}

}
