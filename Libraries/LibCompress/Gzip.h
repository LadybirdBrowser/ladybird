/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCompress/Zlib.h>

namespace Compress {

class GzipDecompressor final : public GenericZlibDecompressor {
public:
    bool is_likely_compressed(ReadonlyBytes bytes);

    static ErrorOr<NonnullOwnPtr<GzipDecompressor>> create(MaybeOwned<Stream>);
    static ErrorOr<ByteBuffer> decompress_all(ReadonlyBytes);

private:
    GzipDecompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, z_stream* zstream)
        : GenericZlibDecompressor(move(buffer), move(stream), zstream)
    {
    }
};

class GzipCompressor final : public GenericZlibCompressor {
public:
    static ErrorOr<NonnullOwnPtr<GzipCompressor>> create(MaybeOwned<Stream>, GenericZlibCompressionLevel = GenericZlibCompressionLevel::Default);
    static ErrorOr<ByteBuffer> compress_all(ReadonlyBytes, GenericZlibCompressionLevel = GenericZlibCompressionLevel::Default);

private:
    GzipCompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, z_stream* zstream)
        : GenericZlibCompressor(move(buffer), move(stream), zstream)
    {
    }
};

}
