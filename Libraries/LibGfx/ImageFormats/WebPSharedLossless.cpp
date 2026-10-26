/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageFormats/WebPSharedLossless.h>

namespace Gfx {

ErrorOr<CanonicalCode> CanonicalCode::from_bytes(ReadonlyBytes bytes)
{
    auto non_zero_symbol_count = 0;
    auto last_non_zero_symbol = -1;
    for (size_t i = 0; i < bytes.size(); i++) {
        if (bytes[i] != 0) {
            non_zero_symbol_count++;
            last_non_zero_symbol = i;
        }
    }

    if (non_zero_symbol_count == 1)
        return CanonicalCode(last_non_zero_symbol);

    return CanonicalCode(TRY(Compress::CanonicalCode::from_bytes(bytes)));
}

}
