/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PackBitsDecoder.h"
#include <AK/MemoryStream.h>

namespace Compress::PackBits {

ErrorOr<ByteBuffer> decode_all(ReadonlyBytes bytes, Optional<u64> expected_output_size, CompatibilityMode mode)
{
    // This implementation uses unsigned values for the selector, as described in the PDF spec.
    // Note that this remains compatible with other implementations based on signed numbers.

    auto memory_stream = make<FixedMemoryStream>(bytes);

    ByteBuffer decoded_bytes;

    if (expected_output_size.has_value())
        TRY(decoded_bytes.try_ensure_capacity(*expected_output_size));

    while (memory_stream->remaining() > 0 && decoded_bytes.size() < expected_output_size.value_or(NumericLimits<u64>::max())) {
        auto const length = TRY(memory_stream->read_value<u8>());

        if (length < 128) {
            for (u8 i = 0; i <= length; ++i)
                TRY(decoded_bytes.try_append(TRY(memory_stream->read_value<u8>())));
        } else if (length > 128) {
            auto const next_byte = TRY(memory_stream->read_value<u8>());

            for (u8 i = 0; i < 257 - length; ++i)
                TRY(decoded_bytes.try_append(next_byte));
        } else {
            VERIFY(length == 128);
            if (mode == CompatibilityMode::PDF)
                break;
        }
    }

    return decoded_bytes;
}

}
