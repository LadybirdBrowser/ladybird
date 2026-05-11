/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <LibCompress/Deflate.h>
#include <LibCompress/DeflateTables.h>

#include <zlib.h>

namespace Compress {

static constexpr u8 MAX_HUFFMAN_BITS = 15;

CanonicalCode const& CanonicalCode::fixed_literal_codes()
{
    static CanonicalCode code = MUST(CanonicalCode::from_bytes(fixed_literal_bit_lengths));
    return code;
}

CanonicalCode const& CanonicalCode::fixed_distance_codes()
{
    static CanonicalCode code = MUST(CanonicalCode::from_bytes(fixed_distance_bit_lengths));
    return code;
}

ErrorOr<CanonicalCode> CanonicalCode::from_bytes(ReadonlyBytes bytes)
{
    CanonicalCode code;

    auto non_zero_symbols = 0;
    auto last_non_zero = -1;
    for (size_t i = 0; i < bytes.size(); i++) {
        if (bytes[i] != 0) {
            non_zero_symbols++;
            last_non_zero = i;
        }
    }

    if (non_zero_symbols == 1) {
        code.m_prefix_table[0] = PrefixTableEntry { static_cast<u16>(last_non_zero), 1u };
        code.m_prefix_table[1] = code.m_prefix_table[0];
        code.m_max_prefixed_code_length = 1;

        if (code.m_bit_codes.size() < static_cast<size_t>(last_non_zero + 1)) {
            TRY(code.m_bit_codes.try_resize(last_non_zero + 1));
            TRY(code.m_bit_code_lengths.try_resize(last_non_zero + 1));
        }
        code.m_bit_codes[last_non_zero] = 0;
        code.m_bit_code_lengths[last_non_zero] = 1;

        return code;
    }

    struct PrefixCode {
        u16 symbol_code { 0 };
        u16 symbol_value { 0 };
        u16 code_length { 0 };
    };
    Array<PrefixCode, 1 << MAX_HUFFMAN_BITS> prefix_codes;
    size_t number_of_prefix_codes = 0;

    auto next_code = 0;
    for (size_t code_length = 1; code_length <= MAX_HUFFMAN_BITS; ++code_length) {
        next_code <<= 1;
        auto start_bit = 1 << code_length;

        for (size_t symbol = 0; symbol < bytes.size(); ++symbol) {
            if (bytes[symbol] != code_length)
                continue;

            if (next_code > start_bit)
                return Error::from_string_literal("Failed to decode code lengths");

            if (code_length <= MAX_HUFFMAN_BITS) {
                if (number_of_prefix_codes >= prefix_codes.size())
                    return Error::from_string_literal("Invalid canonical Huffman code");

                auto& prefix_code = prefix_codes[number_of_prefix_codes++];
                prefix_code.symbol_code = next_code;
                prefix_code.symbol_value = symbol;
                prefix_code.code_length = code_length;

                code.m_max_prefixed_code_length = code_length;
            } else {
                code.m_symbol_codes.append(start_bit | next_code);
                code.m_symbol_values.append(symbol);
            }

            if (code.m_bit_codes.size() < symbol + 1) {
                TRY(code.m_bit_codes.try_resize(symbol + 1));
                TRY(code.m_bit_code_lengths.try_resize(symbol + 1));
            }
            code.m_bit_codes[symbol] = fast_reverse16(start_bit | next_code, code_length);
            code.m_bit_code_lengths[symbol] = code_length;

            next_code++;
        }
    }

    if (next_code != (1 << MAX_HUFFMAN_BITS))
        return Error::from_string_literal("Failed to decode code lengths");

    for (size_t i = 0; i < number_of_prefix_codes; ++i) {
        auto& entry = prefix_codes[i];
        if (entry.code_length == 0 || entry.code_length > code.m_max_prefixed_code_length)
            continue;

        auto shift = code.m_max_prefixed_code_length - entry.code_length;
        u32 base_code = entry.symbol_code << shift;

        for (size_t j = 0; j < (1u << shift); ++j) {
            auto index = fast_reverse16(base_code + j, code.m_max_prefixed_code_length);
            code.m_prefix_table[index] = PrefixTableEntry { entry.symbol_value, entry.code_length };
        }
    }

    return code;
}

ErrorOr<u32> CanonicalCode::read_symbol(LittleEndianInputBitStream& stream) const
{
    if (m_max_prefixed_code_length == 0)
        return Error::from_string_literal("Cannot read symbol with zero code length");

    auto prefix = TRY(stream.peek_bits<size_t>(m_max_prefixed_code_length));

    if (auto [symbol_value, code_length] = m_prefix_table[prefix]; code_length != 0) {
        stream.discard_previously_peeked_bits(code_length);
        return symbol_value;
    }

    auto code_bits = TRY(stream.read_bits<u16>(m_max_prefixed_code_length));
    code_bits = fast_reverse16(code_bits, m_max_prefixed_code_length);
    code_bits |= 1 << m_max_prefixed_code_length;

    for (size_t i = m_max_prefixed_code_length; i < MAX_HUFFMAN_BITS; ++i) {
        size_t index;
        if (binary_search(m_symbol_codes.span(), code_bits, &index))
            return m_symbol_values[index];

        code_bits = code_bits << 1 | TRY(stream.read_bit());
    }

    return Error::from_string_literal("Symbol exceeds maximum symbol number");
}

ErrorOr<NonnullOwnPtr<DeflateDecompressor>> DeflateDecompressor::create(MaybeOwned<Stream> stream)
{
    auto buffer = TRY(AK::FixedArray<u8>::create(16 * 1024));
    auto zstream = TRY(GenericZlibDecompressor::new_z_stream(-MAX_WBITS));
    return adopt_nonnull_own_or_enomem(new (nothrow) DeflateDecompressor(move(buffer), move(stream), zstream));
}

ErrorOr<ByteBuffer> DeflateDecompressor::decompress_all(ReadonlyBytes bytes)
{
    return ::Compress::decompress_all<DeflateDecompressor>(bytes);
}

ErrorOr<NonnullOwnPtr<DeflateCompressor>> DeflateCompressor::create(MaybeOwned<Stream> stream, GenericZlibCompressionLevel compression_level)
{
    auto buffer = TRY(AK::FixedArray<u8>::create(16 * 1024));
    auto zstream = TRY(GenericZlibCompressor::new_z_stream(-MAX_WBITS, compression_level));
    return adopt_nonnull_own_or_enomem(new (nothrow) DeflateCompressor(move(buffer), move(stream), zstream));
}

ErrorOr<ByteBuffer> DeflateCompressor::compress_all(ReadonlyBytes bytes, GenericZlibCompressionLevel compression_level)
{
    return ::Compress::compress_all<DeflateCompressor>(bytes, compression_level);
}

} // namespace Compress
