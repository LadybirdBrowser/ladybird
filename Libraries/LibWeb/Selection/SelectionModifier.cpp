/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/Selection/SelectionModifier.h>
#include <LibWeb/VisualLines.h>

namespace Web::Selection {

// A DOM boundary is not sufficient to identify a visual caret position. At a soft wrap, the same node and offset can
// be painted at the end of one line or the start of the next, so affinity travels with every computed destination.
struct CaretLocation {
    GC::Ref<DOM::Node> node;
    size_t offset { 0 };
    TextAffinity affinity { TextAffinity::Downstream };
};

enum class CaretEntryMode : u8 {
    LineEdge,
    ClosestToInlineCoordinate,
};

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

// Turns a movement request into a CaretLocation without mutating Selection. This allows DOM traversal and rendered
// geometry queries to cooperate without exposing intermediate selection states to script or painting.
class CaretNavigator {
public:
    explicit CaretNavigator(DOM::Document& document)
        : m_document(document)
    {
    }

    Optional<CaretLocation> move(CaretLocation const&, SelectionAlteration, SelectionDirection, SelectionGranularity, Optional<CSSPixels> preferred_inline_coordinate = {});
    Optional<CaretLocation> canonical_location_for_extension(CaretLocation const&, SelectionDirection);

private:
    Optional<CaretLocation> move_to_adjacent_caret_host(CaretLocation const&, SelectionDirection, CaretEntryMode, Optional<CSSPixels>);
    Optional<CaretLocation> move_to_editing_host_boundary(CaretLocation const&, SelectionDirection);
    Optional<CaretLocation> move_by_word(CaretLocation const&, SelectionAlteration, SelectionDirection);
    GC::Ptr<DOM::Node> adjacent_caret_host(DOM::Node&, DOM::Node& editing_host, SelectionDirection);
    Optional<CaretLocation> location_before_atomic_inline(DOM::Node&, SelectionAlteration);
    Optional<CaretLocation> location_after_atomic_inline(DOM::Node&);
    Optional<CaretLocation> canonicalize_backward_word_location(CaretLocation const&, SelectionAlteration);
    static DOM::Node& navigation_origin(CaretLocation const&, SelectionDirection);

    GC::Ref<DOM::Document> m_document;
};

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

Optional<CaretLocation> CaretNavigator::move_to_adjacent_caret_host(CaretLocation const& location, SelectionDirection direction, CaretEntryMode entry_mode, Optional<CSSPixels> inline_coordinate)
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
    if (entry_mode == CaretEntryMode::ClosestToInlineCoordinate) {
        while (target && is_atomic_inline_caret_host(*target)) {
            target = adjacent_caret_host(*target, *editing_host, direction);
            target_is_adjacent_child = false;
        }
    }
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
        Optional<CursorLinePosition> position;
        if (entry_mode == CaretEntryMode::LineEdge) {
            position = direction == SelectionDirection::Forward
                ? cursor_position_at_visual_start(*text)
                : cursor_position_at_visual_end(*text);
        } else {
            position = direction == SelectionDirection::Forward
                ? cursor_position_on_first_line_closest_to(*text, inline_coordinate)
                : cursor_position_on_last_line_closest_to(*text, inline_coordinate);
        }
        if (!position.has_value())
            return {};
        offset = position->offset;
        affinity = position->affinity;
    }

    return CaretLocation { *target, offset, affinity };
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
            return move_to_adjacent_caret_host(location, direction, CaretEntryMode::LineEdge, {});
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
            return moved ? Optional<CaretLocation> { location } : move_to_adjacent_caret_host(location, direction, CaretEntryMode::LineEdge, {});
        auto shares_inline_context = direction == SelectionDirection::Forward
            ? boundary_visual_lines_share_inline_context(*text, *adjacent_text)
            : boundary_visual_lines_share_inline_context(*adjacent_text, *text);
        if (!shares_inline_context)
            return moved ? Optional<CaretLocation> { location } : move_to_adjacent_caret_host(location, direction, CaretEntryMode::LineEdge, {});

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

    if (is<HTML::HTMLBRElement>(*target) && target->parent())
        return CaretLocation { *target->parent(), target->index(), TextAffinity::Downstream };
    return CaretLocation { *target, 0, TextAffinity::Downstream };
}

Optional<CaretLocation> CaretNavigator::move(CaretLocation const& location, SelectionAlteration alteration, SelectionDirection direction, SelectionGranularity granularity, Optional<CSSPixels> preferred_inline_coordinate)
{
    auto* text = as_if<DOM::Text>(*location.node);

    if (granularity == SelectionGranularity::DocumentBoundary)
        return move_to_editing_host_boundary(location, direction);

    if (granularity == SelectionGranularity::Character) {
        if (text) {
            auto position = direction == SelectionDirection::Forward
                ? compute_cursor_position_on_next_character(*text, location.offset, location.affinity)
                : compute_cursor_position_on_previous_character(*text, location.offset, location.affinity);
            if (position.has_value())
                return CaretLocation { *text, position->offset, position->affinity };
        }
        auto adjacent = move_to_adjacent_caret_host(location, direction, CaretEntryMode::LineEdge, {});
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
    if (!text)
        return move_to_adjacent_caret_host(location, direction, CaretEntryMode::ClosestToInlineCoordinate, preferred_inline_coordinate);

    auto is_on_edge_line = direction == SelectionDirection::Forward
        ? offset_is_on_last_visual_line(*text, location.offset, location.affinity)
        : offset_is_on_first_visual_line(*text, location.offset, location.affinity);
    if (is_on_edge_line) {
        auto inline_coordinate = preferred_inline_coordinate;
        if (!inline_coordinate.has_value())
            inline_coordinate = cursor_inline_coordinate(*text, location.offset, location.affinity);
        if (auto result = move_to_adjacent_caret_host(location, direction, CaretEntryMode::ClosestToInlineCoordinate, inline_coordinate); result.has_value())
            return result;
    }

    auto position = direction == SelectionDirection::Forward
        ? compute_cursor_position_on_next_line(*text, location.offset, location.affinity, preferred_inline_coordinate)
        : compute_cursor_position_on_previous_line(*text, location.offset, location.affinity, preferred_inline_coordinate);
    if (!position.has_value())
        return {};
    return CaretLocation { *text, position->offset, position->affinity };
}

void SelectionModifier::modify(SelectionAlteration alteration, SelectionDirection direction, SelectionGranularity granularity)
{
    auto range = m_selection->range();
    if (!range)
        return;

    // INTEROP: Major engines remember a preferred inline-axis position across consecutive Up/Down operations. Without
    // this state, traversing a short line permanently shifts the caret column for every subsequent line.
    auto preferred_inline_coordinate = m_selection->m_preferred_inline_coordinate;
    if (granularity == SelectionGranularity::Line && !preferred_inline_coordinate.has_value()) {
        if (auto* text = as_if<DOM::Text>(m_selection->focus_node().ptr()))
            preferred_inline_coordinate = cursor_inline_coordinate(*text, m_selection->focus_offset(), m_selection->focus_affinity());
    } else if (granularity != SelectionGranularity::Line) {
        m_selection->m_preferred_inline_coordinate.clear();
    }

    Optional<CaretLocation> destination;
    Optional<CaretLocation> canonical_anchor;
    if (alteration == SelectionAlteration::Move && granularity == SelectionGranularity::Character && !m_selection->is_collapsed()) {
        auto boundary = direction == SelectionDirection::Forward ? range->end() : range->start();
        destination = CaretLocation { boundary.node, boundary.offset, TextAffinity::Downstream };
    } else if (auto focus = m_selection->focus_node()) {
        CaretNavigator navigator(*m_selection->document());
        CaretLocation origin { *focus, m_selection->focus_offset(), m_selection->focus_affinity() };
        if (alteration == SelectionAlteration::Extend && m_selection->is_collapsed())
            canonical_anchor = navigator.canonical_location_for_extension(origin, direction);
        destination = navigator.move(origin, alteration, direction, granularity, preferred_inline_coordinate);
    }

    if (!destination.has_value())
        return;

    // Apply exactly one selection mutation after navigation has produced a complete destination. In particular, do
    // not use Selection itself as scratch state while walking across DOM nodes or painted lines.
    if (alteration == SelectionAlteration::Move) {
        MUST(m_selection->collapse(destination->node, destination->offset));
        m_selection->document()->reset_cursor_blink_cycle();
    } else {
        GC::Ptr<DOM::Node> anchor = canonical_anchor.has_value() ? canonical_anchor->node : m_selection->anchor_node();
        if (!anchor)
            return;
        auto anchor_offset = canonical_anchor.has_value() ? canonical_anchor->offset : m_selection->anchor_offset();
        MUST(m_selection->set_base_and_extent(*anchor, anchor_offset, destination->node, destination->offset));
    }
    m_selection->set_focus_affinity(destination->affinity);
    if (granularity == SelectionGranularity::Line)
        m_selection->m_preferred_inline_coordinate = preferred_inline_coordinate;
    m_selection->document()->set_cursor_position_needs_repaint();
    m_selection->scroll_focus_into_view();
}

}
