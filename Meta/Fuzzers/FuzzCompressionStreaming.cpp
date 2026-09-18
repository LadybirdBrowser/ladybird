/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/BoundedInput.h"
#include <AK/Array.h>
#include <LibCompress/Brotli.h>
#include <LibCompress/Deflate.h>
#include <LibCompress/Gzip.h>
#include <LibCompress/Zlib.h>

template<typename Decoder>
static void exercise(ReadonlyBytes bytes, size_t input_chunk, size_t output_chunk, size_t stop_after)
{
    Fuzzing::ChunkedInputStream input(bytes, input_chunk);
    auto result = Decoder::create(MaybeOwned<Stream> { input });
    if (result.is_error())
        return;
    auto decoder = result.release_value();
    Array<u8, 4096> output;
    size_t total = 0;
    for (size_t reads = 0; reads < stop_after && total < 1024 * 1024 && !decoder->is_eof(); ++reads) {
        auto read = decoder->read_some(output.span().slice(0, min(output_chunk, 1024 * 1024 - total)));
        if (read.is_error())
            break;
        total += read.value().size();
    }
    // close() is currently a no-op in these wrappers; destruction still exercises
    // cleanup after partial consumption. This is not Web DecompressionStream cancel.
    if (stop_after != 4096)
        decoder->close();
}

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    if (size < 4 || size > 65536)
        return 0;
    Fuzzing::BoundedInput input({ data, size });
    auto format = input.byte() % 4;
    auto input_chunk = size_t { 1 } << (input.byte() % 13);
    auto output_chunk = size_t { 1 } << (input.byte() % 13);
    auto stop = input.byte();
    auto stop_after = stop & 1 ? size_t { 4096 } : static_cast<size_t>(stop / 2);
    switch (format) {
    case 0:
        exercise<Compress::DeflateDecompressor>(input.remaining(), input_chunk, output_chunk, stop_after);
        break;
    case 1:
        exercise<Compress::ZlibDecompressor>(input.remaining(), input_chunk, output_chunk, stop_after);
        break;
    case 2:
        exercise<Compress::GzipDecompressor>(input.remaining(), input_chunk, output_chunk, stop_after);
        break;
    case 3:
        exercise<Compress::BrotliDecompressor>(input.remaining(), input_chunk, output_chunk, stop_after);
        break;
    }
    return 0;
}
