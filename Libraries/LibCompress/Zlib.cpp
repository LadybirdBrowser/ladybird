/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Zlib.h>

#include <zlib.h>

namespace Compress {

ErrorOr<NonnullOwnPtr<ZlibDecompressor>> ZlibDecompressor::create(MaybeOwned<Stream> stream)
{
    auto buffer = TRY(AK::FixedArray<u8>::create(16 * 1024));
    auto zstream = TRY(GenericZlibDecompressor::new_z_stream(MAX_WBITS));
    return adopt_nonnull_own_or_enomem(new (nothrow) ZlibDecompressor(move(buffer), move(stream), zstream));
}

ErrorOr<ByteBuffer> ZlibDecompressor::decompress_all(ReadonlyBytes bytes)
{
    return ::Compress::decompress_all<ZlibDecompressor>(bytes);
}

ErrorOr<NonnullOwnPtr<ZlibCompressor>> ZlibCompressor::create(MaybeOwned<Stream> stream, GenericZlibCompressionLevel compression_level)
{
    auto buffer = TRY(AK::FixedArray<u8>::create(16 * 1024));
    auto zstream = TRY(GenericZlibCompressor::new_z_stream(MAX_WBITS, compression_level));
    return adopt_nonnull_own_or_enomem(new (nothrow) ZlibCompressor(move(buffer), move(stream), zstream));
}

ErrorOr<ByteBuffer> ZlibCompressor::compress_all(ReadonlyBytes bytes, GenericZlibCompressionLevel compression_level)
{
    return ::Compress::compress_all<ZlibCompressor>(bytes, compression_level);
}

}
