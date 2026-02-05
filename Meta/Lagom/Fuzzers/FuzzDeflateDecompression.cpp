/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Deflate.h>
#include <stdio.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);
    (void)Compress::DeflateDecompressor::decompress_all(ReadonlyBytes { data, size });
    return 0;
}
