/*
 * Copyright (c) 2022, Michiel Visser <opensource@webmichiel.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitStream.h>
#include <AK/CircularQueue.h>
#include <AK/FixedArray.h>
#include <AK/Vector.h>

namespace Compress {

namespace Brotli {

class CanonicalCode {
public:
    CanonicalCode() = default;
    CanonicalCode(Vector<size_t> codes, Vector<size_t> values)
        : m_symbol_codes(move(codes))
        , m_symbol_values(move(values)) {};

    static ErrorOr<CanonicalCode> read_prefix_code(LittleEndianInputBitStream&, size_t alphabet_size);
    static ErrorOr<CanonicalCode> read_simple_prefix_code(LittleEndianInputBitStream&, size_t alphabet_size);
    static ErrorOr<CanonicalCode> read_complex_prefix_code(LittleEndianInputBitStream&, size_t alphabet_size, size_t hskip);

    ErrorOr<size_t> read_symbol(LittleEndianInputBitStream&) const;

private:
    static ErrorOr<size_t> read_complex_prefix_code_length(LittleEndianInputBitStream&);

    Vector<size_t> m_symbol_codes;
    Vector<size_t> m_symbol_values;
};

}

class BrotliDecompressionStream : public Stream {
    using CanonicalCode = Brotli::CanonicalCode;

public:
    enum class State {
        WindowSize,
        Idle,
        UncompressedData,
        CompressedCommand,
        CompressedLiteral,
        CompressedDistance,
        CompressedCopy,
        CompressedDictionary,
    };

    struct Block {
        size_t type;
        size_t type_previous;
        size_t number_of_types;

        size_t length;

        CanonicalCode type_code;
        CanonicalCode length_code;
    };

    class LookbackBuffer {
    private:
        LookbackBuffer(FixedArray<u8>& buffer)
            : m_buffer(move(buffer))
        {
        }

    public:
        static ErrorOr<LookbackBuffer> try_create(size_t size)
        {
            auto buffer = TRY(FixedArray<u8>::create(size));
            return LookbackBuffer { buffer };
        }

        void write(u8 value)
        {
            m_buffer[m_offset] = value;
            m_offset = (m_offset + 1) % m_buffer.size();
            m_total_written++;
        }

        u8 lookback(size_t offset) const
        {
            VERIFY(offset <= m_total_written);
            VERIFY(offset <= m_buffer.size());
            size_t index = (m_offset + m_buffer.size() - offset) % m_buffer.size();
            return m_buffer[index];
        }

        u8 lookback(size_t offset, u8 fallback) const
        {
            if (offset > m_total_written || offset > m_buffer.size())
                return fallback;
            VERIFY(offset <= m_total_written);
            VERIFY(offset <= m_buffer.size());
            size_t index = (m_offset + m_buffer.size() - offset) % m_buffer.size();
            return m_buffer[index];
        }

        size_t total_written() { return m_total_written; }

    private:
        FixedArray<u8> m_buffer;
        size_t m_offset { 0 };
        size_t m_total_written { 0 };
    };

public:
    BrotliDecompressionStream(MaybeOwned<Stream>);

    ErrorOr<Bytes> read_some(Bytes output_buffer) override;
    ErrorOr<size_t> write_some(ReadonlyBytes bytes) override { return m_input_stream.write_some(bytes); }
    bool is_eof() const override;
    bool is_open() const override { return m_input_stream.is_open(); }
    void close() override { m_input_stream.close(); }

private:
    ErrorOr<size_t> read_window_length();
    ErrorOr<size_t> read_size_number_of_nibbles();
    ErrorOr<size_t> read_variable_length();

    ErrorOr<void> read_context_map(size_t number_of_codes, Vector<u8>& context_map, size_t context_map_size);
    ErrorOr<void> read_block_configuration(Block&);

    ErrorOr<void> block_update_length(Block&);
    ErrorOr<void> block_read_new_state(Block&);

    size_t literal_code_index_from_context();

    LittleEndianInputBitStream m_input_stream;
    State m_current_state { State::WindowSize };
    Optional<LookbackBuffer> m_lookback_buffer;

    size_t m_window_size { 0 };
    bool m_read_final_block { false };
    size_t m_postfix_bits { 0 };
    size_t m_direct_distances { 0 };
    size_t m_distances[4] { 4, 11, 15, 16 };

    size_t m_bytes_left { 0 };
    size_t m_insert_length { 0 };
    size_t m_copy_length { 0 };
    bool m_implicit_zero_distance { false };
    size_t m_distance { 0 };
    ByteBuffer m_dictionary_data;

    Block m_literal_block;
    Vector<u8> m_literal_context_modes;
    Block m_insert_and_copy_block;
    Block m_distance_block;

    Vector<u8> m_context_mapping_literal;
    Vector<u8> m_context_mapping_distance;

    Vector<CanonicalCode> m_literal_codes;
    Vector<CanonicalCode> m_insert_and_copy_codes;
    Vector<CanonicalCode> m_distance_codes;
};

}
