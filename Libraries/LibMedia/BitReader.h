/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Concepts.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>

namespace Media {

// Reads big-endian bit fields from a bounded in-memory bitstream. A read past the end of the data
// consumes the remainder, yields zero, and latches the reader into an overrun state, so a parse
// checks has_overrun() once at the end instead of testing every field it reads.
class BitReader {
public:
    explicit BitReader(ReadonlyBytes data)
        : m_data(data)
    {
    }

    template<Unsigned T = u64>
    T read_bits(size_t count)
    {
        VERIFY(count <= sizeof(T) * 8);
        if (count > bits_remaining()) {
            m_bit_position = total_bit_count();
            m_has_overrun = true;
            return 0;
        }

        T result = 0;
        while (count > 0) {
            auto bits_left_in_byte = 8 - (m_bit_position % 8);
            auto bits_to_take = min(count, bits_left_in_byte);
            auto byte = m_data[m_bit_position / 8];
            auto chunk = (byte >> (bits_left_in_byte - bits_to_take)) & ((1u << bits_to_take) - 1);
            result = static_cast<T>(result << bits_to_take) | chunk;
            m_bit_position += bits_to_take;
            count -= bits_to_take;
        }
        return result;
    }

    bool read_bit() { return read_bits<u8>(1) != 0; }

    void skip_bits(size_t count)
    {
        if (count > bits_remaining()) {
            m_bit_position = total_bit_count();
            m_has_overrun = true;
            return;
        }
        m_bit_position += count;
    }

    void align_to_byte() { skip_bits((8 - m_bit_position % 8) % 8); }

    size_t bit_position() const { return m_bit_position; }
    size_t bits_remaining() const { return total_bit_count() - m_bit_position; }
    bool has_overrun() const { return m_has_overrun; }

private:
    size_t total_bit_count() const { return m_data.size() * 8; }

    ReadonlyBytes m_data;
    size_t m_bit_position { 0 };
    bool m_has_overrun { false };
};

}
