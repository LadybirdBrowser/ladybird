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

ErrorOr<u32> CanonicalCode::read_symbol(LittleEndianInputBitStream& bit_stream) const
{
    return TRY(m_code.visit(
        [](u32 single_code) -> ErrorOr<u32> { return single_code; },
        [&bit_stream](Compress::CanonicalCode const& code) { return code.read_symbol(bit_stream); }));
}

}
