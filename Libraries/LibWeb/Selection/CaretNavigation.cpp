/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Selection/CaretNavigation.h>

namespace Web::Selection {

// Unicode word boundaries provide candidate stops, while browser editing behavior decides which candidates a command
// crosses. Separators, punctuation, and word content therefore remain distinct through navigation.
enum class WordSegmentKind : u8 {
    Word,
    Punctuation,
    Separator,
};

static WordSegmentKind word_segment_kind(Utf16View const& segment)
{
    if (all_of(segment, [](auto code_point) { return Unicode::code_point_has_separator_general_category(code_point); }))
        return WordSegmentKind::Separator;
    if (all_of(segment, [](auto code_point) { return Unicode::code_point_has_punctuation_general_category(code_point); }))
        return WordSegmentKind::Punctuation;
    return WordSegmentKind::Word;
}

static bool text_node_has_rendered_text(DOM::Text const& text)
{
    Layout::ForcedReadScope read { text.document() };
    auto const* layout_node = text.unsafe_layout_node(read);
    return layout_node && Layout::RustFFI::render_state_text_has_rendered_text(layout_node->document_host(), Layout::Node::slot_id(layout_node));
}

// A <br> that renders an empty line hosts a caret position on its parent, at its child index. This covers a leading
// <br> in a paragraph with text after it, and the lines between consecutive <br>s.
static bool is_empty_line_break(DOM::Node& node)
{
    Layout::ForcedReadScope read { node.document() };
    auto* br = as_if<HTML::HTMLBRElement>(node);
    return br && br->is_editable() && br->represents_empty_line(read);
}

// A block-level element that renders no text but hosts an empty line where the caret can sit, such as `<p><br></p>`.
static bool is_empty_line_host(DOM::Node& node)
{
    Layout::ForcedReadScope read { node.document() };
    auto* element = as_if<DOM::Element>(node);
    if (!element || !element->is_editable())
        return false;
    auto const* layout_node = element->layout_node(read);
    if (!layout_node || !Painting::is_paintable_with_lines(*layout_node) || Painting::display(*layout_node).is_inline_outside())
        return false;

    bool has_rendered_content = false;
    element->for_each_in_subtree([&](DOM::Node& descendant) {
        if (auto* text = as_if<DOM::Text>(descendant); text && text_node_has_rendered_text(*text)) {
            has_rendered_content = true;
            return TraversalDecision::Break;
        }
        auto const* layout_node = descendant.layout_node(read);
        if (descendant.is_editable() && layout_node && layout_node->is_atomic_inline()) {
            has_rendered_content = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return !has_rendered_content;
}

// Atomic inline content, such as an image, has no caret positions inside it. Its two caret positions are represented
// as DOM boundaries in the parent, immediately before and after the atomic node.
static bool is_atomic_inline_caret_host(DOM::Node& node)
{
    Layout::ForcedReadScope read { node.document() };
    if (is<HTML::HTMLBRElement>(node))
        return false;
    auto const* layout_node = node.layout_node(read);
    return node.is_editable() && node.parent() && layout_node && layout_node->is_atomic_inline();
}

// An atomic inline box with no content, such as an empty inline-block, holds a caret position inside it, on no line.
static bool is_empty_inline_block(DOM::Node& node)
{
    Layout::ForcedReadScope read { node.document() };
    auto const* layout_node = node.layout_node(read);
    return is_atomic_inline_caret_host(node) && !node.has_children() && !layout_node->is_replaced_box();
}

// The line the rendered text of `text` starts on, or ends on, where that line holds rendered text of it.
static Optional<Layout::RustFFI::FfiInlineLine> rendered_text_edge_line(DOM::Text const& text, bool end)
{
    Layout::ForcedReadScope read { text.document() };
    auto const* layout_node = text.unsafe_layout_node(read);
    if (!layout_node)
        return {};
    auto line = Layout::RustFFI::render_state_text_rendered_edge_line(layout_node->document_host(), Layout::Node::slot_id(layout_node), end);
    if (line.root.index == Compositing::RustFFI::INVALID_NODE_SLOT_INDEX)
        return {};
    return line;
}

static bool boundary_visual_lines_share_line(DOM::Text const& before, DOM::Text const& after)
{
    auto before_line = rendered_text_edge_line(before, true);
    auto after_line = rendered_text_edge_line(after, false);
    return before_line.has_value() && after_line.has_value()
        && before_line->root.index == after_line->root.index && before_line->index == after_line->index;
}

static bool boundary_visual_lines_share_inline_context(DOM::Text const& before, DOM::Text const& after)
{
    auto before_line = rendered_text_edge_line(before, true);
    auto after_line = rendered_text_edge_line(after, false);
    return before_line.has_value() && after_line.has_value() && before_line->root.index == after_line->root.index;
}

// INTEROP: A forward vertical extension that ends at the start of text in another block ends at the equivalent boundary
//          at the start of that block, which keeps the selected DOM range aligned with the rendered block contents.
static CaretLocation canonical_location_at_start_of_inline_content(DOM::Text& text, DOM::Node& block)
{
    DOM::Node* node = &text;
    while (auto* parent = node->parent()) {
        if (parent == &block)
            return { *parent, node->index(), TextAffinity::Downstream };
        node = parent;
    }
    return { text, 0, TextAffinity::Downstream };
}

using InlineLine = Layout::RustFFI::FfiInlineLine;

// Lines inside an atomic inline box are on the line that holds the box, as a move between lines in tree order sees them.
static bool is_same_line(DOM::Document& document, InlineLine const& a, InlineLine const& b)
{
    auto* host = document.layout_node_arena().host();
    auto outer_a = Layout::RustFFI::render_state_caret_line_outside_atomic_inlines(host, a);
    auto outer_b = Layout::RustFFI::render_state_caret_line_outside_atomic_inlines(host, b);
    return outer_a.root.index == outer_b.root.index && outer_a.index == outer_b.index;
}

// A boundary before an element with content is at the start of that content, which is what layout has a caret for.
static Optional<Layout::RustFFI::FfiCaretPosition> layout_caret_position_of(Layout::BegunRead const& read, CaretLocation const& location)
{
    GC::Ref<DOM::Node> node = location.node;
    auto offset = location.offset;
    while (!is<DOM::Text>(*node)) {
        auto* child = node->child_at_index(offset);
        if (!child || !child->has_children() || is_atomic_inline_caret_host(*child))
            break;
        node = *child;
        offset = 0;
    }
    return Painting::layout_caret_position(read, node, offset, location.affinity);
}

static Optional<InlineLine> caret_line(DOM::Document& document, Layout::BegunRead const& read, CaretLocation const& location)
{
    auto position = layout_caret_position_of(read, location);
    if (!position.has_value())
        return {};
    auto line = Layout::RustFFI::render_state_caret_line(document.layout_node_arena().host(), *position);
    if (line.root.index == Compositing::RustFFI::INVALID_NODE_SLOT_INDEX)
        return {};
    return line;
}

static Optional<CaretLocation> caret_location_of(DOM::Document& document, Layout::BegunRead const& read, Layout::RustFFI::FfiInlineCaretPosition const& position)
{
    using Kind = Layout::RustFFI::FfiInlineCaretPositionKind;
    if (position.kind == Kind::None)
        return {};
    auto* layout_node = document.layout_node_arena().node_if_live(read, position.node);
    auto* dom_node = layout_node ? layout_node->dom_node() : nullptr;
    if (!dom_node)
        return {};
    switch (position.kind) {
    case Kind::Text:
        return CaretLocation { *dom_node, position.offset, position.downstream ? TextAffinity::Downstream : TextAffinity::Upstream };
    case Kind::StartOfBox:
        return CaretLocation { *dom_node, 0, TextAffinity::Downstream };
    case Kind::BesideAtomicInline:
    case Kind::BeforeForcedBreak:
        if (!dom_node->parent())
            return {};
        return CaretLocation { *dom_node->parent(), dom_node->index() + (position.after ? 1 : 0), TextAffinity::Downstream };
    case Kind::None:
        break;
    }
    VERIFY_NOT_REACHED();
}

// The caret location at the start of the first line of `text`, or at the end of its last line.
static Optional<CaretLocation> text_edge_location(DOM::Text& text, bool end)
{
    Layout::ForcedReadScope read { text.document() };
    text.document().update_layout_if_needed_for_node(text, DOM::UpdateLayoutReason::CursorLineNavigation);
    auto const* layout_node = text.unsafe_layout_node(read);
    if (!layout_node)
        return {};
    return caret_location_of(text.document(), read, Layout::RustFFI::render_state_text_edge(layout_node->document_host(), Layout::Node::slot_id(layout_node), end));
}

Optional<CaretLocation> location_after_character_step(DOM::Text& text, size_t offset, TextAffinity affinity, SelectionDirection direction)
{
    Layout::ForcedReadScope read { text.document() };
    text.document().update_layout_if_needed_for_node(text, DOM::UpdateLayoutReason::CursorLineNavigation);
    auto forward = direction == SelectionDirection::Forward;
    auto grapheme_boundary = forward ? text.grapheme_segmenter().next_boundary(offset) : text.grapheme_segmenter().previous_boundary(offset);
    auto const* layout_node = text.unsafe_layout_node(read);
    if (!layout_node) {
        if (!grapheme_boundary.has_value())
            return {};
        return CaretLocation { text, *grapheme_boundary, TextAffinity::Downstream };
    }
    return caret_location_of(text.document(), read,
        Layout::RustFFI::render_state_text_character_step(layout_node->document_host(), Layout::Node::slot_id(layout_node),
            offset, affinity == TextAffinity::Downstream, forward, grapheme_boundary.has_value(), grapheme_boundary.value_or(0)));
}

GC::Ptr<DOM::Node> CaretNavigator::adjacent_caret_host(DOM::Node& from, DOM::Node& editing_host, SelectionDirection direction)
{
    auto* node = &from;
    while (node) {
        node = direction == SelectionDirection::Forward
            ? node->next_in_pre_order(&editing_host)
            : node->previous_in_pre_order();
        if (!node || node == &editing_host || !editing_host.is_inclusive_ancestor_of(*node))
            return nullptr;
        // Walking backwards visits ancestors of the origin; the caret is already inside those.
        if (node->is_inclusive_ancestor_of(from))
            continue;
        if (auto* text = as_if<DOM::Text>(*node); text && text->is_editable() && text_node_has_rendered_text(*text))
            return node;
        if (is_atomic_inline_caret_host(*node) || is_empty_line_host(*node) || is_empty_line_break(*node))
            return node;
    }
    return nullptr;
}

DOM::Node& CaretNavigator::navigation_origin(CaretLocation const& location, SelectionDirection direction)
{
    if (direction == SelectionDirection::Forward && location.offset > 0) {
        if (auto* child_before = location.node->child_at_index(location.offset - 1))
            return *child_before;
    }
    if (direction == SelectionDirection::Backward) {
        if (auto* child_after = location.node->child_at_index(location.offset))
            return *child_after;
    }
    return location.node;
}

Optional<CSSPixels> CaretNavigator::inline_coordinate(CaretLocation const& location)
{
    Layout::ForcedReadScope read { *m_document };
    m_document->update_layout_if_needed_for_node(location.node, DOM::UpdateLayoutReason::CursorLineNavigation);
    // A parent boundary after an atomic inline keeps the coordinate of the end of the text before the atomic inline, so
    // that a vertical round trip comes back in front of it.
    auto origin = location;
    if (auto* child_before = location.offset > 0 ? location.node->child_at_index(location.offset - 1) : nullptr; child_before && is_atomic_inline_caret_host(*child_before)) {
        auto editing_host = location.node->editing_host();
        auto previous = editing_host ? adjacent_caret_host(*child_before, *editing_host, SelectionDirection::Backward) : nullptr;
        if (auto* previous_text = as_if<DOM::Text>(previous.ptr()))
            origin = { *previous_text, previous_text->length(), TextAffinity::Downstream };
    }
    auto position = layout_caret_position_of(read, origin);
    if (!position.has_value())
        return {};
    Layout::RustFFI::FfiOptionalCssPixels coordinate;
    Layout::RustFFI::render_state_caret_inline_coordinate(m_document->layout_node_arena().host(), *position, &coordinate);
    if (!coordinate.has_value)
        return {};
    return coordinate.value;
}

Optional<CaretLocation> CaretNavigator::move_to_adjacent_caret_host(CaretLocation const& location, SelectionDirection direction)
{
    auto editing_host = location.node->editing_host();
    if (!editing_host)
        return {};

    m_document->update_layout_if_needed_for_node(*editing_host, DOM::UpdateLayoutReason::CursorLineNavigation);

    auto& origin = navigation_origin(location, direction);
    auto* adjacent_child = direction == SelectionDirection::Forward
        ? location.node->child_at_index(location.offset)
        : (location.offset > 0 ? location.node->child_at_index(location.offset - 1) : nullptr);
    GC::Ptr<DOM::Node> target;
    bool target_is_adjacent_child = false;
    // INTEROP: An empty-line <br> contributes a single caret position at its parent boundary. Once the caret is already
    // at that boundary, forward movement must start after the <br> instead of rediscovering the same position.
    if (direction == SelectionDirection::Forward && adjacent_child && is_empty_line_break(*adjacent_child)) {
        target = adjacent_caret_host(*adjacent_child, *editing_host, direction);
    } else if (adjacent_child) {
        auto* text = as_if<DOM::Text>(*adjacent_child);
        if ((text && text->is_editable() && text_node_has_rendered_text(*text))
            || is_atomic_inline_caret_host(*adjacent_child) || is_empty_line_host(*adjacent_child) || is_empty_line_break(*adjacent_child)) {
            target = adjacent_child;
            target_is_adjacent_child = true;
        }
    }
    if (!target)
        target = adjacent_caret_host(origin, *editing_host, direction);
    if (!target)
        return {};

    size_t offset = 0;

    if (is_atomic_inline_caret_host(*target) && target->parent()) {
        auto target_is_in_origin_parent = target->parent() == origin.parent();
        if (direction == SelectionDirection::Backward && target_is_in_origin_parent) {
            if (auto previous_target = adjacent_caret_host(*target, *editing_host, direction); previous_target && is<DOM::Text>(*previous_target))
                target = previous_target;
        }
        if (is_atomic_inline_caret_host(*target) && target->parent()) {
            // INTEROP: Enter an atomic-only line at its near edge, but cross an atomic child adjacent to the current
            // parent boundary. Chromium exposes both edges in the former case and one visual step in the latter.
            auto use_boundary_after_target = target_is_adjacent_child
                ? direction == SelectionDirection::Forward
                : (direction == SelectionDirection::Forward) == target_is_in_origin_parent;
            offset = target->index() + (use_boundary_after_target ? 1 : 0);
            target = target->parent();
        }
    } else if (is<HTML::HTMLBRElement>(*target) && target->parent()) {
        offset = target->index();
        target = target->parent();
    }

    if (auto* text = as_if<DOM::Text>(*target))
        return text_edge_location(*text, direction == SelectionDirection::Backward);

    return CaretLocation { *target, offset, TextAffinity::Downstream };
}

CaretLocation CaretNavigator::canonical_location_for_editing(CaretLocation const& location)
{
    auto node = location.node;
    auto offset = location.offset;

    auto is_formatting_whitespace = [](DOM::Node const& candidate) {
        auto const* text = as_if<DOM::Text>(candidate);
        if (!text || !text->data().is_ascii_whitespace())
            return false;
        auto* previous_sibling = candidate.previous_sibling();
        auto* next_sibling = candidate.next_sibling();
        return (previous_sibling && Editing::is_prohibited_paragraph_child(const_cast<DOM::Node&>(*previous_sibling)))
            || (next_sibling && Editing::is_prohibited_paragraph_child(const_cast<DOM::Node&>(*next_sibling)));
    };
    if (is<DOM::Comment>(*node) || is<DOM::ProcessingInstruction>(*node)) {
        // INTEROP: Comments and processing instructions cannot contain rendered carets, and DOM range insertion
        //          rejects them. Normalize the caret to the parent boundary immediately before the node, matching the
        //          insertParagraph command's comment handling and giving insertion commands a valid boundary.
        if (!node->parent())
            return location;
        offset = node->index();
        node = *node->parent();
    } else if (is_formatting_whitespace(*node)) {
        // INTEROP: Source indentation between block children of an editing host is not a rendered caret position.
        //          Blink and WebKit associate it with the following paragraph, or the preceding paragraph when the
        //          whitespace trails the final block.
        if (node->next_sibling()) {
            node = *node->next_sibling();
            offset = 0;
        } else if (node->previous_sibling()) {
            node = *node->previous_sibling();
            offset = node->length();
        }
    } else if (!is<DOM::Element>(*node)) {
        return location;
    }
    auto can_descend_into = [&](DOM::Node const& candidate) {
        if (!candidate.is_editable())
            return false;
        if (is<DOM::Text>(candidate)) {
            // INTEROP: Source formatting whitespace between blocks is not a rendered caret position. Chromium keeps
            //          an adjacent editing-host boundary at the element level instead of descending into that text.
            return !is_formatting_whitespace(candidate);
        }
        return is<DOM::Element>(candidate) && candidate.has_children();
    };
    auto is_trailing_placeholder = [](DOM::Node const& candidate) {
        if (!is<HTML::HTMLBRElement>(candidate))
            return false;
        for (auto* sibling = candidate.next_sibling(); sibling; sibling = sibling->next_sibling()) {
            auto const* text = as_if<DOM::Text>(*sibling);
            if (!text || !text->data().is_ascii_whitespace())
                return false;
        }
        return true;
    };

    while (is<DOM::Element>(*node)) {
        auto before_offset = offset;
        GC::Ptr<DOM::Node> before = before_offset > 0 ? node->child_at_index(before_offset - 1) : nullptr;
        // INTEROP: A trailing br is the block's caret placeholder. An element boundary after it canonicalizes to the
        //          preceding editable content, rather than remaining after the placeholder itself.
        while (before && (is_trailing_placeholder(*before) || is_formatting_whitespace(*before))) {
            --before_offset;
            before = before_offset > 0 ? node->child_at_index(before_offset - 1) : nullptr;
        }
        if (before_offset != offset)
            offset = before_offset;

        GC::Ptr<DOM::Node> after = node->child_at_index(offset);
        while (after && is_formatting_whitespace(*after))
            after = after->next_sibling();

        // INTEROP: A boundary between rendered blocks belongs to the following block. Descending backward would
        //          cross a paragraph boundary and expose the end of the preceding block as the same caret.
        if (after && Editing::is_prohibited_paragraph_child(*after) && can_descend_into(*after)) {
            node = *after;
            offset = 0;
        }
        // INTEROP: A br that follows a block starts a line of its own. A boundary before it is on that line, and not at
        //          the end of the preceding block.
        else if (before && can_descend_into(*before)
            && !(Editing::is_prohibited_paragraph_child(*before) && is<HTML::HTMLBRElement>(after.ptr()))) {
            node = *before;
            offset = before->length();
        } else if ((!before || is_formatting_whitespace(*before) || is<HTML::HTMLBRElement>(*before))
            && after && can_descend_into(*after)) {
            node = *after;
            offset = 0;
        } else {
            break;
        }
    }
    return { node, offset, location.affinity };
}

CaretLocation CaretNavigator::upstream_equivalent_location(CaretLocation const& location)
{
    // Blink and WebKit represent a visible position by its most-upstream candidate when one exists. Keep this
    // non-mutating: callers which need a stable snapshot can use the equivalent point without changing the DOM
    // Selection that an editing algorithm will act on.
    return canonical_location_for_extension(location, SelectionDirection::Backward).value_or(location);
}

Optional<CaretLocation> CaretNavigator::canonical_location_for_extension(CaretLocation const& location, SelectionDirection direction)
{
    // INTEROP: A rendered caret boundary can have several equivalent DOM representations, especially between styled
    // text nodes and beside atomic inline content. Chromium canonicalizes the anchor when selection first extends so
    // subsequent movement includes the same rendered content regardless of which equivalent boundary held the caret.
    auto* text = as_if<DOM::Text>(*location.node);
    if (!text)
        return {};
    if ((direction == SelectionDirection::Forward && location.offset != text->length())
        || (direction == SelectionDirection::Backward && location.offset != 0))
        return {};

    auto editing_host = text->editing_host();
    if (!editing_host)
        return {};
    m_document->update_layout_if_needed_for_node(*editing_host, DOM::UpdateLayoutReason::CursorLineNavigation);
    auto target = adjacent_caret_host(*text, *editing_host, direction);
    if (!target)
        return {};
    if (is_atomic_inline_caret_host(*target) && target->parent())
        return CaretLocation { *target->parent(), target->index() + (direction == SelectionDirection::Backward ? 1 : 0), TextAffinity::Downstream };
    auto* target_text = as_if<DOM::Text>(*target);
    if (!target_text)
        return {};
    auto shares_line = direction == SelectionDirection::Forward
        ? boundary_visual_lines_share_line(*text, *target_text)
        : boundary_visual_lines_share_line(*target_text, *text);
    if (!shares_line)
        return {};
    return CaretLocation { *target_text, direction == SelectionDirection::Forward ? 0u : target_text->length(), TextAffinity::Downstream };
}

Optional<CaretLocation> CaretNavigator::location_after_atomic_inline(DOM::Node& atomic_inline)
{
    if (!atomic_inline.parent())
        return {};
    return CaretLocation { *atomic_inline.parent(), atomic_inline.index() + 1, TextAffinity::Downstream };
}

Optional<CaretLocation> CaretNavigator::location_before_atomic_inline(DOM::Node& atomic_inline, SelectionAlteration alteration)
{
    if (!atomic_inline.parent())
        return {};
    // INTEROP: Chromium exposes the parent boundary when extending a range, but canonicalizes a collapsed caret to
    // the preceding text end when possible. This prevents Right from visiting two visually identical positions before
    // the atomic inline while retaining the range boundary needed to include it during selection.
    if (alteration == SelectionAlteration::Extend)
        return CaretLocation { *atomic_inline.parent(), atomic_inline.index(), TextAffinity::Downstream };

    auto editing_host = atomic_inline.editing_host();
    if (!editing_host)
        return {};
    auto previous = adjacent_caret_host(atomic_inline, *editing_host, SelectionDirection::Backward);
    if (auto* previous_text = as_if<DOM::Text>(previous.ptr())) {
        if (auto position = text_edge_location(*previous_text, true); position.has_value())
            return position;
    }
    return CaretLocation { *atomic_inline.parent(), atomic_inline.index(), TextAffinity::Downstream };
}

Optional<CaretLocation> CaretNavigator::canonicalize_backward_word_location(CaretLocation const& location, SelectionAlteration alteration)
{
    // A backward word move can land at the start of a styled text node, which is visually identical to the end of the
    // preceding node in the same inline context. Use that preceding end for a collapsed caret so another Left command
    // advances instead of revisiting the same painted position.
    if (alteration == SelectionAlteration::Extend || location.offset != 0)
        return location;
    auto* text = as_if<DOM::Text>(*location.node);
    if (!text)
        return location;
    auto editing_host = text->editing_host();
    if (!editing_host)
        return location;
    auto previous = adjacent_caret_host(*text, *editing_host, SelectionDirection::Backward);
    auto* previous_text = as_if<DOM::Text>(previous.ptr());
    if (!previous_text || !boundary_visual_lines_share_inline_context(*previous_text, *text))
        return location;
    return CaretLocation { *previous_text, previous_text->length(), TextAffinity::Downstream };
}

Optional<CaretLocation> CaretNavigator::move_by_word(CaretLocation const& initial_location, SelectionAlteration alteration, SelectionDirection direction)
{
    // INTEROP: Chromium word movement skips separators and can continue through DOM-split text when the fragments
    // share a rendered inline context. Punctuation and atomic inline content remain observable stops. This is editing
    // behavior rather than a direct application of the Unicode segmentation algorithm.
    auto editing_host = initial_location.node->editing_host();
    m_document->update_layout_if_needed_for_node(initial_location.node, DOM::UpdateLayoutReason::CursorLineNavigation);

    auto location = initial_location;
    auto* text = as_if<DOM::Text>(*location.node);
    if (!text) {
        auto* adjacent_child = direction == SelectionDirection::Forward
            ? location.node->child_at_index(location.offset)
            : (location.offset > 0 ? location.node->child_at_index(location.offset - 1) : nullptr);
        if (adjacent_child && is_atomic_inline_caret_host(*adjacent_child)) {
            return direction == SelectionDirection::Forward
                ? location_after_atomic_inline(*adjacent_child)
                : location_before_atomic_inline(*adjacent_child, alteration);
        }
        text = as_if<DOM::Text>(adjacent_child);
        if (!text)
            return move_to_adjacent_caret_host(location, direction);
        location = { *text, direction == SelectionDirection::Forward ? 0u : text->length(), TextAffinity::Downstream };
    }

    bool moved = false;
    Optional<CaretLocation> punctuation_end;
    while (true) {
        auto next_offset = direction == SelectionDirection::Forward
            ? text->word_segmenter().next_boundary(location.offset)
            : text->word_segmenter().previous_boundary(location.offset);
        if (next_offset.has_value()) {
            auto segment = direction == SelectionDirection::Forward
                ? text->data().substring_view(location.offset, *next_offset - location.offset)
                : text->data().substring_view(*next_offset, location.offset - *next_offset);
            location.offset = *next_offset;
            auto kind = word_segment_kind(segment);
            if (punctuation_end.has_value() && kind != WordSegmentKind::Punctuation)
                return direction == SelectionDirection::Backward
                    ? canonicalize_backward_word_location(*punctuation_end, alteration)
                    : punctuation_end;
            moved = true;
            if (kind == WordSegmentKind::Separator)
                continue;
            if (kind == WordSegmentKind::Word)
                return direction == SelectionDirection::Backward
                    ? canonicalize_backward_word_location(location, alteration)
                    : Optional<CaretLocation> { location };

            punctuation_end = location;
            if ((direction == SelectionDirection::Forward && location.offset != text->length())
                || (direction == SelectionDirection::Backward && location.offset != 0))
                return direction == SelectionDirection::Backward
                    ? canonicalize_backward_word_location(location, alteration)
                    : Optional<CaretLocation> { location };
        }

        if (!editing_host)
            return moved ? Optional<CaretLocation> { location } : Optional<CaretLocation> {};
        auto adjacent = adjacent_caret_host(*text, *editing_host, direction);
        if (!adjacent)
            return moved ? Optional<CaretLocation> { location } : Optional<CaretLocation> {};
        if (is_atomic_inline_caret_host(*adjacent)) {
            if (punctuation_end.has_value())
                return direction == SelectionDirection::Backward
                    ? canonicalize_backward_word_location(*punctuation_end, alteration)
                    : punctuation_end;
            return direction == SelectionDirection::Forward
                ? location_after_atomic_inline(*adjacent)
                : location_before_atomic_inline(*adjacent, alteration);
        }

        auto* adjacent_text = as_if<DOM::Text>(*adjacent);
        if (!adjacent_text)
            return moved ? Optional<CaretLocation> { location } : move_to_adjacent_caret_host(location, direction);
        auto shares_inline_context = direction == SelectionDirection::Forward
            ? boundary_visual_lines_share_inline_context(*text, *adjacent_text)
            : boundary_visual_lines_share_inline_context(*adjacent_text, *text);
        if (!shares_inline_context)
            return moved ? Optional<CaretLocation> { location } : move_to_adjacent_caret_host(location, direction);

        text = adjacent_text;
        location = { *text, direction == SelectionDirection::Forward ? 0u : text->length(), TextAffinity::Downstream };
    }
}

Optional<CaretLocation> CaretNavigator::move_to_editing_host_boundary(CaretLocation const& location, SelectionDirection direction)
{
    // INTEROP: Chromium and WebKit constrain "start/end of document" commands to the active editing host. Letting the
    // traversal escape the host can create a caret before the first editable position or in unrelated page content.
    auto editing_host = location.node->editing_host();
    if (!editing_host)
        return {};

    m_document->update_layout_if_needed_for_node(*editing_host, DOM::UpdateLayoutReason::CursorLineNavigation);

    auto target = adjacent_caret_host(*editing_host, *editing_host, SelectionDirection::Forward);
    if (!target)
        return {};
    if (direction == SelectionDirection::Forward) {
        while (auto next_target = adjacent_caret_host(*target, *editing_host, SelectionDirection::Forward))
            target = next_target;
    }

    if (auto* text = as_if<DOM::Text>(*target))
        return text_edge_location(*text, direction == SelectionDirection::Forward);

    if (is_atomic_inline_caret_host(*target) && target->parent())
        return CaretLocation { *target->parent(), target->index() + (direction == SelectionDirection::Forward ? 1 : 0), TextAffinity::Downstream };
    if (is<HTML::HTMLBRElement>(*target) && target->parent())
        return CaretLocation { *target->parent(), target->index(), TextAffinity::Downstream };
    return CaretLocation { *target, 0, TextAffinity::Downstream };
}

Optional<CaretLocation> CaretNavigator::first_position_on_another_line(CaretLocation const& location, DOM::Node& editing_host, SelectionDirection direction, InlineLine const* line)
{
    Layout::ForcedReadScope read { *m_document };
    for (auto host = adjacent_caret_host(navigation_origin(location, direction), editing_host, direction); host; host = adjacent_caret_host(*host, editing_host, direction)) {
        Optional<CaretLocation> candidate;
        if (auto* text = as_if<DOM::Text>(*host)) {
            candidate = text_edge_location(*text, direction == SelectionDirection::Backward);
        } else if (is_empty_inline_block(*host)) {
            candidate = CaretLocation { *host, 0 };
        } else if (is_atomic_inline_caret_host(*host)) {
            candidate = CaretLocation { *host->parent(), host->index() + (direction == SelectionDirection::Backward ? 1 : 0) };
        } else if (is<HTML::HTMLBRElement>(*host) && host->parent()) {
            candidate = CaretLocation { *host->parent(), host->index() };
        } else {
            candidate = CaretLocation { *host, 0 };
        }
        if (!candidate.has_value())
            continue;
        if (line) {
            if (auto candidate_line = caret_line(*m_document, read, *candidate); candidate_line.has_value() && is_same_line(*m_document, *candidate_line, *line))
                continue;
        }
        return candidate;
    }
    return {};
}

Optional<CaretLocation> CaretNavigator::position_on_adjacent_line(CaretLocation const& location, SelectionDirection direction, CSSPixels inline_coordinate)
{
    auto editing_host = location.node->editing_host();
    if (!editing_host)
        return {};
    Layout::ForcedReadScope read { *m_document };
    m_document->update_layout_if_needed_for_node(*editing_host, DOM::UpdateLayoutReason::CursorLineNavigation);
    auto* host = m_document->layout_node_arena().host();

    // The next line in the line root of the caret's line, which runs on through the fragmentainers the root spans, or
    // else the line of the next caret position in tree order, as editing navigates lines.
    auto line = caret_line(*m_document, read, location);
    Optional<InlineLine> target_line;
    if (line.has_value()) {
        auto adjacent_line = Layout::RustFFI::render_state_adjacent_caret_line(host, *line, direction == SelectionDirection::Forward);
        if (adjacent_line.root.index != Compositing::RustFFI::INVALID_NODE_SLOT_INDEX)
            target_line = adjacent_line;
    }
    if (!target_line.has_value()) {
        auto candidate = first_position_on_another_line(location, *editing_host, direction, line.has_value() ? &*line : nullptr);
        if (!candidate.has_value())
            return {};
        target_line = caret_line(*m_document, read, *candidate);
        if (!target_line.has_value())
            return candidate;
    }
    auto position = caret_location_of(*m_document, read, Layout::RustFFI::render_state_caret_position_at_inline_coordinate(host, *target_line, inline_coordinate));
    if (!position.has_value() || !editing_host->is_inclusive_ancestor_of(position->node))
        return {};
    return position;
}

Optional<CaretLocation> CaretNavigator::move_by_page(CaretLocation const& location, SelectionDirection direction, CSSPixels inline_coordinate)
{
    auto editing_host = location.node->editing_host();
    if (!editing_host)
        return {};

    Layout::ForcedReadScope read { *m_document };
    m_document->update_layout_if_needed_for_node(*editing_host, DOM::UpdateLayoutReason::CursorLineNavigation);
    auto const* editing_host_layout_node = editing_host->layout_node(read);
    if (!editing_host_layout_node || !Painting::has_committed_box(*editing_host_layout_node))
        return {};
    auto window = m_document->window();
    if (!window)
        return {};

    // INTEROP: A page is the smaller of the editing host and viewport heights, less 12.5% (or on macOS, at least 40 CSS
    //          pixels) of overlap. A page move walks lines until the next one would be farther away than that.
    auto page_height = min(Painting::absolute_padding_box_rect(*editing_host_layout_node).height().to_int(), window->inner_height());
    if (page_height <= 0)
        return {};
    auto page_distance = static_cast<i32>(page_height * 0.875);
#if defined(AK_OS_MACOS)
    page_distance = max(page_distance, page_height - 40);
#endif
    page_distance = max(page_distance, 1);

    auto* host = m_document->layout_node_arena().host();
    auto start_line = caret_line(*m_document, read, location);
    if (!start_line.has_value())
        return {};
    CSSPixels start_block_coordinate;
    Layout::RustFFI::render_state_caret_line_block_coordinate(host, *start_line, &start_block_coordinate);

    auto current = location;
    Optional<CaretLocation> result;
    for (u32 iteration = 0; iteration < 1024; ++iteration) {
        auto next = position_on_adjacent_line(current, direction, inline_coordinate);
        if (!next.has_value())
            next = move_to_editing_host_boundary(current, direction);
        if (!next.has_value() || (next->node == current.node && next->offset == current.offset))
            break;
        auto next_line = caret_line(*m_document, read, *next);
        if (!next_line.has_value())
            break;
        CSSPixels next_block_coordinate;
        Layout::RustFFI::render_state_caret_line_block_coordinate(host, *next_line, &next_block_coordinate);
        auto block_distance = next_block_coordinate > start_block_coordinate
            ? next_block_coordinate - start_block_coordinate
            : start_block_coordinate - next_block_coordinate;
        if (block_distance > CSSPixels(page_distance))
            break;
        current = *next;
        result = next;
    }
    return result;
}

Optional<CaretLocation> CaretNavigator::move_to_paragraph_boundary(CaretLocation const& location, SelectionDirection direction)
{
    auto editing_host = location.node->editing_host();
    if (!editing_host)
        return {};
    m_document->update_layout_if_needed_for_node(*editing_host, DOM::UpdateLayoutReason::CursorLineNavigation);
    auto current = canonical_location_for_editing(location);
    Layout::ForcedReadScope read { *m_document };
    for (;;) {
        auto* text = as_if<DOM::Text>(*current.node);
        if (!text)
            return current;
        auto data = text->data().utf16_view();
        auto offset = min(current.offset, data.length_in_code_units());
        auto white_space = Editing::resolved_keyword(*text, CSS::PropertyID::WhiteSpaceCollapse);
        auto preserves_newlines = white_space.has_value() && first_is_one_of(*white_space, CSS::Keyword::Preserve, CSS::Keyword::PreserveBreaks, CSS::Keyword::BreakSpaces);
        if (direction == SelectionDirection::Backward) {
            while (offset > 0 && (!preserves_newlines || data.code_unit_at(offset - 1) != '\n'))
                --offset;
        } else {
            while (offset < data.length_in_code_units() && (!preserves_newlines || data.code_unit_at(offset) != '\n'))
                ++offset;
        }
        current = { *text, offset, TextAffinity::Downstream };
        if ((direction == SelectionDirection::Backward && offset > 0) || (direction == SelectionDirection::Forward && offset < data.length_in_code_units()))
            return current;

        auto* node = static_cast<DOM::Node*>(text);
        GC::Ptr<DOM::Text> next_text;
        while (node) {
            node = direction == SelectionDirection::Forward ? node->next_in_pre_order(editing_host.ptr()) : node->previous_in_pre_order();
            if (!node || node == editing_host.ptr() || !editing_host->is_inclusive_ancestor_of(*node))
                return current;
            if (node->is_inclusive_ancestor_of(*text))
                continue;
            if (is<HTML::HTMLBRElement>(*node) && node->layout_node(read))
                return current;
            if (is_empty_line_host(*node))
                return current;
            auto* candidate = as_if<DOM::Text>(*node);
            if (!candidate || !candidate->is_editable() || candidate->editing_host() != editing_host || !text_node_has_rendered_text(*candidate))
                continue;
            if (!boundary_visual_lines_share_inline_context(*text, *candidate))
                return current;
            next_text = candidate;
            break;
        }
        if (!next_text)
            return current;
        current = { *next_text, direction == SelectionDirection::Forward ? 0 : next_text->length(), TextAffinity::Downstream };
    }
}

Optional<CaretLocation> CaretNavigator::move(CaretLocation const& location, SelectionAlteration alteration, SelectionDirection direction, SelectionGranularity granularity, Optional<CSSPixels> preferred_inline_coordinate)
{
    auto* text = as_if<DOM::Text>(*location.node);

    if (granularity == SelectionGranularity::DocumentBoundary)
        return move_to_editing_host_boundary(location, direction);

    if (granularity == SelectionGranularity::ParagraphBoundary)
        return move_to_paragraph_boundary(location, direction);

    if (granularity == SelectionGranularity::Page) {
        if (!preferred_inline_coordinate.has_value())
            return {};
        return move_by_page(location, direction, *preferred_inline_coordinate);
    }

    if (granularity == SelectionGranularity::Character) {
        if (text) {
            if (auto position = location_after_character_step(*text, location.offset, location.affinity, direction); position.has_value())
                return position;
        }
        auto adjacent = move_to_adjacent_caret_host(location, direction);
        if (!adjacent.has_value())
            return {};
        auto* adjacent_text = as_if<DOM::Text>(*adjacent->node);
        // INTEROP: The parent boundary after an image is already the start of its following text. Advancing into
        //          that text must cross a character instead of stopping again at the same rendered position.
        if (!text && adjacent_text && direction == SelectionDirection::Forward && location.offset > 0
            && location.node->child_at_index(location.offset) == adjacent_text) {
            auto* previous = location.node->child_at_index(location.offset - 1);
            if (previous && is_atomic_inline_caret_host(*previous)) {
                if (auto position = location_after_character_step(*adjacent_text, adjacent->offset, adjacent->affinity, direction); position.has_value())
                    return position;
            }
        }
        if (text && adjacent_text) {
            auto shares_line = direction == SelectionDirection::Forward
                ? boundary_visual_lines_share_line(*text, *adjacent_text)
                : boundary_visual_lines_share_line(*adjacent_text, *text);
            if (shares_line) {
                if (auto position = location_after_character_step(*adjacent_text, adjacent->offset, adjacent->affinity, direction); position.has_value())
                    return position;
            }
        }
        return adjacent;
    }

    if (granularity == SelectionGranularity::Word) {
        return move_by_word(location, alteration, direction);
    }

    if (granularity == SelectionGranularity::LineBoundary || granularity == SelectionGranularity::PhysicalLineBoundary) {
        Layout::ForcedReadScope read { *m_document };
        m_document->update_layout_if_needed_for_node(location.node, DOM::UpdateLayoutReason::CursorLineNavigation);
        auto line = caret_line(*m_document, read, location);
        if (!line.has_value())
            return {};
        auto edge = direction == SelectionDirection::Forward ? Layout::RustFFI::LineEdge::End : Layout::RustFFI::LineEdge::Start;
        if (granularity == SelectionGranularity::PhysicalLineBoundary)
            edge = direction == SelectionDirection::Forward ? Layout::RustFFI::LineEdge::Right : Layout::RustFFI::LineEdge::Left;
        return caret_location_of(*m_document, read, Layout::RustFFI::render_state_caret_position_at_line_edge(m_document->layout_node_arena().host(), *line, edge, location.node->is_editable_or_editing_host()));
    }

    VERIFY(granularity == SelectionGranularity::Line);
    auto editing_host = location.node->editing_host();
    if (!editing_host || !preferred_inline_coordinate.has_value())
        return {};
    // INTEROP: With no line in the requested direction, the caret moves to the start or end of the editing host.
    auto position = position_on_adjacent_line(location, direction, *preferred_inline_coordinate);
    if (!position.has_value())
        position = move_to_editing_host_boundary(location, direction);
    if (!position.has_value())
        return {};

    auto destination = *position;
    // Point-to-caret resolution naturally returns the parent boundary before an atomic inline. For a collapsed caret,
    // prefer the visually equivalent preceding text edge so horizontal movement does not revisit the same position.
    if (alteration == SelectionAlteration::Move && !is<DOM::Text>(*destination.node)) {
        auto* child_after = destination.node->child_at_index(destination.offset);
        if (child_after && is_atomic_inline_caret_host(*child_after)) {
            auto previous = adjacent_caret_host(*child_after, *editing_host, SelectionDirection::Backward);
            if (auto* previous_text = as_if<DOM::Text>(previous.ptr()))
                destination = { *previous_text, previous_text->length(), TextAffinity::Downstream };
        }
    }
    auto* destination_text = as_if<DOM::Text>(*destination.node);
    if (alteration == SelectionAlteration::Extend && destination_text && direction == SelectionDirection::Forward && destination.offset == 0) {
        auto origin_block = Editing::block_node_of_node(location.node);
        auto destination_block = Editing::block_node_of_node(*destination_text);
        if (origin_block && destination_block && origin_block != destination_block)
            destination = canonical_location_at_start_of_inline_content(*destination_text, *destination_block);
    }
    return destination;
}

}
