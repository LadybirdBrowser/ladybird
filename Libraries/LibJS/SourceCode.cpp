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

NonnullRefPtr<SourceCode const> SourceCode::create(String filename, Utf16String code)
{
    return adopt_ref(*new SourceCode(move(filename), move(code)));
}

SourceCode::SourceCode(String filename, Utf16String code)
    : m_filename(move(filename))
    , m_code(move(code))
    , m_code_view(m_code.utf16_view())
    , m_length_in_code_units(m_code_view.length_in_code_units())
{
}

u16 const* SourceCode::utf16_data() const
{
    if (!m_code_view.has_ascii_storage())
        return reinterpret_cast<u16 const*>(m_code_view.utf16_span().data());

    if (m_utf16_data_cache.is_empty() && m_length_in_code_units > 0) {
        auto ascii = m_code_view.ascii_span();
        m_utf16_data_cache.ensure_capacity(m_length_in_code_units);
        for (size_t i = 0; i < m_length_in_code_units; ++i)
            m_utf16_data_cache.unchecked_append(static_cast<u16>(ascii[i]));
    }
    return m_utf16_data_cache.data();
}

void SourceCode::fill_position_cache() const
{
    constexpr size_t predicted_minimum_cached_positions = 8;
    constexpr size_t minimum_distance_between_cached_positions = 32;
    constexpr size_t maximum_distance_between_cached_positions = 8192;

    if (m_code.is_empty())
        return;

    u32 previous_code_point = 0;
    u32 line = 1;
    u32 column = 1;
    u32 offset_of_last_starting_point = 0;

    m_cached_positions.ensure_capacity(predicted_minimum_cached_positions + (m_code.length_in_code_units() / maximum_distance_between_cached_positions));
    m_cached_positions.append({ .line = 1, .column = 1, .offset = 0 });

    auto view = m_code.utf16_view();

    for (auto it = view.begin(); it != view.end(); ++it) {
        u32 code_point = *it;
        bool is_line_terminator = code_point == '\r' || (code_point == '\n' && previous_code_point != '\r') || code_point == LINE_SEPARATOR || code_point == PARAGRAPH_SEPARATOR;

        auto offset = view.iterator_offset(it);
        VERIFY(offset <= NumericLimits<u32>::max());

        bool is_nonempty_line = is_line_terminator && previous_code_point != '\n' && previous_code_point != LINE_SEPARATOR && previous_code_point != PARAGRAPH_SEPARATOR && (code_point == '\n' || previous_code_point != '\r');
        auto distance_between_cached_position = offset - offset_of_last_starting_point;

        if ((distance_between_cached_position >= minimum_distance_between_cached_positions && is_nonempty_line) || distance_between_cached_position >= maximum_distance_between_cached_positions) {
            m_cached_positions.append({ .line = line, .column = column, .offset = static_cast<u32>(offset) });
            offset_of_last_starting_point = offset;
        }

        if (is_line_terminator) {
            line += 1;
            column = 1;
        } else {
            column += 1;
        }

        previous_code_point = code_point;
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

    u32 previous_code_point = 0;

    auto view = m_code.utf16_view();

    for (auto it = view.iterator_at_code_unit_offset(current.offset); it != view.end(); ++it) {
        // If we're on or after the start offset, this is the start position.
        if (!start.has_value() && view.iterator_offset(it) >= start_offset) {
            start = Position {
                .line = current.line,
                .column = current.column,
                .offset = start_offset,
            };
        }

        // If we're on or after the end offset, this is the end position.
        if (!end.has_value() && view.iterator_offset(it) >= end_offset) {
            end = Position {
                .line = current.line,
                .column = current.column,
                .offset = end_offset,
            };
            break;
        }

        u32 code_point = *it;

        bool const is_line_terminator = code_point == '\r' || (code_point == '\n' && previous_code_point != '\r') || code_point == LINE_SEPARATOR || code_point == PARAGRAPH_SEPARATOR;
        previous_code_point = code_point;

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
