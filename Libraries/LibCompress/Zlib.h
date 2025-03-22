/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/MaybeOwned.h>
#include <AK/Stream.h>
#include <LibCompress/GenericZlib.h>

namespace Compress {

class ZlibDecompressor final : public GenericZlibDecompressor {
public:
    static ErrorOr<NonnullOwnPtr<ZlibDecompressor>> create(MaybeOwned<Stream>);
    static ErrorOr<ByteBuffer> decompress_all(ReadonlyBytes);

private:
    ZlibDecompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, z_stream* zstream)
        : GenericZlibDecompressor(move(buffer), move(stream), zstream)
    {
    }
};

class ZlibCompressor final : public GenericZlibCompressor {
public:
    static ErrorOr<NonnullOwnPtr<ZlibCompressor>> create(MaybeOwned<Stream>, GenericZlibCompressionLevel = GenericZlibCompressionLevel::Default);
    static ErrorOr<ByteBuffer> compress_all(ReadonlyBytes, GenericZlibCompressionLevel = GenericZlibCompressionLevel::Default);

private:
    ZlibCompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, z_stream* zstream)
        : GenericZlibCompressor(move(buffer), move(stream), zstream)
    {
    }
};

}
