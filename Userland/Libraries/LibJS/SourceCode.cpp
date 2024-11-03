/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/Utf8View.h>
#include <LibJS/SourceCode.h>
#include <LibJS/SourceRange.h>
#include <LibJS/Token.h>

namespace JS {

NonnullRefPtr<SourceCode const> SourceCode::create(String filename, String code)
{
    return adopt_ref(*new SourceCode(move(filename), move(code)));
}

SourceCode::SourceCode(String filename, String code)
    : m_filename(move(filename))
    , m_code(move(code))
{
}

String const& SourceCode::filename() const
{
    return m_filename;
}

String const& SourceCode::code() const
{
    return m_code;
}

void SourceCode::fill_position_cache() const
{
    constexpr size_t minimum_distance_between_cached_positions = 10000;

    if (m_code.is_empty())
        return;

    bool previous_code_point_was_carriage_return = false;
    size_t line = 1;
    size_t column = 1;
    size_t offset_of_last_starting_point = 0;
    m_cached_positions.ensure_capacity(m_code.bytes().size() / minimum_distance_between_cached_positions);
    m_cached_positions.append({ .line = 1, .column = 1, .offset = 0 });

    Utf8View const view(m_code);
    for (auto it = view.begin(); it != view.end(); ++it) {
        u32 code_point = *it;
        bool is_line_terminator = code_point == '\r' || (code_point == '\n' && !previous_code_point_was_carriage_return) || code_point == LINE_SEPARATOR || code_point == PARAGRAPH_SEPARATOR;
        previous_code_point_was_carriage_return = code_point == '\r';

        auto byte_offset = view.byte_offset_of(it);
        if ((byte_offset - offset_of_last_starting_point) >= minimum_distance_between_cached_positions) {
            m_cached_positions.append({ .line = line, .column = column, .offset = byte_offset });
            offset_of_last_starting_point = byte_offset;
        }

        if (is_line_terminator) {
            line += 1;
            column = 1;
        } else {
            column += 1;
        }
    }
}

SourceRange SourceCode::range_from_offsets(u32 start_offset, u32 end_offset) const
{
    // If the underlying code is an empty string, the range is 1,1 - 1,1 no matter what.
    if (m_code.is_empty())
        return { *this, { .line = 1, .column = 1, .offset = 0 }, { .line = 1, .column = 1, .offset = 0 } };

    if (m_cached_positions.is_empty())
        fill_position_cache();

    Position current { .line = 1, .column = 1, .offset = 0 };

    if (!m_cached_positions.is_empty()) {
        Position const dummy;
        size_t nearest_index = 0;
        binary_search(m_cached_positions, dummy, &nearest_index,
            [&](auto&, auto& starting_point) {
                return start_offset - starting_point.offset;
            });

        current = m_cached_positions[nearest_index];
    }

    Optional<Position> start;
    Optional<Position> end;

    bool previous_code_point_was_carriage_return = false;

    Utf8View const view(m_code);
    for (auto it = view.iterator_at_byte_offset_without_validation(current.offset); it != view.end(); ++it) {

        // If we're on or after the start offset, this is the start position.
        if (!start.has_value() && view.byte_offset_of(it) >= start_offset) {
            start = Position {
                .line = current.line,
                .column = current.column,
                .offset = start_offset,
            };
        }

        // If we're on or after the end offset, this is the end position.
        if (!end.has_value() && view.byte_offset_of(it) >= end_offset) {
            end = Position {
                .line = current.line,
                .column = current.column,
                .offset = end_offset,
            };
            break;
        }

        u32 code_point = *it;

        bool const is_line_terminator = code_point == '\r' || (code_point == '\n' && !previous_code_point_was_carriage_return) || code_point == LINE_SEPARATOR || code_point == PARAGRAPH_SEPARATOR;
        previous_code_point_was_carriage_return = code_point == '\r';

        if (is_line_terminator) {
            current.line += 1;
            current.column = 1;
            continue;
        }
        current.column += 1;
    }

    // If we didn't find both a start and end position, just return 1,1-1,1.
    // FIXME: This is a hack. Find a way to return the nicest possible values here.
    if (!start.has_value() || !end.has_value())
        return SourceRange { *this, { .line = 1, .column = 1 }, { .line = 1, .column = 1 } };

    return SourceRange { *this, *start, *end };
}

}
