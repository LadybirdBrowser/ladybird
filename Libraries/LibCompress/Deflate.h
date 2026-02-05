/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitStream.h>
#include <AK/Stream.h>
#include <LibCompress/Zlib.h>

namespace Compress {

class CanonicalCode {
public:
    CanonicalCode() = default;
    ErrorOr<u32> read_symbol(LittleEndianInputBitStream&) const;
    ErrorOr<void> write_symbol(LittleEndianOutputBitStream&, u32) const;

    static CanonicalCode const& fixed_literal_codes();
    static CanonicalCode const& fixed_distance_codes();

    static ErrorOr<CanonicalCode> from_bytes(ReadonlyBytes);

private:
    static constexpr size_t max_allowed_prefixed_code_length = 8;

    struct PrefixTableEntry {
        u16 symbol_value { 0 };
        u16 code_length { 0 };
    };

    // Decompression - indexed by code
    Vector<u16, 286> m_symbol_codes;
    Vector<u16, 286> m_symbol_values;

    Array<PrefixTableEntry, 1 << max_allowed_prefixed_code_length> m_prefix_table {};
    size_t m_max_prefixed_code_length { 0 };

    // Compression - indexed by symbol
    // Deflate uses a maximum of 288 symbols (maximum of 32 for distances),
    // but this is also used by webp, which can use up to 256 + 24 + (1 << 11) == 2328 symbols.
    Vector<u16, 288> m_bit_codes {};
    Vector<u16, 288> m_bit_code_lengths {};
};

ALWAYS_INLINE ErrorOr<void> CanonicalCode::write_symbol(LittleEndianOutputBitStream& stream, u32 symbol) const
{
    auto code = symbol < m_bit_codes.size() ? m_bit_codes[symbol] : 0u;
    auto length = symbol < m_bit_code_lengths.size() ? m_bit_code_lengths[symbol] : 0u;
    TRY(stream.write_bits(code, length));
    return {};
}

class DeflateDecompressor final : public GenericZlibDecompressor {
public:
    static ErrorOr<NonnullOwnPtr<DeflateDecompressor>> create(MaybeOwned<Stream>);
    static ErrorOr<ByteBuffer> decompress_all(ReadonlyBytes);

private:
    DeflateDecompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, z_stream* zstream)
        : GenericZlibDecompressor(move(buffer), move(stream), zstream)
    {
    }
};

class DeflateCompressor final : public GenericZlibCompressor {
public:
    static ErrorOr<NonnullOwnPtr<DeflateCompressor>> create(MaybeOwned<Stream>, GenericZlibCompressionLevel = GenericZlibCompressionLevel::Default);
    static ErrorOr<ByteBuffer> compress_all(ReadonlyBytes, GenericZlibCompressionLevel = GenericZlibCompressionLevel::Default);

private:
    DeflateCompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, z_stream* zstream)
        : GenericZlibCompressor(move(buffer), move(stream), zstream)
    {
    }
};

}
