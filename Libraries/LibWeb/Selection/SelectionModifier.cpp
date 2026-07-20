/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/Editing/VisiblePosition.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/Selection/SelectionModifier.h>

namespace Web::Selection {

void SelectionModifier::modify(SelectionAlteration alteration, SelectionDirection direction, SelectionGranularity granularity)
{
    auto range = m_selection->range();
    if (!range)
        return;

    auto focus = m_selection->focus_node();
    if (!focus)
        return;
    auto origin = Editing::VisiblePosition::create(*m_selection->document(), { *focus, static_cast<WebIDL::UnsignedLong>(m_selection->focus_offset()) }, m_selection->focus_affinity());

    // INTEROP: Major engines remember a preferred inline-axis position across consecutive Up/Down operations. Without
    // this state, traversing a short line permanently shifts the caret column for every subsequent line.
    auto preferred_inline_coordinate = m_selection->m_preferred_inline_coordinate;
    auto is_vertical_movement = granularity == SelectionGranularity::Line || granularity == SelectionGranularity::Page;
    if (is_vertical_movement && !preferred_inline_coordinate.has_value())
        preferred_inline_coordinate = origin.inline_coordinate();
    else if (!is_vertical_movement)
        m_selection->m_preferred_inline_coordinate.clear();

    Optional<Editing::VisiblePosition> destination;
    Optional<DOM::BoundaryPoint> canonical_anchor;
    if (alteration == SelectionAlteration::Move && granularity == SelectionGranularity::Character && !m_selection->is_collapsed()) {
        auto boundary = direction == SelectionDirection::Forward ? range->end() : range->start();
        destination = Editing::VisiblePosition::create(*m_selection->document(), boundary);
    } else {
        if (alteration == SelectionAlteration::Extend && m_selection->is_collapsed())
            canonical_anchor = origin.canonical_boundary_for_extension(direction);
        destination = origin.move(alteration, direction, granularity, preferred_inline_coordinate);
    }

    if (!destination.has_value())
        return;

    auto destination_boundary = destination->boundary();
    // Apply exactly one selection mutation after navigation has produced a complete destination. In particular, do
    // not use Selection itself as scratch state while walking across DOM nodes or painted lines.
    if (alteration == SelectionAlteration::Move) {
        MUST(m_selection->collapse(destination_boundary.node, destination_boundary.offset));
        m_selection->document()->reset_cursor_blink_cycle();
    } else {
        GC::Ptr<DOM::Node> anchor = canonical_anchor.has_value() ? canonical_anchor->node : m_selection->anchor_node();
        if (!anchor)
            return;
        auto anchor_offset = canonical_anchor.has_value() ? canonical_anchor->offset : m_selection->anchor_offset();
        MUST(m_selection->set_base_and_extent(*anchor, anchor_offset, destination_boundary.node, destination_boundary.offset));
    }
    m_selection->set_focus_affinity(destination->affinity());
    if (is_vertical_movement)
        m_selection->m_preferred_inline_coordinate = preferred_inline_coordinate;
    m_selection->document()->set_cursor_position_needs_repaint();
    m_selection->scroll_focus_into_view();
}

void SelectionModifier::select_all()
{
    // INTEROP: Select All in an editing host uses its first and last rendered caret positions, rather than parent
    // boundaries around the host's child list. Reuse document-boundary navigation so keyboard movement and selection
    // expose the same DOM endpoints around styled text, empty blocks, and atomic inline content.
    auto focus = m_selection->focus_node();
    if (!focus)
        return;

    auto origin = Editing::VisiblePosition::create(*m_selection->document(), { *focus, static_cast<WebIDL::UnsignedLong>(m_selection->focus_offset()) }, m_selection->focus_affinity());
    auto start = origin.move(SelectionAlteration::Move, SelectionDirection::Backward, SelectionGranularity::DocumentBoundary);
    auto end = origin.move(SelectionAlteration::Move, SelectionDirection::Forward, SelectionGranularity::DocumentBoundary);
    if (!start.has_value() || !end.has_value())
        return;

    auto start_boundary = start->boundary();
    auto end_boundary = end->boundary();
    MUST(m_selection->set_base_and_extent(start_boundary.node, start_boundary.offset, end_boundary.node, end_boundary.offset));
    m_selection->m_preferred_inline_coordinate.clear();
    m_selection->document()->reset_cursor_blink_cycle();
    m_selection->document()->set_cursor_position_needs_repaint();
    m_selection->scroll_focus_into_view();
}

}
