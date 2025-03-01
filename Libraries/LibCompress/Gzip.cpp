/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Gzip.h>

#include <zlib.h>

namespace Compress {

ErrorOr<NonnullOwnPtr<GzipDecompressor>> GzipDecompressor::create(MaybeOwned<Stream> stream)
{
    auto zstream = TRY(GenericZlibDecompressor::new_z_stream(MAX_WBITS | 16));
    return adopt_nonnull_own_or_enomem(new (nothrow) GzipDecompressor(move(stream), zstream));
}

ErrorOr<ByteBuffer> GzipDecompressor::decompress_all(ReadonlyBytes bytes)
{
    return ::Compress::decompress_all<GzipDecompressor>(bytes);
}

ErrorOr<NonnullOwnPtr<GzipCompressor>> GzipCompressor::create(MaybeOwned<Stream> stream, GenericZlibCompressionLevel compression_level)
{
    auto zstream = TRY(GenericZlibCompressor::new_z_stream(MAX_WBITS | 16, compression_level));
    return adopt_nonnull_own_or_enomem(new (nothrow) GzipCompressor(move(stream), zstream));
}

ErrorOr<ByteBuffer> GzipCompressor::compress_all(ReadonlyBytes bytes, GenericZlibCompressionLevel compression_level)
{
    return ::Compress::compress_all<GzipCompressor>(bytes, compression_level);
}

}
