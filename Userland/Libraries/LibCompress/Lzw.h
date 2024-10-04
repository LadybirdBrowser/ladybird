/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitStream.h>
#include <AK/Concepts.h>
#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/IntegralMath.h>
#include <AK/MemoryStream.h>
#include <AK/Vector.h>

namespace Compress {

namespace Details {

class LzwState {
public:
    u16 add_control_code()
    {
        u16 const control_code = m_code_table.size();
        m_code_table.append(Vector<u8> {});
        m_original_code_table.append(Vector<u8> {});
        if (m_code_table.size() >= m_table_capacity && m_code_size < max_code_size) {
            ++m_code_size;
            ++m_original_code_size;
            m_table_capacity *= 2;
        }
        return control_code;
    }

    void reset()
    {
        m_code_table.clear();
        m_code_table.extend(m_original_code_table);
        m_code_size = m_original_code_size;
        m_table_capacity = AK::exp2<u32>(m_code_size);
    }

protected:
    static constexpr int max_code_size = 12;
    static constexpr int max_table_size = 1 << max_code_size;

    LzwState(u8 min_code_size, i32 offset_for_size_change)
        : m_code_size(min_code_size)
        , m_original_code_size(min_code_size)
        , m_table_capacity(AK::exp2<u32>(min_code_size))
        , m_offset_for_size_change(offset_for_size_change)
    {
        init_code_table();
    }

    void init_code_table()
    {
        m_code_table.ensure_capacity(m_table_capacity);
        for (u16 i = 0; i < m_table_capacity; ++i) {
            m_code_table.unchecked_append({ (u8)i });
        }
        m_original_code_table = m_code_table;
    }

    void extend_code_table(Vector<u8> const& entry)
    {
        if (entry.size() > 1 && m_code_table.size() < max_table_size) {
            m_code_table.append(entry);
            if (m_code_table.size() >= (m_table_capacity + m_offset_for_size_change) && m_code_size < max_code_size) {
                ++m_code_size;
                m_table_capacity *= 2;
            }
        }
    }

    Vector<Vector<u8>> m_code_table {};
    Vector<Vector<u8>> m_original_code_table {};

    u8 m_code_size { 0 };
    u8 m_original_code_size { 0 };

    u32 m_table_capacity { 0 };
    i32 m_offset_for_size_change {};
};

}

template<InputBitStream InputStream>
class LzwDecompressor : private Details::LzwState {
public:
    explicit LzwDecompressor(MaybeOwned<InputStream> lzw_stream, u8 min_code_size, i32 offset_for_size_change = 0)
        : LzwState(min_code_size, offset_for_size_change)
        , m_bit_stream(move(lzw_stream))

    {
    }

    static ErrorOr<ByteBuffer> decompress_all(ReadonlyBytes bytes, u8 initial_code_size, i32 offset_for_size_change = 0)
    {
        auto memory_stream = make<FixedMemoryStream>(bytes);
        auto lzw_stream = make<InputStream>(MaybeOwned<Stream>(move(memory_stream)));
        LzwDecompressor lzw_decompressor { MaybeOwned<InputStream> { move(lzw_stream) }, initial_code_size, offset_for_size_change };

        ByteBuffer decompressed;

        u16 const clear_code = lzw_decompressor.add_control_code();
        u16 const end_of_data_code = lzw_decompressor.add_control_code();

        while (true) {
            auto const code = TRY(lzw_decompressor.next_code());

            if (code == clear_code) {
                lzw_decompressor.reset();
                continue;
            }

            if (code == end_of_data_code)
                break;

            TRY(decompressed.try_append(lzw_decompressor.get_output()));
        }

        return decompressed;
    }

    void reset()
    {
        LzwState::reset();
        m_output.clear();
    }

    ErrorOr<u16> next_code()
    {
        m_current_code = TRY(m_bit_stream->template read_bits<u16>(m_code_size));

        if (m_current_code > m_code_table.size()) {
            dbgln_if(LZW_DEBUG, "Corrupted LZW stream, invalid code: {}, code table size: {}",
                m_current_code,
                m_code_table.size());
            return Error::from_string_literal("Corrupted LZW stream, invalid code");
        } else if (m_current_code == m_code_table.size() && m_output.is_empty()) {
            dbgln_if(LZW_DEBUG, "Corrupted LZW stream, valid new code but output buffer is empty: {}, code table size: {}",
                m_current_code,
                m_code_table.size());
            return Error::from_string_literal("Corrupted LZW stream, valid new code but output buffer is empty");
        }

        return m_current_code;
    }

    Vector<u8>& get_output()
    {
        VERIFY(m_current_code <= m_code_table.size());
        if (m_current_code < m_code_table.size()) {
            Vector<u8> new_entry = m_output;
            m_output = m_code_table.at(m_current_code);
            new_entry.append(m_output[0]);
            extend_code_table(new_entry);
        } else if (m_current_code == m_code_table.size()) {
            VERIFY(!m_output.is_empty());
            m_output.append(m_output[0]);
            extend_code_table(m_output);
        }
        return m_output;
    }

private:
    MaybeOwned<InputStream> m_bit_stream;

    u16 m_current_code { 0 };
    Vector<u8> m_output {};
};

class LzwCompressor : private Details::LzwState {
public:
    static ErrorOr<ByteBuffer> compress_all(ReadonlyBytes bytes, u8 initial_code_size)
    {
        LzwCompressor compressor { initial_code_size };
        AllocatingMemoryStream buffer;
        LittleEndianOutputBitStream output_stream { MaybeOwned<Stream>(buffer) };

        u16 const clear_code = compressor.add_control_code();
        u16 const end_of_data_code = compressor.add_control_code();

        TRY(output_stream.write_bits(clear_code, compressor.m_code_size));

        u32 last_offset = 0;

        while (last_offset < bytes.size()) {
            ReadonlyBytes current_symbol {};
            u16 current_code {};

            if (compressor.m_code_table.size() == max_table_size - 2) {
                TRY(output_stream.write_bits(clear_code, compressor.m_code_size));
                compressor.reset();
            }

            bool found_symbol = false;

            for (u32 symbol_size = 1; last_offset + symbol_size <= bytes.size(); ++symbol_size) {
                current_symbol = bytes.slice(last_offset, symbol_size);
                auto const new_code = compressor.code_for_symbol(current_symbol);

                if (new_code.has_value()) {
                    current_code = *new_code;
                } else {
                    found_symbol = true;
                    break;
                }
            }

            TRY(output_stream.write_bits(current_code, compressor.m_code_size));

            if (found_symbol) {
                compressor.extend_code_table(Vector(current_symbol));
                current_symbol = current_symbol.trim(current_symbol.size() - 1);
            }
            last_offset += current_symbol.size();
        }

        TRY(output_stream.write_bits(end_of_data_code, compressor.m_code_size));
        TRY(output_stream.align_to_byte_boundary());
        TRY(output_stream.flush_buffer_to_stream());

        return TRY(buffer.read_until_eof());
    }

private:
    LzwCompressor(u8 initial_code_size)
        : Details::LzwState(initial_code_size, 1)
    {
    }

    Optional<u16> code_for_symbol(ReadonlyBytes bytes)
    {
        for (u16 i = 0; i < m_code_table.size(); ++i) {
            if (m_code_table[i].span() == bytes)
                return i;
        }

        return OptionalNone {};
    }
};

}
