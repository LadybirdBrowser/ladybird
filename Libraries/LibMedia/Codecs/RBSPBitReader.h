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

namespace Media::Codecs {

// Reads a raw byte sequence payload, the form H.264 and H.265 parameter sets take once the emulation prevention
// bytes padding their 0x000000 through 0x000003 sequences are dropped. Overrun latches as it does in BitReader, so
// a parse checks has_overrun() once at the end rather than testing every field.
class RBSPBitReader {
public:
    explicit RBSPBitReader(ReadonlyBytes data)
        : m_data(data)
    {
    }

    template<Unsigned T = u64>
    T read_bits(size_t count)
    {
        VERIFY(count <= sizeof(T) * 8);

        T result = 0;
        while (count > 0) {
            if (m_byte_index >= m_data.size()) {
                m_has_overrun = true;
                return 0;
            }

            auto bits_left_in_byte = 8 - m_bit_offset;
            auto bits_to_take = min(count, bits_left_in_byte);
            auto byte = m_data[m_byte_index];
            auto chunk = (byte >> (bits_left_in_byte - bits_to_take)) & ((1u << bits_to_take) - 1);
            result = static_cast<T>(result << bits_to_take) | chunk;

            m_bit_offset += bits_to_take;
            count -= bits_to_take;
            if (m_bit_offset == 8)
                advance_to_next_byte(byte);
        }
        return result;
    }

    bool read_bit() { return read_bits<u8>(1) != 0; }

    void skip_bits(size_t count)
    {
        while (count > 0) {
            auto bits_to_skip = min(count, static_cast<size_t>(8));
            read_bits<u8>(bits_to_skip);
            count -= bits_to_skip;
        }
    }

    // ue(v), an Exp-Golomb coded unsigned integer.
    u32 read_unsigned_exp_golomb()
    {
        static constexpr size_t MAXIMUM_LEADING_ZERO_COUNT = 31;
        size_t leading_zero_count = 0;
        while (!read_bit()) {
            if (m_has_overrun)
                return 0;
            leading_zero_count++;
            if (leading_zero_count == MAXIMUM_LEADING_ZERO_COUNT) {
                m_has_overrun = true;
                return 0;
            }
        }
        return static_cast<u32>((1u << leading_zero_count) - 1 + read_bits<u32>(leading_zero_count));
    }

    // se(v), which maps the unsigned code to 0, 1, -1, 2, -2 and so on.
    i32 read_signed_exp_golomb()
    {
        auto code = read_unsigned_exp_golomb();
        auto magnitude = static_cast<i32>((code + 1) / 2);
        return (code % 2) == 0 ? -magnitude : magnitude;
    }

    bool has_overrun() const { return m_has_overrun; }

private:
    void advance_to_next_byte(u8 consumed_byte)
    {
        static constexpr u8 EMULATION_PREVENTION_BYTE = 0x03;

        m_bit_offset = 0;
        m_byte_index++;
        m_preceding_zero_byte_count = consumed_byte == 0 ? m_preceding_zero_byte_count + 1 : 0;

        if (m_preceding_zero_byte_count < 2 || m_byte_index >= m_data.size())
            return;
        if (m_data[m_byte_index] == EMULATION_PREVENTION_BYTE) {
            m_byte_index++;
            m_preceding_zero_byte_count = 0;
        }
    }

    ReadonlyBytes m_data;
    size_t m_byte_index { 0 };
    u8 m_bit_offset { 0 };
    u8 m_preceding_zero_byte_count { 0 };
    bool m_has_overrun { false };
};

}
