/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Deflate.h>
#include <LibCompress/DeflateTables.h>

namespace Compress {

ErrorOr<CanonicalCode> CanonicalCode::from_bytes(ReadonlyBytes bytes)
{
    CanonicalCode code;

    auto non_zero_symbols = 0;
    auto last_non_zero = -1;
    for (size_t i = 0; i < bytes.size(); i++) {
        if (bytes[i] != 0) {
            non_zero_symbols++;
            last_non_zero = i;
        }
    }

    if (non_zero_symbols == 1) { // special case - only 1 symbol
        if (code.m_bit_codes.size() < static_cast<size_t>(last_non_zero + 1)) {
            TRY(code.m_bit_codes.try_resize(last_non_zero + 1));
            TRY(code.m_bit_code_lengths.try_resize(last_non_zero + 1));
        }
        code.m_bit_codes[last_non_zero] = 0;
        code.m_bit_code_lengths[last_non_zero] = 1;

        return code;
    }

    auto next_code = 0;
    for (size_t code_length = 1; code_length <= 15; ++code_length) {
        next_code <<= 1;
        auto start_bit = 1 << code_length;

        for (size_t symbol = 0; symbol < bytes.size(); ++symbol) {
            if (bytes[symbol] != code_length)
                continue;

            if (next_code > start_bit)
                return Error::from_string_literal("Failed to decode code lengths");

            if (code.m_bit_codes.size() < symbol + 1) {
                TRY(code.m_bit_codes.try_resize(symbol + 1));
                TRY(code.m_bit_code_lengths.try_resize(symbol + 1));
            }
            code.m_bit_codes[symbol] = fast_reverse16(start_bit | next_code, code_length); // DEFLATE writes huffman encoded symbols as lsb-first
            code.m_bit_code_lengths[symbol] = code_length;

            next_code++;
        }
    }

    if (next_code != (1 << 15))
        return Error::from_string_literal("Failed to decode code lengths");

    return code;
}

}
