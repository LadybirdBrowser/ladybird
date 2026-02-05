/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Gzip.h>
#include <stdio.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);
    auto input = ReadonlyBytes { data, size };

    auto compressed = MUST(Compress::GzipCompressor::compress_all(input));
    auto decompressed = MUST(Compress::GzipDecompressor::decompress_all(compressed));

    VERIFY(decompressed == input);

    return 0;
}
