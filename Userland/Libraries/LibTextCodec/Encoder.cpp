/*
 * Copyright (c) 2024, Ben Jilks <benjyjilks@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/Utf8View.h>
#include <LibTextCodec/Decoder.h>
#include <LibTextCodec/Encoder.h>
#include <LibTextCodec/LookupTables.h>

namespace TextCodec {

namespace {
UTF8Encoder s_utf8_encoder;
Big5Encoder s_big5_encoder;
EUCJPEncoder s_euc_jp_encoder;
EUCKREncoder s_euc_kr_encoder;
}

Optional<Encoder&> encoder_for_exact_name(StringView encoding)
{
    if (encoding.equals_ignoring_ascii_case("utf-8"sv))
        return s_utf8_encoder;
    if (encoding.equals_ignoring_ascii_case("big5"sv))
        return s_big5_encoder;
    if (encoding.equals_ignoring_ascii_case("euc-jp"sv))
        return s_euc_jp_encoder;
    if (encoding.equals_ignoring_ascii_case("euc-kr"sv))
        return s_euc_kr_encoder;
    dbgln("TextCodec: No encoder implemented for encoding '{}'", encoding);
    return {};
}

Optional<Encoder&> encoder_for(StringView label)
{
    auto encoding = get_standardized_encoding(label);
    return encoding.has_value() ? encoder_for_exact_name(encoding.value()) : Optional<Encoder&> {};
}

// https://encoding.spec.whatwg.org/#utf-8-encoder
ErrorOr<void> UTF8Encoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte)
{
    ReadonlyBytes bytes { input.bytes(), input.byte_length() };
    for (auto byte : bytes)
        TRY(on_byte(byte));
    return {};
}

// https://encoding.spec.whatwg.org/#euc-jp-encoder
ErrorOr<void> EUCJPEncoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte)
{
    for (auto item : input) {
        // 1. If code point is end-of-queue, return finished.

        // 2. If code point is an ASCII code point, return a byte whose value is code point.
        if (is_ascii(item)) {
            TRY(on_byte(static_cast<u8>(item)));
            continue;
        }

        // 3. If code point is U+00A5, return byte 0x5C.
        if (item == 0x00A5) {
            TRY(on_byte(static_cast<u8>(0x5C)));
            continue;
        }

        // 4. If code point is U+203E, return byte 0x7E.
        if (item == 0x203E) {
            TRY(on_byte(static_cast<u8>(0x7E)));
            continue;
        }

        // 5. If code point is in the range U+FF61 to U+FF9F, inclusive, return two bytes whose values are 0x8E and code point − 0xFF61 + 0xA1.
        if (item >= 0xFF61 && item <= 0xFF9F) {
            TRY(on_byte(0x8E));
            TRY(on_byte(static_cast<u8>(item - 0xFF61 + 0xA1)));
            continue;
        }

        // 6. If code point is U+2212, set it to U+FF0D.
        if (item == 0x2212)
            item = 0xFF0D;

        // 7. Let pointer be the index pointer for code point in index jis0208.
        auto pointer = code_point_jis0208_index(item);

        // 8. If pointer is null, return error with code point.
        if (!pointer.has_value()) {
            // TODO: Report error.
            continue;
        }

        // 9. Let lead be pointer / 94 + 0xA1.
        auto lead = *pointer / 94 + 0xA1;

        // 10. Let trail be pointer % 94 + 0xA1.
        auto trail = *pointer % 94 + 0xA1;

        // 11. Return two bytes whose values are lead and trail.
        TRY(on_byte(static_cast<u8>(lead)));
        TRY(on_byte(static_cast<u8>(trail)));
    }

    return {};
}

// https://encoding.spec.whatwg.org/#euc-kr-encoder
ErrorOr<void> EUCKREncoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte)
{
    for (u32 item : input) {
        // 1. If code point is end-of-queue, return finished.

        // 2. If code point is an ASCII code point, return a byte whose value is code point.
        if (is_ascii(item)) {
            TRY(on_byte(static_cast<u8>(item)));
            continue;
        }

        // 3. Let pointer be the index pointer for code point in index EUC-KR.
        auto pointer = code_point_euc_kr_index(item);

        // 4. If pointer is null, return error with code point.
        if (!pointer.has_value()) {
            // TODO: Report error.
            continue;
        }

        // 5. Let lead be pointer / 190 + 0x81.
        auto lead = *pointer / 190 + 0x81;

        // 6. Let trail be pointer % 190 + 0x41.
        auto trail = *pointer % 190 + 0x41;

        // 7. Return two bytes whose values are lead and trail.
        TRY(on_byte(static_cast<u8>(lead)));
        TRY(on_byte(static_cast<u8>(trail)));
    }

    return {};
}

// https://encoding.spec.whatwg.org/#index-big5-pointer
static Optional<u32> index_big5_pointer(u32 code_point)
{
    // 1. Let index be index Big5 excluding all entries whose pointer is less than (0xA1 - 0x81) × 157.
    auto start_index = (0xA1 - 0x81) * 157 - s_big5_index_first_pointer;

    // 2. If code point is U+2550, U+255E, U+2561, U+256A, U+5341, or U+5345, return the last pointer
    //    corresponding to code point in index.
    if (Array<u32, 6> { 0x2550, 0x255E, 0x2561, 0x256A, 0x5341, 0x5345 }.contains_slow(code_point)) {
        for (u32 i = s_big5_index.size() - 1; i >= start_index; --i) {
            if (s_big5_index[i] == code_point) {
                return s_big5_index_first_pointer + i;
            }
        }
        return {};
    }

    // 3. Return the index pointer for code point in index.
    for (u32 i = start_index; i < s_big5_index.size(); ++i) {
        if (s_big5_index[i] == code_point) {
            return s_big5_index_first_pointer + i;
        }
    }
    return {};
}

// https://encoding.spec.whatwg.org/#big5-encoder
ErrorOr<void> Big5Encoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte)
{
    for (u32 item : input) {
        // 1. If code point is end-of-queue, return finished.

        // 2. If code point is an ASCII code point, return a byte whose value is code point.
        if (is_ascii(item)) {
            TRY(on_byte(static_cast<u8>(item)));
            continue;
        }

        // 3. Let pointer be the index Big5 pointer for code point.
        auto pointer = index_big5_pointer(item);

        // 4. If pointer is null, return error with code point.
        if (!pointer.has_value()) {
            // TODO: Report error.
            continue;
        }

        // 5. Let lead be pointer / 157 + 0x81.
        auto lead = *pointer / 157 + 0x81;

        // 6. Let trail be pointer % 157.
        auto trail = *pointer % 157;

        // 7. Let offset be 0x40 if trail is less than 0x3F, otherwise 0x62.
        auto offset = 0x62;
        if (trail < 0x3f)
            offset = 0x40;

        // 8. Return two bytes whose values are lead and trail + offset.
        TRY(on_byte(static_cast<u8>(lead)));
        TRY(on_byte(static_cast<u8>(trail + offset)));
    }

    return {};
}

}
