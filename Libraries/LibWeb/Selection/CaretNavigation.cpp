/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Selection/CaretNavigation.h>
#include <LibWeb/VisualLines.h>

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
    for (auto const& line : collect_visual_lines(text)) {
        if (!line.fragments.is_empty())
            return true;
    }
    return false;
}

// A <br> that renders an empty line hosts a caret position on its parent, at its child index. This covers a leading
// <br> in a paragraph with text after it, and the lines between consecutive <br>s.
static bool is_empty_line_break(DOM::Node& node)
{
    auto* br = as_if<HTML::HTMLBRElement>(node);
    return br && br->is_editable() && br->represents_empty_line();
}

// A block-level element that renders no text but hosts an empty line where the caret can sit, such as `<p><br></p>`.
static bool is_empty_line_host(DOM::Node& node)
{
    auto* element = as_if<DOM::Element>(node);
    if (!element || !element->is_editable())
        return false;
    auto element_paintable = element->unsafe_paintable();
    auto* paintable = as_if<Painting::PaintableWithLines>(element_paintable.ptr());
    if (!paintable || paintable->layout_node().display().is_inline_outside())
        return false;

    bool has_rendered_content = false;
    element->for_each_in_subtree([&](DOM::Node& descendant) {
        if (auto* text = as_if<DOM::Text>(descendant); text && text_node_has_rendered_text(*text)) {
            has_rendered_content = true;
            return TraversalDecision::Break;
        }
        auto const* layout_node = descendant.layout_node();
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
    if (is<HTML::HTMLBRElement>(node))
        return false;
    auto const* layout_node = node.layout_node();
    return node.is_editable() && node.parent() && layout_node && layout_node->is_atomic_inline();
}

static bool boundary_visual_lines_share_line(DOM::Text const& before, DOM::Text const& after)
{
    auto before_lines = collect_visual_lines(before);
    auto after_lines = collect_visual_lines(after);
    if (before_lines.is_empty() || before_lines.last().fragments.is_empty() || after_lines.is_empty() || after_lines.first().fragments.is_empty())
        return false;

    auto const& before_fragment = *before_lines.last().fragments.last();
    auto const& after_fragment = *after_lines.first().fragments.first();
    return &before_fragment.paintable_with_lines() == &after_fragment.paintable_with_lines()
        && before_fragment.line_index() == after_fragment.line_index();
}

static bool boundary_visual_lines_share_inline_context(DOM::Text const& before, DOM::Text const& after)
{
    auto before_lines = collect_visual_lines(before);
    auto after_lines = collect_visual_lines(after);
    if (before_lines.is_empty() || before_lines.last().fragments.is_empty() || after_lines.is_empty() || after_lines.first().fragments.is_empty())
        return false;

    auto const& before_fragment = *before_lines.last().fragments.last();
    auto const& after_fragment = *after_lines.first().fragments.first();
    return &before_fragment.paintable_with_lines() == &after_fragment.paintable_with_lines();
}

// Convert a text-edge destination to the equivalent boundary in its block. Chromium exposes this form when a vertical
// extension crosses a block boundary, which keeps the selected DOM range aligned with the rendered block contents.
static CaretLocation canonical_location_at_outer_edge_of_inline_content(DOM::Text& text, DOM::Node& block, SelectionDirection direction)
{
    DOM::Node* node = &text;
    while (auto* parent = node->parent()) {
        auto offset = direction == SelectionDirection::Forward ? node->index() : node->index() + 1;
        if (parent == &block)
            return { *parent, offset, TextAffinity::Downstream };
        node = parent;
    }
    return { text, direction == SelectionDirection::Forward ? 0u : text.length(), TextAffinity::Downstream };
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
    m_document->update_layout_if_needed_for_node(location.node, DOM::UpdateLayoutReason::CursorLineNavigation);
    if (auto* text = as_if<DOM::Text>(*location.node))
        return cursor_inline_coordinate(*text, location.offset, location.affinity);

    if (location.offset > 0) {
        auto* child_before = location.node->child_at_index(location.offset - 1);
        if (child_before && is_atomic_inline_caret_host(*child_before)) {
            // A parent boundary after an atomic inline can paint at the same position as the end of the preceding text.
            // Use that text position when available so a vertical round trip preserves the visually chosen column.
            auto editing_host = location.node->editing_host();
            auto previous = editing_host ? adjacent_caret_host(*child_before, *editing_host, SelectionDirection::Backward) : nullptr;
            if (auto* previous_text = as_if<DOM::Text>(previous.ptr()))
                return cursor_inline_coordinate(*previous_text, previous_text->length(), TextAffinity::Downstream);
        }
    }

    auto paintable = location.node->paintable();
    return m_document->current_caret_rect().map([&](auto const& rect) {
        if (paintable && paintable->computed_values().writing_mode() != CSS::WritingMode::HorizontalTb)
            return rect.y();
        return rect.x();
    });
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
    auto affinity = TextAffinity::Downstream;

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

    if (auto* text = as_if<DOM::Text>(*target)) {
        auto position = direction == SelectionDirection::Forward
            ? cursor_position_at_visual_start(*text)
            : cursor_position_at_visual_end(*text);
        if (!position.has_value())
            return {};
        offset = position->offset;
        affinity = position->affinity;
    }

    return CaretLocation { *target, offset, affinity };
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
    if (is_formatting_whitespace(*node)) {
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
        } else if (before && can_descend_into(*before)) {
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
        auto position = cursor_position_at_visual_end(*previous_text);
        if (position.has_value())
            return CaretLocation { *previous_text, position->offset, position->affinity };
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

    if (auto* text = as_if<DOM::Text>(*target)) {
        auto position = direction == SelectionDirection::Forward
            ? cursor_position_at_visual_end(*text)
            : cursor_position_at_visual_start(*text);
        if (!position.has_value())
            return {};
        return CaretLocation { *text, position->offset, position->affinity };
    }

    if (is_atomic_inline_caret_host(*target) && target->parent())
        return CaretLocation { *target->parent(), target->index() + (direction == SelectionDirection::Forward ? 1 : 0), TextAffinity::Downstream };
    if (is<HTML::HTMLBRElement>(*target) && target->parent())
        return CaretLocation { *target->parent(), target->index(), TextAffinity::Downstream };
    return CaretLocation { *target, 0, TextAffinity::Downstream };
}

Optional<CaretLocation> CaretNavigator::move_by_page(CaretLocation const& location, SelectionDirection direction, CSSPixels inline_coordinate)
{
    auto editing_host = location.node->editing_host();
    if (!editing_host)
        return {};

    m_document->update_layout_if_needed_for_node(*editing_host, DOM::UpdateLayoutReason::CursorLineNavigation);
    auto editing_host_paintable = editing_host->paintable();
    if (!editing_host_paintable)
        return {};
    auto window = m_document->window();
    if (!window)
        return {};

    // INTEROP: Chromium defines a page as the smaller of the editing host and viewport heights, then leaves either
    // 12.5% or (on macOS) at least 40 CSS pixels of overlap. It walks visual lines until the next line would exceed
    // that distance, rather than mapping the target coordinate directly back into the DOM.
    auto page_height = min(editing_host_paintable->absolute_padding_box_rect().height().to_int(), window->inner_height());
    if (page_height <= 0)
        return {};
    auto page_distance = static_cast<i32>(page_height * 0.875);
#if defined(AK_OS_MACOS)
    page_distance = max(page_distance, page_height - 40);
#endif
    page_distance = max(page_distance, 1);

    auto start_block_coordinate = m_document->caret_line_block_coordinate(location.node, location.offset, location.affinity);
    if (!start_block_coordinate.has_value())
        return {};

    auto current = location;
    Optional<CaretLocation> result;
    auto line_direction = direction == SelectionDirection::Forward ? Painting::CaretLineDirection::Next : Painting::CaretLineDirection::Previous;
    for (u32 iteration = 0; iteration < 1024; ++iteration) {
        auto position = m_document->caret_position_on_adjacent_line(current.node, current.offset, current.affinity, line_direction, inline_coordinate, *editing_host);
        Optional<CaretLocation> next;
        if (position.has_value()) {
            next = CaretLocation { position->boundary.node, position->boundary.offset, position->affinity };
        } else {
            // PreviousLinePosition and NextLinePosition in Chromium expose the editing-host boundary as a final stop
            // on the first or last visual line. Preserve that stop when it still fits within this page movement.
            next = move_to_editing_host_boundary(current, direction);
            if (!next.has_value() || (next->node == current.node && next->offset == current.offset))
                break;
        }
        auto next_block_coordinate = m_document->caret_line_block_coordinate(next->node, next->offset, next->affinity);
        if (!next_block_coordinate.has_value())
            break;
        auto block_distance = *next_block_coordinate > *start_block_coordinate
            ? *next_block_coordinate - *start_block_coordinate
            : *start_block_coordinate - *next_block_coordinate;
        if (block_distance > CSSPixels(page_distance))
            break;
        current = *next;
        result = next;
    }
    return result;
}

Optional<CaretLocation> CaretNavigator::move(CaretLocation const& location, SelectionAlteration alteration, SelectionDirection direction, SelectionGranularity granularity, Optional<CSSPixels> preferred_inline_coordinate)
{
    auto* text = as_if<DOM::Text>(*location.node);

    if (granularity == SelectionGranularity::DocumentBoundary)
        return move_to_editing_host_boundary(location, direction);

    if (granularity == SelectionGranularity::Page) {
        if (!preferred_inline_coordinate.has_value())
            return {};
        return move_by_page(location, direction, *preferred_inline_coordinate);
    }

    if (granularity == SelectionGranularity::Character) {
        if (text) {
            auto position = direction == SelectionDirection::Forward
                ? compute_cursor_position_on_next_character(*text, location.offset, location.affinity)
                : compute_cursor_position_on_previous_character(*text, location.offset, location.affinity);
            if (position.has_value())
                return CaretLocation { *text, position->offset, position->affinity };
        }
        auto adjacent = move_to_adjacent_caret_host(location, direction);
        if (!adjacent.has_value())
            return {};
        auto* adjacent_text = as_if<DOM::Text>(*adjacent->node);
        if (text && adjacent_text) {
            auto shares_line = direction == SelectionDirection::Forward
                ? boundary_visual_lines_share_line(*text, *adjacent_text)
                : boundary_visual_lines_share_line(*adjacent_text, *text);
            if (shares_line) {
                auto position = direction == SelectionDirection::Forward
                    ? compute_cursor_position_on_next_character(*adjacent_text, adjacent->offset, adjacent->affinity)
                    : compute_cursor_position_on_previous_character(*adjacent_text, adjacent->offset, adjacent->affinity);
                if (position.has_value())
                    return CaretLocation { *adjacent_text, position->offset, position->affinity };
            }
        }
        return adjacent;
    }

    if (granularity == SelectionGranularity::Word) {
        return move_by_word(location, alteration, direction);
    }

    if (granularity == SelectionGranularity::LineBoundary) {
        m_document->update_layout_if_needed_for_node(location.node, DOM::UpdateLayoutReason::CursorLineNavigation);
        auto edge = direction == SelectionDirection::Forward ? Painting::CaretLineEdge::End : Painting::CaretLineEdge::Start;
        auto position = m_document->caret_position_at_line_edge(location.node, location.offset, location.affinity, edge);
        if (!position.has_value())
            return {};
        return CaretLocation { position->boundary.node, position->boundary.offset, position->affinity };
    }

    VERIFY(granularity == SelectionGranularity::Line);
    auto line_origin = location;
    // Resolve the parent boundary after an atomic inline to its visually equivalent text edge before asking painting
    // which line contains the caret. This avoids selecting the atomic inline's other side as the current line.
    if (!text && location.offset > 0) {
        auto* child_before = location.node->child_at_index(location.offset - 1);
        if (child_before && is_atomic_inline_caret_host(*child_before)) {
            auto editing_host = location.node->editing_host();
            auto previous = editing_host ? adjacent_caret_host(*child_before, *editing_host, SelectionDirection::Backward) : nullptr;
            if (auto* previous_text = as_if<DOM::Text>(previous.ptr())) {
                line_origin = { *previous_text, previous_text->length(), TextAffinity::Downstream };
                text = previous_text;
            }
        }
    }

    auto editing_host = line_origin.node->editing_host();
    if (!editing_host || !preferred_inline_coordinate.has_value())
        return {};
    m_document->update_layout_if_needed_for_node(line_origin.node, DOM::UpdateLayoutReason::CursorLineNavigation);
    auto line_direction = direction == SelectionDirection::Forward ? Painting::CaretLineDirection::Next : Painting::CaretLineDirection::Previous;
    auto position = m_document->caret_position_on_adjacent_line(line_origin.node, line_origin.offset, line_origin.affinity, line_direction, *preferred_inline_coordinate, *editing_host);
    if (!position.has_value()) {
        // INTEROP: Chromium and WebKit move to the current line's directional edge when there is no visual line in the
        // requested direction. This makes Up and Down useful at the first and final lines instead of becoming no-ops.
        auto edge = direction == SelectionDirection::Forward ? Painting::CaretLineEdge::End : Painting::CaretLineEdge::Start;
        position = m_document->caret_position_at_line_edge(line_origin.node, line_origin.offset, line_origin.affinity, edge);
        if (!position.has_value())
            return {};
    }

    CaretLocation destination { position->boundary.node, position->boundary.offset, position->affinity };
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
    if (alteration == SelectionAlteration::Extend && destination_text
        && ((direction == SelectionDirection::Forward && destination.offset == 0)
            || (direction == SelectionDirection::Backward && destination.offset == destination_text->length()))) {
        auto origin_block = Editing::block_node_of_node(line_origin.node);
        auto destination_block = Editing::block_node_of_node(*destination_text);
        if (origin_block && destination_block && origin_block != destination_block)
            destination = canonical_location_at_outer_edge_of_inline_content(*destination_text, *destination_block, direction);
    }
    return destination;
}

}
