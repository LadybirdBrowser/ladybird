/*
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/MaybeOwned.h>
#include <AK/Stream.h>

namespace AK {

/// A stream wrapper class that allows you to write arbitrary amounts of bits
/// in big-endian order to another stream.
class BigEndianOutputBitStream : public Stream {
public:
    explicit BigEndianOutputBitStream(MaybeOwned<Stream> stream)
        : m_stream(move(stream))
    {
    }

    virtual ErrorOr<Bytes> read_some(Bytes) override
    {
        return Error::from_errno(EBADF);
    }

    virtual ErrorOr<size_t> write_some(ReadonlyBytes bytes) override
    {
        VERIFY(m_bit_offset == 0);
        return m_stream->write_some(bytes);
    }

    template<Unsigned T>
    ErrorOr<void> write_bits(T value, size_t bit_count)
    {
        VERIFY(m_bit_offset <= 7);

        while (bit_count > 0) {
            u8 next_bit = (value >> (bit_count - 1)) & 1;
            bit_count--;

            m_current_byte <<= 1;
            m_current_byte |= next_bit;
            m_bit_offset++;

            if (m_bit_offset > 7) {
                TRY(m_stream->write_value(m_current_byte));
                m_bit_offset = 0;
                m_current_byte = 0;
            }
        }

        return {};
    }

    virtual bool is_eof() const override
    {
        return true;
    }

    virtual bool is_open() const override
    {
        return m_stream->is_open();
    }

    virtual void close() override
    {
    }

    size_t bit_offset() const
    {
        return m_bit_offset;
    }

    ErrorOr<void> align_to_byte_boundary()
    {
        if (m_bit_offset == 0)
            return {};

        TRY(write_bits(0u, 8 - m_bit_offset));
        VERIFY(m_bit_offset == 0);
        return {};
    }

private:
    MaybeOwned<Stream> m_stream;
    u8 m_current_byte { 0 };
    size_t m_bit_offset { 0 };
};

}
