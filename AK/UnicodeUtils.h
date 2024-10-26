/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/Forward.h>

namespace AK::UnicodeUtils {

constexpr int bytes_to_store_code_point_in_utf8(u32 code_point)
{
    if (code_point <= 0x7f)
        return 1;
    if (code_point <= 0x7ff)
        return 2;
    if (code_point <= 0xffff)
        return 3;
    if (code_point <= 0x10ffff)
        return 4;
    return 0;
}

template<typename Callback>
[[nodiscard]] constexpr int code_point_to_utf8(u32 code_point, Callback callback)
{
    if (code_point <= 0x7f) {
        callback(static_cast<char>(code_point));
        return 1;
    } else if (code_point <= 0x07ff) {
        callback(static_cast<char>(((code_point >> 6) & 0x1f) | 0xc0));
        callback(static_cast<char>(((code_point >> 0) & 0x3f) | 0x80));
        return 2;
    } else if (code_point <= 0xffff) {
        callback(static_cast<char>(((code_point >> 12) & 0x0f) | 0xe0));
        callback(static_cast<char>(((code_point >> 6) & 0x3f) | 0x80));
        callback(static_cast<char>(((code_point >> 0) & 0x3f) | 0x80));
        return 3;
    } else if (code_point <= 0x10ffff) {
        callback(static_cast<char>(((code_point >> 18) & 0x07) | 0xf0));
        callback(static_cast<char>(((code_point >> 12) & 0x3f) | 0x80));
        callback(static_cast<char>(((code_point >> 6) & 0x3f) | 0x80));
        callback(static_cast<char>(((code_point >> 0) & 0x3f) | 0x80));
        return 4;
    }
    return -1;
}

template<FallibleFunction<char> Callback>
[[nodiscard]] ErrorOr<int> try_code_point_to_utf8(u32 code_point, Callback&& callback)
{
    if (code_point <= 0x7f) {
        TRY(callback(static_cast<char>(code_point)));
        return 1;
    }
    if (code_point <= 0x07ff) {
        TRY(callback(static_cast<char>((((code_point >> 6) & 0x1f) | 0xc0))));
        TRY(callback(static_cast<char>((((code_point >> 0) & 0x3f) | 0x80))));
        return 2;
    }
    if (code_point <= 0xffff) {
        TRY(callback(static_cast<char>((((code_point >> 12) & 0x0f) | 0xe0))));
        TRY(callback(static_cast<char>((((code_point >> 6) & 0x3f) | 0x80))));
        TRY(callback(static_cast<char>((((code_point >> 0) & 0x3f) | 0x80))));
        return 3;
    }
    if (code_point <= 0x10ffff) {
        TRY(callback(static_cast<char>((((code_point >> 18) & 0x07) | 0xf0))));
        TRY(callback(static_cast<char>((((code_point >> 12) & 0x3f) | 0x80))));
        TRY(callback(static_cast<char>((((code_point >> 6) & 0x3f) | 0x80))));
        TRY(callback(static_cast<char>((((code_point >> 0) & 0x3f) | 0x80))));
        return 4;
    }
    return -1;
}

/**
 * Compute the maximum number of UTF-8 bytes needed to store a given UTF-16 string, accounting for unmatched UTF-16 surrogates.
 * This function will overcount by at most 33%; 2 bytes for every valid UTF-16 codepoint between U+100000 and U+10FFFF.
 */
[[nodiscard]] static inline size_t maximum_utf8_length_from_utf16(ReadonlySpan<u16> code_units)
{
    // # UTF-8 code point -> no. UTF-8 bytes needed
    // U+0000   - U+007F   => 1 UTF-8 bytes
    // U+0080   - U+07FF   => 2 UTF-8 bytes
    // U+0800   - U+FFFF   => 3 UTF-8 bytes
    // U+010000 - U+10FFFF => 4 UTF-8 bytes

    // # UTF-16 code unit -> no. UTF-8 bytes needed
    // 0x0000 - 0x007f [U+000000 - U+00007F] = 1 UTF-8 bytes
    // 0x0080 - 0x07ff [U+000080 - U+0007FF] = 2 UTF-8 bytes
    // 0x0800 - 0xd7ff [U+000800 - U+00FFFF] = 3 UTF-8 bytes
    // 0xd800 - 0xdbff [U+010000 - U+10FFFF] = 4 UTF-8 bytes to encode valid UTF-16 code units,
    //                                         or 3 UTF-8 bytes to encode the unmatched surrogate code unit.
    // 0xdc00 - 0xdfff [U+010000 - U+10FFFF] = 0 UTF-8 bytes to encode valid UTF-16 code units (because it is already accounted for in 0xdc00 - 0xdfff),
    //                                         or 3 UTF-8 bytes to encode the unmatched surrogate code unit.
    // 0xe000 - 0xffff [U+00E000 - U+00FFFF] = 3 UTF-8 bytes

    // # UTF-16 code unit -> actual length added.
    // 0x0000 - 0x007f = 1
    // 0x0080 - 0x07ff = 2
    // 0x0800 - 0xd7ff = 3
    // 0xd800 - 0xdbff = 3
    //   ^ If the next code unit is 0xdc00 - 0xdfff, they will combined sum to 6, which is greater than the 4 required.
    //   Otherwise, 3 bytes are needed to encode U+D800 - U+DBFF.
    // 0xdc00 - 0xdfff = 3
    //   ^ If the previous code unit was, 0xd800 - 0xdbff, this will ensure that the combined sum is greater than 4.
    //   Otherwise, 3 bytes are needed to encode U+DC00 - U+DFFF.
    // 0xe000 - 0xffff = 3

    size_t maximum_utf8_length = 0;

    // NOTE: This loop is designed to be easy to vectorize.
    for (auto code_unit : code_units) {
        maximum_utf8_length += 1;
        maximum_utf8_length += code_unit > 0x007f;
        maximum_utf8_length += code_unit > 0x07ff;
    }

    return maximum_utf8_length;
}

}
