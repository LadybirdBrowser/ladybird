/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWeb/Forward.h>

namespace Web {

// A visual line of a text node: a run of text that is rendered on a single line box, delimited by soft wraps and/or
// preserved newline characters. Lines with no rendered text (such as the line between two consecutive newlines) have
// no fragments.
struct VisualLine {
    size_t start_offset { 0 };
    size_t end_offset { 0 };
    Vector<Painting::PaintableFragment const*, 1> fragments;
};

// NB: Layout must be up to date when calling this.
Vector<VisualLine> collect_visual_lines(DOM::Text const&);

Optional<size_t> compute_cursor_position_on_next_line(DOM::Text const&, size_t current_offset);
Optional<size_t> compute_cursor_position_on_previous_line(DOM::Text const&, size_t current_offset);

size_t find_visual_line_start(DOM::Text const&, size_t offset);
size_t find_visual_line_end(DOM::Text const&, size_t offset);

bool white_space_preserves_newlines(Layout::TextNode const&);

// Whether any position in the text renders as an empty visual line (between two consecutive newlines, or after a
// newline at the very end of the text), assuming newlines are preserved.
bool text_contains_empty_visual_line_positions(Utf16View const&);

}
