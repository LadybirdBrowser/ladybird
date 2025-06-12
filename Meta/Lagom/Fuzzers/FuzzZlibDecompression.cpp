/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibCompress/Zlib.h>
#include <stdio.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);

    auto stream = make<FixedMemoryStream>(ReadonlyBytes { data, size });

    auto decompressor_or_error = Compress::ZlibDecompressor::create(move(stream));
    if (decompressor_or_error.is_error())
        return 0;
    auto decompressor = decompressor_or_error.release_value();
    (void)decompressor->read_until_eof();
    return 0;
}
