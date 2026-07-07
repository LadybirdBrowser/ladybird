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
#include <LibWeb/TextAffinity.h>

namespace Web {

// A visual line of a text node: a run of text that is rendered on a single line box, delimited by soft wraps and/or
// preserved newline characters. Lines with no rendered text (such as the line between two consecutive newlines) have
// no fragments. Whitespace hanging at a soft wrap point is part of the line it trails, between end_offset and
// end_offset_with_trailing_whitespace.
struct VisualLine {
    size_t start_offset { 0 };
    size_t end_offset { 0 };
    size_t end_offset_with_trailing_whitespace { 0 };
    Vector<Painting::PaintableFragment const*, 1> fragments;
};

// NB: Layout must be up to date when calling this.
Vector<VisualLine> collect_visual_lines(DOM::Text const&);

// A cursor position produced by caret navigation: the affinity determines which line the offset renders on when the
// offset sits exactly at a soft wrap boundary.
struct CursorLinePosition {
    size_t offset { 0 };
    TextAffinity affinity { TextAffinity::Downstream };
};

Optional<CursorLinePosition> compute_cursor_position_on_next_line(DOM::Text const&, size_t current_offset, TextAffinity);
Optional<CursorLinePosition> compute_cursor_position_on_previous_line(DOM::Text const&, size_t current_offset, TextAffinity);

// One cursor step right or left. At a soft wrap boundary, the same offset has two visual positions (the end of the
// earlier line and the start of the next); stepping visits both before moving to the adjacent grapheme.
Optional<CursorLinePosition> compute_cursor_position_on_next_character(DOM::Text const&, size_t current_offset, TextAffinity);
Optional<CursorLinePosition> compute_cursor_position_on_previous_character(DOM::Text const&, size_t current_offset, TextAffinity);

size_t find_visual_line_start(DOM::Text const&, size_t offset, TextAffinity);
CursorLinePosition find_visual_line_end(DOM::Text const&, size_t offset, TextAffinity);

bool white_space_preserves_newlines(Layout::TextNode const&);

// Whether any position in the text renders as an empty visual line (between two consecutive newlines, or after a
// newline at the very end of the text), assuming newlines are preserved.
bool text_contains_empty_visual_line_positions(Utf16View const&);

}
