/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/Utf16View.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/GraphemeEdgeTracker.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/TextOffsetMapping.h>
#include <LibWeb/Painting/PaintableFragment.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/VisualLines.h>

namespace Web {

static bool white_space_preserves_newlines(Layout::TextNode const& layout_node)
{
    return first_is_one_of(layout_node.computed_values().white_space_collapse(),
        CSS::WhiteSpaceCollapse::Preserve, CSS::WhiteSpaceCollapse::PreserveBreaks);
}

Vector<VisualLine> collect_visual_lines(DOM::Text const& dom_node)
{
    Vector<VisualLine> lines;

    auto const* layout_node = as_if<Layout::TextNode>(dom_node.unsafe_layout_node());
    if (!layout_node)
        return lines;

    Layout::TextOffsetMapping mapping { dom_node };
    mapping.for_each_paintable_fragment([&](Painting::PaintableFragment const& fragment) {
        if (!lines.is_empty()
            && !lines.last().fragments.is_empty()
            && &lines.last().fragments.last()->paintable_with_lines() == &fragment.paintable_with_lines()
            && lines.last().fragments.last()->line_box_data().index == fragment.line_box_data().index) {
            auto& line = lines.last();
            line.start_offset = min(line.start_offset, fragment.dom_start_offset_in_node());
            line.end_offset = max(line.end_offset, fragment.dom_end_offset_in_node());
            line.fragments.append(&fragment);
        } else {
            lines.append({ fragment.dom_start_offset_in_node(), fragment.dom_end_offset_in_node(), { &fragment } });
        }
        return TraversalDecision::Continue;
    });

    // Positions that render as empty lines (between two consecutive preserved newlines, or after a preserved newline
    // at the very end of the text) have no fragments; add caret-only lines for them.
    if (white_space_preserves_newlines(*layout_node)) {
        auto text = dom_node.data().utf16_view();
        auto length = text.length_in_code_units();
        for (size_t i = 0; i < length; ++i) {
            if (text.code_unit_at(i) != '\n')
                continue;
            auto position = i + 1;
            if (position == length || text.code_unit_at(position) == '\n')
                lines.append({ position, position, {} });
        }
    }

    quick_sort(lines, [](auto const& a, auto const& b) { return a.start_offset < b.start_offset; });
    return lines;
}

static Vector<VisualLine> visual_lines_with_up_to_date_layout(DOM::Text const& dom_node)
{
    const_cast<DOM::Document&>(dom_node.document()).update_layout_if_needed_for_node(dom_node, DOM::UpdateLayoutReason::CursorLineNavigation);
    return collect_visual_lines(dom_node);
}

static Optional<size_t> visual_line_index_for_offset(Vector<VisualLine> const& lines, size_t offset)
{
    // An offset exactly at a soft wrap boundary belongs to the line before the wrap. This matches how the caret is
    // painted (see PaintableWithLines::fragment_at_position).
    for (size_t i = 0; i < lines.size(); ++i) {
        if (offset >= lines[i].start_offset && offset <= lines[i].end_offset)
            return i;
    }

    if (lines.is_empty())
        return {};

    // Offsets inside whitespace that was collapsed at a soft wrap belong to the line they trail.
    Optional<size_t> result;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].start_offset <= offset)
            result = i;
    }
    return result.value_or(0);
}

// Returns the absolute inline-axis coordinate of the caret at the given offset, which must be on the given line.
// Returns nothing for lines without rendered text, whose caret sits at the line's inline start.
static Optional<CSSPixels> caret_inline_coordinate(VisualLine const& line, size_t offset)
{
    if (line.fragments.is_empty())
        return {};

    auto const* fragment = line.fragments.first();
    for (auto const* candidate : line.fragments) {
        if (offset >= candidate->dom_start_offset_in_node() && offset <= candidate->dom_end_offset_in_node()) {
            fragment = candidate;
            break;
        }
        if (candidate->dom_start_offset_in_node() <= offset)
            fragment = candidate;
    }

    auto clamped_offset = clamp(offset, fragment->dom_start_offset_in_node(), fragment->dom_end_offset_in_node());
    auto rect = fragment->range_rect(Painting::Paintable::SelectionState::StartAndEnd, clamped_offset, clamped_offset);
    return rect.primary_offset_for_orientation(fragment->orientation());
}

static size_t offset_in_visual_line_closest_to_inline_coordinate(VisualLine const& line, Optional<CSSPixels> inline_coordinate)
{
    if (line.fragments.is_empty() || !inline_coordinate.has_value())
        return line.start_offset;

    // Pick the last fragment that starts at or before the coordinate; for coordinates before all fragments, the
    // first fragment applies.
    auto const* fragment = line.fragments.first();
    for (auto const* candidate : line.fragments) {
        auto inline_start = candidate->absolute_rect().primary_offset_for_orientation(candidate->orientation());
        if (inline_start <= *inline_coordinate)
            fragment = candidate;
    }

    auto rect = fragment->absolute_rect();
    auto point = rect.center();
    point.set_primary_offset_for_orientation(fragment->orientation(), max(rect.primary_offset_for_orientation(fragment->orientation()), *inline_coordinate));
    return fragment->index_in_node_for_point(point);
}

Optional<size_t> compute_cursor_position_on_next_line(DOM::Text const& dom_node, size_t current_offset)
{
    // NB: The layout update is best-effort; a detached document may still have no layout node.
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    if (!as_if<Layout::TextNode>(dom_node.unsafe_layout_node()))
        return {};

    auto line_index = visual_line_index_for_offset(lines, current_offset);

    // On the last line (or with no line information at all), move to the end of the text.
    if (!line_index.has_value() || *line_index + 1 >= lines.size())
        return dom_node.data().length_in_code_units();

    auto inline_coordinate = caret_inline_coordinate(lines[*line_index], current_offset);
    return offset_in_visual_line_closest_to_inline_coordinate(lines[*line_index + 1], inline_coordinate);
}

Optional<size_t> compute_cursor_position_on_previous_line(DOM::Text const& dom_node, size_t current_offset)
{
    // NB: The layout update is best-effort; a detached document may still have no layout node.
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    if (!as_if<Layout::TextNode>(dom_node.unsafe_layout_node()))
        return {};

    auto line_index = visual_line_index_for_offset(lines, current_offset);

    // On the first line (or with no line information at all), move to the start of the text.
    if (!line_index.has_value() || *line_index == 0)
        return 0uz;

    auto inline_coordinate = caret_inline_coordinate(lines[*line_index], current_offset);
    return offset_in_visual_line_closest_to_inline_coordinate(lines[*line_index - 1], inline_coordinate);
}

size_t find_visual_line_start(DOM::Text const& dom_node, size_t offset)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    if (auto line_index = visual_line_index_for_offset(lines, offset); line_index.has_value())
        return lines[*line_index].start_offset;

    return find_line_start(dom_node.data().utf16_view(), offset);
}

size_t find_visual_line_end(DOM::Text const& dom_node, size_t offset)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    if (auto line_index = visual_line_index_for_offset(lines, offset); line_index.has_value())
        return lines[*line_index].end_offset;

    return find_line_end(dom_node.data().utf16_view(), offset);
}

}
