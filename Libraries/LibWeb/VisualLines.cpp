/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/VisualLines.h>

namespace Web {

Vector<VisualLine> collect_visual_lines(Layout::BegunRead const& read, DOM::Text const& dom_node)
{
    Vector<VisualLine> lines;

    // NB: Unlike the callers below, this one is also reached from serialization and caret queries
    //     that do not update layout first, so it has to read whatever layout it finds.
    auto const* layout_node = as_if<Layout::TextNode>(dom_node.unsafe_layout_node(read));
    if (!layout_node)
        return lines;

    Layout::RustFFI::render_state_text_visual_lines(layout_node->document_host(), Layout::Node::slot_id(layout_node), &lines,
        [](void* context, Layout::RustFFI::FfiVisualLine line) {
            static_cast<Vector<VisualLine>*>(context)->append({
                .start_offset = line.start_offset,
                .end_offset = line.end_offset,
                .end_offset_with_trailing_whitespace = line.end_offset_with_trailing_whitespace,
                .has_fragments = line.has_fragments,
                .owner_paintable = line.owner_paintable,
                .line_index = line.line_index,
            });
        });
    return lines;
}

static Vector<VisualLine> visual_lines_with_up_to_date_layout(DOM::Text const& dom_node)
{
    Layout::ForcedReadScope read { dom_node.document() };
    const_cast<DOM::Document&>(dom_node.document()).update_layout_if_needed_for_node(dom_node, DOM::UpdateLayoutReason::CursorLineNavigation);
    return collect_visual_lines(read, dom_node);
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

size_t find_line_start(Utf16View const& view, size_t offset)
{
    while (offset != 0 && view.code_unit_at(offset - 1) != '\n')
        --offset;
    return offset;
}

size_t find_line_end(Utf16View const& view, size_t offset)
{
    auto length = view.length_in_code_units();
    while (offset < length && view.code_unit_at(offset) != '\n')
        ++offset;
    return offset;
}

}
