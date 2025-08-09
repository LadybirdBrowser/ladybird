/*
 * Copyright (c) 2020, Benoit Lormeau <blormeau@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>

namespace AK {

LineTrackingLexer::Position LineTrackingLexer::position_for(size_t index) const
{
    // Sad case: we have no idea where the nearest newline is, so we have to
    //           scan ahead a bit.
    while (index > m_largest_known_line_start_position) {
        auto next_newline = m_input.find('\n', m_largest_known_line_start_position);
        if (!next_newline.has_value()) {
            // No more newlines, add the end of the input as a line start to avoid searching again.
            m_line_start_positions->insert(input_length(), m_line_start_positions->size());
            m_largest_known_line_start_position = input_length();
            break;
        }
        m_line_start_positions->insert(next_newline.value() + 1, m_line_start_positions->size());
        m_largest_known_line_start_position = next_newline.value() + 1;
    }
    // We should always have at least the first line start position.
    auto previous_line_it = m_line_start_positions->find_largest_not_above_iterator(index);
    auto previous_line_index = previous_line_it.key();

    auto line = *previous_line_it;
    auto column = index - previous_line_index;
    if (line == 0) {
        // First line, take into account the start position.
        column += m_first_line_start_position.column;
    }

    line += m_first_line_start_position.line;
    return { index, line, column };
}

}
