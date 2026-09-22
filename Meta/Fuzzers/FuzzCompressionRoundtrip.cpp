/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/BoundedInput.h"
#include <AK/Array.h>
#include <AK/MemoryStream.h>
#include <LibCompress/Brotli.h>
#include <LibCompress/Deflate.h>
#include <LibCompress/Gzip.h>
#include <LibCompress/Zlib.h>

template<typename Compressor, typename Decompressor>
static void roundtrip(ReadonlyBytes original, size_t chunk_size)
{
    AllocatingMemoryStream compressed;
    auto compressor = MUST(Compressor::create(MaybeOwned<Stream> { compressed }));
    auto remaining = original;
    while (!remaining.is_empty()) {
        auto count = min(remaining.size(), chunk_size);
        MUST(compressor->write_until_depleted(remaining.slice(0, count)));
        remaining = remaining.slice(count);
    }
    MUST(compressor->finish());
    auto wire = MUST(ByteBuffer::create_uninitialized(compressed.used_buffer_size()));
    MUST(compressed.read_until_filled(wire.bytes()));
    Fuzzing::ChunkedInputStream input(wire.bytes(), chunk_size);
    auto decoder = MUST(Decompressor::create(MaybeOwned<Stream> { input }));
    Array<u8, 4096> buffer;
    size_t offset = 0;
    // Only self-generated valid streams use an exact-output/termination oracle.
    for (size_t reads = 0; !decoder->is_eof(); ++reads) {
        VERIFY(reads <= original.size() + wire.size() + 16);
        auto output = MUST(decoder->read_some(buffer.span().slice(0, min(chunk_size, buffer.size()))));
        VERIFY(output.size() <= original.size() - offset);
        VERIFY(original.slice(offset, output.size()) == output);
        offset += output.size();
        // Reading only a header/trailer can consume input without producing bytes.
        // The iteration budget, not a nonempty-output assumption, checks progress.
    }
    VERIFY(offset == original.size());
}

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    if (size < 2 || size > 16384)
        return 0;
    Fuzzing::BoundedInput input({ data, size });
    auto format = input.byte() % 4;
    auto chunk = size_t { 1 } << (input.byte() % 13);
    switch (format) {
    case 0:
        roundtrip<Compress::DeflateCompressor, Compress::DeflateDecompressor>(input.remaining(), chunk);
        break;
    case 1:
        roundtrip<Compress::ZlibCompressor, Compress::ZlibDecompressor>(input.remaining(), chunk);
        break;
    case 2:
        roundtrip<Compress::GzipCompressor, Compress::GzipDecompressor>(input.remaining(), chunk);
        break;
    case 3:
        roundtrip<Compress::BrotliCompressor, Compress::BrotliDecompressor>(input.remaining(), chunk);
        break;
    }
    return 0;
}
