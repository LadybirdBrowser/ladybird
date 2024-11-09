/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitStream.h>
#include <AK/IntegralMath.h>
#include <AK/MemoryStream.h>
#include <AK/Span.h>
#include <AK/TypeCasts.h>
#include <AK/Types.h>
#include <LibCompress/Deflate.h>
#include <LibCompress/Zlib.h>

namespace Compress {

ErrorOr<NonnullOwnPtr<ZlibDecompressor>> ZlibDecompressor::create(MaybeOwned<Stream> stream)
{
    auto header = TRY(stream->read_value<ZlibHeader>());

    if (header.compression_method != ZlibCompressionMethod::Deflate || header.compression_info > 7)
        return Error::from_string_literal("Non-DEFLATE compression inside Zlib is not supported");

    if (header.present_dictionary)
        return Error::from_string_literal("Zlib compression with a pre-defined dictionary is currently not supported");

    if (header.as_u16 % 31 != 0)
        return Error::from_string_literal("Zlib error correction code does not match");

    auto bit_stream = make<LittleEndianInputBitStream>(move(stream));
    auto deflate_stream = TRY(Compress::DeflateDecompressor::construct(move(bit_stream)));

    return adopt_nonnull_own_or_enomem(new (nothrow) ZlibDecompressor(header, move(deflate_stream)));
}

ZlibDecompressor::ZlibDecompressor(ZlibHeader header, NonnullOwnPtr<Stream> stream)
    : m_header(header)
    , m_stream(move(stream))
{
}

ErrorOr<Bytes> ZlibDecompressor::read_some(Bytes bytes)
{
    return m_stream->read_some(bytes);
}

ErrorOr<size_t> ZlibDecompressor::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

bool ZlibDecompressor::is_eof() const
{
    return m_stream->is_eof();
}

bool ZlibDecompressor::is_open() const
{
    return m_stream->is_open();
}

void ZlibDecompressor::close()
{
}

ErrorOr<NonnullOwnPtr<ZlibCompressor>> ZlibCompressor::construct(MaybeOwned<Stream> stream, ZlibCompressionLevel compression_level)
{
    // Zlib only defines Deflate as a compression method.
    auto compression_method = ZlibCompressionMethod::Deflate;

    // FIXME: Find a way to compress with Deflate's "Best" compression level.
    auto compressor_stream = TRY(DeflateCompressor::construct(MaybeOwned(*stream), static_cast<DeflateCompressor::CompressionLevel>(compression_level)));

    auto zlib_compressor = TRY(adopt_nonnull_own_or_enomem(new (nothrow) ZlibCompressor(move(stream), move(compressor_stream))));
    TRY(zlib_compressor->write_header(compression_method, compression_level));

    return zlib_compressor;
}

ZlibCompressor::ZlibCompressor(MaybeOwned<Stream> stream, NonnullOwnPtr<Stream> compressor_stream)
    : m_output_stream(move(stream))
    , m_compressor(move(compressor_stream))
{
}

ZlibCompressor::~ZlibCompressor()
{
    VERIFY(m_finished);
}

ErrorOr<void> ZlibCompressor::write_header(ZlibCompressionMethod compression_method, ZlibCompressionLevel compression_level)
{
    u8 compression_info = 0;
    if (compression_method == ZlibCompressionMethod::Deflate) {
        compression_info = AK::log2(DeflateCompressor::window_size) - 8;
        VERIFY(compression_info <= 7);
    }

    ZlibHeader header {
        .compression_method = compression_method,
        .compression_info = compression_info,
        .check_bits = 0,
        .present_dictionary = false,
        .compression_level = compression_level,
    };
    header.check_bits = 0b11111 - header.as_u16 % 31;

    // FIXME: Support pre-defined dictionaries.

    TRY(m_output_stream->write_value(header.as_u16));

    return {};
}

ErrorOr<Bytes> ZlibCompressor::read_some(Bytes)
{
    return Error::from_errno(EBADF);
}

ErrorOr<size_t> ZlibCompressor::write_some(ReadonlyBytes bytes)
{
    VERIFY(!m_finished);

    size_t n_written = TRY(m_compressor->write_some(bytes));
    m_adler32_checksum.update(bytes.trim(n_written));
    return n_written;
}

bool ZlibCompressor::is_eof() const
{
    return false;
}

bool ZlibCompressor::is_open() const
{
    return m_output_stream->is_open();
}

void ZlibCompressor::close()
{
}

ErrorOr<void> ZlibCompressor::finish()
{
    VERIFY(!m_finished);

    if (is<DeflateCompressor>(m_compressor.ptr()))
        TRY(static_cast<DeflateCompressor*>(m_compressor.ptr())->final_flush());

    NetworkOrdered<u32> adler_sum = m_adler32_checksum.digest();
    TRY(m_output_stream->write_value(adler_sum));

    m_finished = true;

    return {};
}

ErrorOr<ByteBuffer> ZlibCompressor::compress_all(ReadonlyBytes bytes, ZlibCompressionLevel compression_level)
{
    auto output_stream = TRY(try_make<AllocatingMemoryStream>());
    auto zlib_stream = TRY(ZlibCompressor::construct(MaybeOwned<Stream>(*output_stream), compression_level));

    TRY(zlib_stream->write_until_depleted(bytes));

    TRY(zlib_stream->finish());

    auto buffer = TRY(ByteBuffer::create_uninitialized(output_stream->used_buffer_size()));
    TRY(output_stream->read_until_filled(buffer.bytes()));

    return buffer;
}

}
