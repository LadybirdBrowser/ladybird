/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/GraphemeEdgeTracker.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web {

// FIXME: Using newline characters to determine line breaks is insufficient. If a line is wrapped due space constraints,
//        we want to consider each segment of the wrapped line as its own line in the algorithms below.

static constexpr size_t find_line_start(Utf16View const& view, size_t offset)
{
    while (offset != 0 && view.code_unit_at(offset - 1) != '\n')
        --offset;
    return offset;
}

static constexpr size_t find_line_end(Utf16View const& view, size_t offset)
{
    auto length = view.length_in_code_units();
    while (offset < length && view.code_unit_at(offset) != '\n')
        ++offset;
    return offset;
}

static float measure_text_width(Layout::TextNode const& text_node, Utf16View const& text)
{
    if (text.is_empty())
        return 0;

    auto segmenter = text_node.grapheme_segmenter().clone();
    segmenter->set_segmented_text(text);

    Layout::TextNode::ChunkIterator iterator { text_node, text, *segmenter, false, false };
    float width = 0;

    for (auto chunk = iterator.next(); chunk.has_value(); chunk = iterator.next())
        width += chunk->font->width(chunk->view);

    return width;
}

static size_t translate_position_across_lines(Layout::TextNode const& text_node, Utf16View const& source_line, Utf16View const& target_line)
{
    GraphemeEdgeTracker tracker(measure_text_width(text_node, source_line));
    auto previous_index = 0uz;

    text_node.grapheme_segmenter().clone()->for_each_boundary(target_line, [&](auto index) {
        auto current_width = measure_text_width(text_node, target_line.substring_view(previous_index, index - previous_index));

        if (tracker.update(index - previous_index, current_width) == IterationDecision::Break)
            return IterationDecision::Break;

        previous_index = index;
        return IterationDecision::Continue;
    });

    return tracker.resolve();
}

Optional<size_t> compute_cursor_position_on_next_line(DOM::Text const& dom_node, size_t current_offset)
{
    auto const* layout_node = as_if<Layout::TextNode>(dom_node.layout_node());
    if (!layout_node)
        return {};

    auto text = dom_node.data().utf16_view();
    auto new_offset = text.length_in_code_units();

    if (auto current_line_end = find_line_end(text, current_offset); current_line_end < text.length_in_code_units()) {
        auto current_line_start = find_line_start(text, current_offset);
        auto current_line_text = text.substring_view(current_line_start, current_offset - current_line_start);

        auto next_line_start = current_line_end + 1;
        auto next_line_length = find_line_end(text, next_line_start) - next_line_start;
        auto next_line_text = text.substring_view(next_line_start, next_line_length);

        new_offset = next_line_start + translate_position_across_lines(*layout_node, current_line_text, next_line_text);
    }

    return new_offset;
}

Optional<size_t> compute_cursor_position_on_previous_line(DOM::Text const& dom_node, size_t current_offset)
{
    auto const* layout_node = as_if<Layout::TextNode>(dom_node.layout_node());
    if (!layout_node)
        return {};

    auto text = dom_node.data().utf16_view();
    auto new_offset = 0uz;

    if (auto current_line_start = find_line_start(text, current_offset); current_line_start != 0) {
        auto current_line_text = text.substring_view(current_line_start, current_offset - current_line_start);

        auto previous_line_start = find_line_start(text, current_line_start - 1);
        auto previous_line_length = current_line_start - previous_line_start - 1;
        auto previous_line_text = text.substring_view(previous_line_start, previous_line_length);

        new_offset = previous_line_start + translate_position_across_lines(*layout_node, current_line_text, previous_line_text);
    }

    return new_offset;
}

}
