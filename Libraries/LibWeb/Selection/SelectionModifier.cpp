/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

    bool has_rendered_text = false;
    element->for_each_in_subtree_of_type<DOM::Text>([&](auto& text) {
        if (!text_node_has_rendered_text(text))
            return TraversalDecision::Continue;
        has_rendered_text = true;
        return TraversalDecision::Break;
    });
    return !has_rendered_text;
}

// Turns a movement request into a CaretLocation without mutating Selection. This allows DOM traversal and rendered
// geometry queries to cooperate without exposing intermediate selection states to script or painting.
class CaretNavigator {
public:
    explicit CaretNavigator(DOM::Document& document)
        : m_document(document)
    {
    }

    Optional<CaretLocation> move(CaretLocation const&, SelectionDirection, SelectionGranularity, Optional<CSSPixels> preferred_inline_coordinate = {});

private:
    Optional<CaretLocation> move_to_adjacent_caret_host(CaretLocation const&, SelectionDirection, CaretEntryMode, Optional<CSSPixels>);
    Optional<CaretLocation> move_to_editing_host_boundary(CaretLocation const&, SelectionDirection);
    GC::Ptr<DOM::Node> adjacent_caret_host(DOM::Node&, DOM::Node& editing_host, SelectionDirection);
    static DOM::Node& navigation_origin(CaretLocation const&);

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
        if (is_empty_line_host(*node) || is_empty_line_break(*node))
            return node;
    }
    return nullptr;
}

DOM::Node& CaretNavigator::navigation_origin(CaretLocation const& location)
{
    if (auto* child = location.node->child_at_index(location.offset))
        return *child;
    return location.node;
}

Optional<CaretLocation> CaretNavigator::move_to_adjacent_caret_host(CaretLocation const& location, SelectionDirection direction, CaretEntryMode entry_mode, Optional<CSSPixels> inline_coordinate)
{
    auto editing_host = location.node->editing_host();
    if (!editing_host)
        return {};

    m_document->update_layout_if_needed_for_node(*editing_host, DOM::UpdateLayoutReason::CursorLineNavigation);

    auto target = adjacent_caret_host(navigation_origin(location), *editing_host, direction);
    if (!target)
        return {};

    size_t offset = 0;
    auto affinity = TextAffinity::Downstream;

    if (is<HTML::HTMLBRElement>(*target) && target->parent()) {
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

Optional<CaretLocation> CaretNavigator::move(CaretLocation const& location, SelectionDirection direction, SelectionGranularity granularity, Optional<CSSPixels> preferred_inline_coordinate)
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
        return move_to_adjacent_caret_host(location, direction, CaretEntryMode::LineEdge, {});
    }

    if (granularity == SelectionGranularity::Word) {
        if (!text)
            return move_to_adjacent_caret_host(location, direction, CaretEntryMode::LineEdge, {});

        auto offset = location.offset;
        while (true) {
            auto next_offset = direction == SelectionDirection::Forward
                ? text->word_segmenter().next_boundary(offset)
                : text->word_segmenter().previous_boundary(offset);
            if (!next_offset.has_value())
                break;
            auto word = direction == SelectionDirection::Forward
                ? text->data().substring_view(offset, *next_offset - offset)
                : text->data().substring_view(*next_offset, offset - *next_offset);
            offset = *next_offset;
            if (!Unicode::Segmenter::should_continue_beyond_word(word))
                break;
        }
        if (offset == location.offset)
            return {};
        return CaretLocation { *text, offset, TextAffinity::Downstream };
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
    if (alteration == SelectionAlteration::Move && granularity == SelectionGranularity::Character && !m_selection->is_collapsed()) {
        auto boundary = direction == SelectionDirection::Forward ? range->end() : range->start();
        destination = CaretLocation { boundary.node, boundary.offset, TextAffinity::Downstream };
    } else if (auto focus = m_selection->focus_node()) {
        CaretNavigator navigator(*m_selection->document());
        destination = navigator.move({ *focus, m_selection->focus_offset(), m_selection->focus_affinity() }, direction, granularity, preferred_inline_coordinate);
    }

    if (!destination.has_value())
        return;

    // Apply exactly one selection mutation after navigation has produced a complete destination. In particular, do
    // not use Selection itself as scratch state while walking across DOM nodes or painted lines.
    if (alteration == SelectionAlteration::Move) {
        MUST(m_selection->collapse(destination->node, destination->offset));
        m_selection->document()->reset_cursor_blink_cycle();
    } else {
        auto anchor = m_selection->anchor_node();
        if (!anchor)
            return;
        MUST(m_selection->set_base_and_extent(*anchor, m_selection->anchor_offset(), destination->node, destination->offset));
    }
    m_selection->set_focus_affinity(destination->affinity);
    if (granularity == SelectionGranularity::Line)
        m_selection->m_preferred_inline_coordinate = preferred_inline_coordinate;
    m_selection->document()->set_cursor_position_needs_repaint();
    m_selection->scroll_focus_into_view();
}

}
