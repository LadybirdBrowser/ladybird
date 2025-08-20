/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IterationDecision.h>
#include <AK/Optional.h>
#include <LibWeb/Forward.h>

namespace Web {

// When we want to move the cursor from some position within a line to a visually-equivalent position in an adjacent
// line, there are several things to consider. Let's use the following HTML as an example:
//
//     <textarea>
//     hello 👩🏼‍❤️‍👨🏻 there
//     my 👩🏼‍❤️‍👨🏻 friends!
//     </textarea>
//
// And let's define the following terms:
//    * logical index = the raw code unit offset of the cursor
//    * visual index = the grapheme-aware offset of the cursor (i.e. the offset the user actually perceives)
//    * text affinity = the side (left or right) of a grapheme that the cursor is visually closest to
//
// If we want to move the cursor from the position just after "hello" (logical index=5, visual index=5) to the next
// line, the user will expect the cursor to be located just after the "👩🏼‍❤️‍👨🏻" (logical index=15, visual index=4). These
// locations do not share the same visual index, so it's not enough to simply map the visual index of 5 back to a
// logical index on the next line. The difference becomes even more apparent when multiple fonts are used within a
// single line.
//
// Instead, we must measure the text between the start of the line and the starting index. On the next line, we want
// to find the position whose corresponding width is as close to the starting width as possible. The target width
// might not be the same as the starting width at all, so we must further consider the text affinity. We want to
// chose a target index whose affinity brings us closest to the starting width.
class GraphemeEdgeTracker {
public:
    explicit constexpr GraphemeEdgeTracker(float target_width)
        : m_target_width(target_width)
    {
    }

    constexpr IterationDecision update(size_t grapheme_length_in_code_units, float grapheme_width)
    {
        if (grapheme_width == 0)
            return IterationDecision::Continue;

        m_right_edge += grapheme_length_in_code_units;
        m_width_to_right_edge += grapheme_width;

        if (m_width_to_right_edge >= m_target_width)
            return IterationDecision::Break;

        m_left_edge = m_right_edge;
        m_width_to_left_edge = m_width_to_right_edge;

        return IterationDecision::Continue;
    }

    constexpr size_t resolve() const
    {
        if ((m_target_width - m_width_to_left_edge) < (m_width_to_right_edge - m_target_width))
            return m_left_edge;
        return m_right_edge;
    }

private:
    float m_target_width { 0 };

    size_t m_left_edge { 0 };
    size_t m_right_edge { 0 };

    float m_width_to_left_edge { 0 };
    float m_width_to_right_edge { 0 };
};

Optional<size_t> compute_cursor_position_on_next_line(DOM::Text const&, size_t current_offset);
Optional<size_t> compute_cursor_position_on_previous_line(DOM::Text const&, size_t current_offset);

}
