/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <stdio.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);
    auto input = ReadonlyBytes { data, size };

    auto encoded = MUST(encode_base64(input));
    auto decoded = MUST(decode_base64(encoded));

    VERIFY(decoded == input);

    return 0;
}
