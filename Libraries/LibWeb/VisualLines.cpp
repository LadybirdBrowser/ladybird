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

bool white_space_preserves_newlines(Layout::TextNode const& layout_node)
{
    return first_is_one_of(layout_node.parent()->computed_values().white_space_collapse(),
        CSS::WhiteSpaceCollapse::Preserve, CSS::WhiteSpaceCollapse::PreserveBreaks);
}

bool text_contains_empty_visual_line_positions(Utf16View const& text)
{
    auto length = text.length_in_code_units();
    for (size_t i = 0; i < length; ++i) {
        if (text.code_unit_at(i) != '\n')
            continue;
        auto position = i + 1;
        if (position == length || text.code_unit_at(position) == '\n')
            return true;
    }
    return false;
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
            && lines.last().fragments.last()->line_index() == fragment.line_index()) {
            auto& line = lines.last();
            line.start_offset = min(line.start_offset, fragment.dom_start_offset_in_node());
            line.end_offset = max(line.end_offset, fragment.dom_end_offset_in_node());
            line.end_offset_with_trailing_whitespace = max(line.end_offset_with_trailing_whitespace, fragment.dom_end_offset_with_trailing_whitespace());
            line.fragments.append(&fragment);
        } else {
            lines.append({ fragment.dom_start_offset_in_node(), fragment.dom_end_offset_in_node(), fragment.dom_end_offset_with_trailing_whitespace(), { &fragment } });
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
                lines.append({ position, position, position, {} });
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

static Optional<size_t> visual_line_index_for_offset(Vector<VisualLine> const& lines, size_t offset, TextAffinity affinity)
{
    // An offset exactly at a soft wrap boundary belongs to the earlier line with Upstream affinity and to the later
    // line with Downstream affinity. This matches how the caret is painted (see
    // PaintableWithLines::fragment_at_position).
    Optional<size_t> boundary_match;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (offset < lines[i].start_offset || offset > lines[i].end_offset_with_trailing_whitespace)
            continue;
        if (affinity == TextAffinity::Downstream && offset == lines[i].end_offset_with_trailing_whitespace) {
            // A later line starting exactly at this offset takes precedence.
            if (!boundary_match.has_value())
                boundary_match = i;
            continue;
        }
        return i;
    }
    if (boundary_match.has_value())
        return boundary_match;

    if (lines.is_empty())
        return {};

    // Offsets that no line covers belong to the closest line that starts before them.
    Optional<size_t> result;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].start_offset <= offset)
            result = i;
    }
    return result.value_or(0);
}

// The affinity a cursor landing on `offset` within `lines[line_index]` needs in order to render on that line.
static TextAffinity affinity_for_offset_on_line(Vector<VisualLine> const& lines, size_t line_index, size_t offset)
{
    if (line_index + 1 < lines.size() && offset >= lines[line_index + 1].start_offset)
        return TextAffinity::Upstream;
    return TextAffinity::Downstream;
}

// Whether the offset sits at a soft wrap boundary with two visual caret positions (the end of one visual line and
// the start of the next).
static bool offset_is_soft_wrap_boundary(Vector<VisualLine> const& lines, size_t offset)
{
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        if (lines[i].end_offset_with_trailing_whitespace == offset && lines[i + 1].start_offset == offset)
            return true;
    }
    return false;
}

// Moving right into whitespace hanging at a soft wrap point, or onto a soft wrap boundary, keeps the cursor on the
// earlier line.
static TextAffinity affinity_for_moving_right(Vector<VisualLine> const& lines, size_t new_offset)
{
    for (size_t i = 0; i < lines.size(); ++i) {
        if (new_offset > lines[i].end_offset && new_offset <= lines[i].end_offset_with_trailing_whitespace)
            return TextAffinity::Upstream;
        if (new_offset == lines[i].end_offset_with_trailing_whitespace
            && i + 1 < lines.size() && lines[i + 1].start_offset == new_offset)
            return TextAffinity::Upstream;
    }
    return TextAffinity::Downstream;
}

// Returns the absolute inline-axis coordinate of the caret at the given offset, which must be on the given line.
// Returns nothing for lines without rendered text, whose caret sits at the line's inline start.
static Optional<CSSPixels> caret_inline_coordinate(VisualLine const& line, size_t offset)
{
    if (line.fragments.is_empty())
        return {};

    auto const* fragment = line.fragments.first();
    for (auto const* candidate : line.fragments) {
        if (offset >= candidate->dom_start_offset_in_node() && offset <= candidate->dom_end_offset_with_trailing_whitespace()) {
            fragment = candidate;
            break;
        }
        if (candidate->dom_start_offset_in_node() <= offset)
            fragment = candidate;
    }

    auto clamped_offset = clamp(offset, fragment->dom_start_offset_in_node(), fragment->dom_end_offset_with_trailing_whitespace());
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

Optional<CursorLinePosition> compute_cursor_position_on_next_line(DOM::Text const& dom_node, size_t current_offset, TextAffinity affinity)
{
    // NB: The layout update is best-effort; a detached document may still have no layout node.
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    if (!as_if<Layout::TextNode>(dom_node.unsafe_layout_node()))
        return {};

    auto line_index = visual_line_index_for_offset(lines, current_offset, affinity);

    // On the last line (or with no line information at all), move to the end of the text.
    if (!line_index.has_value() || *line_index + 1 >= lines.size())
        return CursorLinePosition { dom_node.data().length_in_code_units(), TextAffinity::Downstream };

    auto inline_coordinate = caret_inline_coordinate(lines[*line_index], current_offset);
    auto new_offset = offset_in_visual_line_closest_to_inline_coordinate(lines[*line_index + 1], inline_coordinate);
    return CursorLinePosition { new_offset, affinity_for_offset_on_line(lines, *line_index + 1, new_offset) };
}

Optional<CursorLinePosition> compute_cursor_position_on_previous_line(DOM::Text const& dom_node, size_t current_offset, TextAffinity affinity)
{
    // NB: The layout update is best-effort; a detached document may still have no layout node.
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    if (!as_if<Layout::TextNode>(dom_node.unsafe_layout_node()))
        return {};

    auto line_index = visual_line_index_for_offset(lines, current_offset, affinity);

    // On the first line (or with no line information at all), move to the start of the text.
    if (!line_index.has_value() || *line_index == 0)
        return CursorLinePosition { 0, TextAffinity::Downstream };

    auto inline_coordinate = caret_inline_coordinate(lines[*line_index], current_offset);
    auto new_offset = offset_in_visual_line_closest_to_inline_coordinate(lines[*line_index - 1], inline_coordinate);
    return CursorLinePosition { new_offset, affinity_for_offset_on_line(lines, *line_index - 1, new_offset) };
}

Optional<CursorLinePosition> compute_cursor_position_on_next_character(DOM::Text const& dom_node, size_t current_offset, TextAffinity affinity)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);

    // Step from the end of the earlier line to the start of the next line without advancing the offset.
    if (affinity == TextAffinity::Upstream && offset_is_soft_wrap_boundary(lines, current_offset))
        return CursorLinePosition { current_offset, TextAffinity::Downstream };

    auto next_offset = dom_node.grapheme_segmenter().next_boundary(current_offset);
    if (!next_offset.has_value())
        return {};
    return CursorLinePosition { *next_offset, affinity_for_moving_right(lines, *next_offset) };
}

Optional<CursorLinePosition> compute_cursor_position_on_previous_character(DOM::Text const& dom_node, size_t current_offset, TextAffinity affinity)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);

    // Step from the start of the later line back to the end of the earlier line without moving the offset.
    if (affinity == TextAffinity::Downstream && offset_is_soft_wrap_boundary(lines, current_offset))
        return CursorLinePosition { current_offset, TextAffinity::Upstream };

    auto previous_offset = dom_node.grapheme_segmenter().previous_boundary(current_offset);
    if (!previous_offset.has_value())
        return {};
    return CursorLinePosition { *previous_offset, TextAffinity::Downstream };
}

bool offset_is_on_first_visual_line(DOM::Text const& dom_node, size_t offset, TextAffinity affinity)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    auto line_index = visual_line_index_for_offset(lines, offset, affinity);
    return !line_index.has_value() || *line_index == 0;
}

bool offset_is_on_last_visual_line(DOM::Text const& dom_node, size_t offset, TextAffinity affinity)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    auto line_index = visual_line_index_for_offset(lines, offset, affinity);
    return !line_index.has_value() || *line_index + 1 >= lines.size();
}

Optional<CSSPixels> cursor_inline_coordinate(DOM::Text const& dom_node, size_t offset, TextAffinity affinity)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    auto line_index = visual_line_index_for_offset(lines, offset, affinity);
    if (!line_index.has_value())
        return {};
    return caret_inline_coordinate(lines[*line_index], offset);
}

Optional<CursorLinePosition> cursor_position_at_visual_start(DOM::Text const& dom_node)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    if (lines.is_empty())
        return {};
    auto offset = lines.first().start_offset;
    return CursorLinePosition { offset, affinity_for_offset_on_line(lines, 0, offset) };
}

Optional<CursorLinePosition> cursor_position_at_visual_end(DOM::Text const& dom_node)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    if (lines.is_empty())
        return {};
    auto offset = lines.last().end_offset;
    return CursorLinePosition { offset, affinity_for_offset_on_line(lines, lines.size() - 1, offset) };
}

Optional<CursorLinePosition> cursor_position_on_first_line_closest_to(DOM::Text const& dom_node, Optional<CSSPixels> inline_coordinate)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    if (lines.is_empty())
        return {};
    auto offset = offset_in_visual_line_closest_to_inline_coordinate(lines.first(), inline_coordinate);
    return CursorLinePosition { offset, affinity_for_offset_on_line(lines, 0, offset) };
}

Optional<CursorLinePosition> cursor_position_on_last_line_closest_to(DOM::Text const& dom_node, Optional<CSSPixels> inline_coordinate)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    if (lines.is_empty())
        return {};
    auto offset = offset_in_visual_line_closest_to_inline_coordinate(lines.last(), inline_coordinate);
    return CursorLinePosition { offset, affinity_for_offset_on_line(lines, lines.size() - 1, offset) };
}

size_t find_visual_line_start(DOM::Text const& dom_node, size_t offset, TextAffinity affinity)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    if (auto line_index = visual_line_index_for_offset(lines, offset, affinity); line_index.has_value())
        return lines[*line_index].start_offset;

    return find_line_start(dom_node.data().utf16_view(), offset);
}

CursorLinePosition find_visual_line_end(DOM::Text const& dom_node, size_t offset, TextAffinity affinity)
{
    auto lines = visual_lines_with_up_to_date_layout(dom_node);
    if (auto line_index = visual_line_index_for_offset(lines, offset, affinity); line_index.has_value()) {
        // The end of a visual line includes whitespace hanging at a soft wrap point.
        auto end_offset = lines[*line_index].end_offset_with_trailing_whitespace;
        return CursorLinePosition { end_offset, affinity_for_offset_on_line(lines, *line_index, end_offset) };
    }

    return CursorLinePosition { find_line_end(dom_node.data().utf16_view(), offset), TextAffinity::Downstream };
}

}
