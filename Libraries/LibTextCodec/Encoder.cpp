/*
 * Copyright (c) 2024, Ben Jilks <benjyjilks@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/Error.h>
#include <AK/Utf8View.h>
#include <LibTextCodec/Decoder.h>
#include <LibTextCodec/Encoder.h>
#include <LibTextCodec/LookupTables.h>

namespace TextCodec {

namespace {
UTF8Encoder s_utf8_encoder;
GB18030Encoder s_gb18030_encoder;
GB18030Encoder s_gbk_encoder(GB18030Encoder::IsGBK::Yes);
Big5Encoder s_big5_encoder;
EUCJPEncoder s_euc_jp_encoder;
ISO2022JPEncoder s_iso_2022_jp_encoder;
ShiftJISEncoder s_shift_jis_encoder;
EUCKREncoder s_euc_kr_encoder;

// s_{encoding}_index is generated from https://encoding.spec.whatwg.org/indexes.json
// Found separately in https://encoding.spec.whatwg.org/index-{encoding}.txt
SingleByteEncoder s_ibm866_encoder { s_ibm866_index };
SingleByteEncoder s_latin2_encoder { s_iso_8859_2_index };
SingleByteEncoder s_latin3_encoder { s_iso_8859_3_index };
SingleByteEncoder s_latin4_encoder { s_iso_8859_4_index };
SingleByteEncoder s_latin_cyrillic_encoder { s_iso_8859_5_index };
SingleByteEncoder s_latin_arabic_encoder { s_iso_8859_6_index };
SingleByteEncoder s_latin_greek_encoder { s_iso_8859_7_index };
SingleByteEncoder s_latin_hebrew_encoder { s_iso_8859_8_index };
SingleByteEncoder s_latin6_encoder { s_iso_8859_10_index };
SingleByteEncoder s_latin7_encoder { s_iso_8859_13_index };
SingleByteEncoder s_latin8_encoder { s_iso_8859_14_index };
SingleByteEncoder s_latin9_encoder { s_iso_8859_15_index };
SingleByteEncoder s_latin10_encoder { s_iso_8859_16_index };
SingleByteEncoder s_centraleurope_encoder { s_windows_1250_index };
SingleByteEncoder s_cyrillic_encoder { s_windows_1251_index };
SingleByteEncoder s_hebrew_encoder { s_windows_1255_index };
SingleByteEncoder s_koi8r_encoder { s_koi8_r_index };
SingleByteEncoder s_koi8u_encoder { s_koi8_u_index };
SingleByteEncoder s_mac_roman_encoder { s_macintosh_index };
SingleByteEncoder s_windows874_encoder { s_windows_874_index };
SingleByteEncoder s_windows1252_encoder { s_windows_1252_index };
SingleByteEncoder s_windows1253_encoder { s_windows_1253_index };
SingleByteEncoder s_turkish_encoder { s_windows_1254_index };
SingleByteEncoder s_windows1256_encoder { s_windows_1256_index };
SingleByteEncoder s_windows1257_encoder { s_windows_1257_index };
SingleByteEncoder s_windows1258_encoder { s_windows_1258_index };
SingleByteEncoder s_mac_cyrillic_encoder { s_x_mac_cyrillic_index };

}

Optional<Encoder&> encoder_for_exact_name(StringView encoding)
{
    if (encoding.equals_ignoring_ascii_case("utf-8"sv))
        return s_utf8_encoder;
    if (encoding.equals_ignoring_ascii_case("big5"sv))
        return s_big5_encoder;
    if (encoding.equals_ignoring_ascii_case("euc-jp"sv))
        return s_euc_jp_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-2022-jp"sv))
        return s_iso_2022_jp_encoder;
    if (encoding.equals_ignoring_ascii_case("shift_jis"sv))
        return s_shift_jis_encoder;
    if (encoding.equals_ignoring_ascii_case("euc-kr"sv))
        return s_euc_kr_encoder;
    if (encoding.equals_ignoring_ascii_case("gb18030"sv))
        return s_gb18030_encoder;
    if (encoding.equals_ignoring_ascii_case("gbk"sv))
        return s_gbk_encoder;
    if (encoding.equals_ignoring_ascii_case("ibm866"sv))
        return s_ibm866_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-2"sv))
        return s_latin2_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-3"sv))
        return s_latin3_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-4"sv))
        return s_latin4_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-5"sv))
        return s_latin_cyrillic_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-6"sv))
        return s_latin_arabic_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-7"sv))
        return s_latin_greek_encoder;
    if (encoding.is_one_of_ignoring_ascii_case("iso-8859-8"sv, "iso-8859-8-i"sv))
        return s_latin_hebrew_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-10"sv))
        return s_latin6_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-13"sv))
        return s_latin7_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-14"sv))
        return s_latin8_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-15"sv))
        return s_latin9_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-16"sv))
        return s_latin10_encoder;
    if (encoding.equals_ignoring_ascii_case("koi8-r"sv))
        return s_koi8r_encoder;
    if (encoding.equals_ignoring_ascii_case("koi8-u"sv))
        return s_koi8u_encoder;
    if (encoding.equals_ignoring_ascii_case("macintosh"sv))
        return s_mac_roman_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-874"sv))
        return s_windows874_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1250"sv))
        return s_centraleurope_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1251"sv))
        return s_cyrillic_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1252"sv))
        return s_windows1252_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1253"sv))
        return s_windows1253_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1254"sv))
        return s_turkish_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1255"sv))
        return s_hebrew_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1256"sv))
        return s_windows1256_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1257"sv))
        return s_windows1257_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1258"sv))
        return s_windows1258_encoder;
    if (encoding.equals_ignoring_ascii_case("x-mac-cyrillic"sv))
        return s_mac_cyrillic_encoder;
    dbgln("TextCodec: No encoder implemented for encoding '{}'", encoding);
    return {};
}

Optional<Encoder&> encoder_for(StringView label)
{
    auto encoding = get_standardized_encoding(label);
    return encoding.has_value() ? encoder_for_exact_name(encoding.value()) : Optional<Encoder&> {};
}

// https://encoding.spec.whatwg.org/#utf-8-encoder
ErrorOr<void> UTF8Encoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)>)
{
    ReadonlyBytes bytes { input.bytes(), input.byte_length() };
    for (auto byte : bytes)
        TRY(on_byte(byte));
    return {};
}

// https://encoding.spec.whatwg.org/#euc-jp-encoder
ErrorOr<void> EUCJPEncoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error)
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
            TRY(on_error(item));
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

// https://encoding.spec.whatwg.org/#iso-2022-jp-encoder
ErrorOr<ISO2022JPEncoder::State> ISO2022JPEncoder::process_item(u32 item, State state, Function<ErrorOr<void>(u8)>& on_byte, Function<ErrorOr<void>(u32)>& on_error)
{
    // 3. If ISO-2022-JP encoder state is ASCII or Roman, and code point is U+000E, U+000F, or U+001B, return error with U+FFFD.
    if (state == State::ASCII || state == State::Roman) {
        if (item == 0x000E || item == 0x000F || item == 0x001B) {
            TRY(on_error(0xFFFD));
            return state;
        }
    }

    // 4. If ISO-2022-JP encoder state is ASCII and code point is an ASCII code point, return a byte whose value is code point.
    if (state == State::ASCII && is_ascii(item)) {
        TRY(on_byte(static_cast<u8>(item)));
        return state;
    }

    // 5. If ISO-2022-JP encoder state is Roman and code point is an ASCII code point, excluding U+005C and U+007E, or is U+00A5 or U+203E, then:
    if (state == State::Roman && ((is_ascii(item) && item != 0x005C && item != 0x007E) || (item == 0x00A5 || item == 0x203E))) {
        // 1. If code point is an ASCII code point, return a byte whose value is code point.
        if (is_ascii(item)) {
            TRY(on_byte(static_cast<u8>(item)));
            return state;
        }

        // 2. If code point is U+00A5, return byte 0x5C.
        if (item == 0x00A5) {
            TRY(on_byte(0x5C));
            return state;
        }

        // 3. If code point is U+203E, return byte 0x7E.
        if (item == 0x203E) {
            TRY(on_byte(0x7E));
            return state;
        }
    }

    // 6. If code point is an ASCII code point, and ISO-2022-JP encoder state is not ASCII, restore code point to ioQueue, set
    //    ISO-2022-JP encoder state to ASCII, and return three bytes 0x1B 0x28 0x42.
    if (is_ascii(item) && state != State::ASCII) {
        TRY(on_byte(0x1B));
        TRY(on_byte(0x28));
        TRY(on_byte(0x42));
        return process_item(item, State::ASCII, on_byte, on_error);
    }

    // 7. If code point is either U+00A5 or U+203E, and ISO-2022-JP encoder state is not Roman, restore code point to ioQueue,
    //    set ISO-2022-JP encoder state to Roman, and return three bytes 0x1B 0x28 0x4A.
    if ((item == 0x00A5 || item == 0x203E) && state != State::Roman) {
        TRY(on_byte(0x1B));
        TRY(on_byte(0x28));
        TRY(on_byte(0x4A));
        return process_item(item, State::Roman, on_byte, on_error);
    }

    // 8. If code point is U+2212, set it to U+FF0D.
    if (item == 0x2212)
        item = 0xFF0D;

    // 9. If code point is in the range U+FF61 to U+FF9F, inclusive, set it to the index code point for code point − 0xFF61
    //    in index ISO-2022-JP katakana.
    if (item >= 0xFF61 && item <= 0xFF9F) {
        item = *index_iso_2022_jp_katakana_code_point(item - 0xFF61);
    }

    // 10. Let pointer be the index pointer for code point in index jis0208.
    auto pointer = code_point_jis0208_index(item);

    // 11. If pointer is null, then:
    if (!pointer.has_value()) {
        // 1. If ISO-2022-JP encoder state is jis0208, then restore code point to ioQueue, set ISO-2022-JP encoder state to
        //    ASCII, and return three bytes 0x1B 0x28 0x42.
        if (state == State::jis0208) {
            TRY(on_byte(0x1B));
            TRY(on_byte(0x28));
            TRY(on_byte(0x4A));
            return process_item(item, State::ASCII, on_byte, on_error);
        }

        // 2. Return error with code point.
        TRY(on_error(item));
        return state;
    }

    // 12. If ISO-2022-JP encoder state is not jis0208, restore code point to ioQueue, set ISO-2022-JP encoder state to
    //     jis0208, and return three bytes 0x1B 0x24 0x42.
    if (state != State::jis0208) {
        TRY(on_byte(0x1B));
        TRY(on_byte(0x24));
        TRY(on_byte(0x42));
        return process_item(item, State::jis0208, on_byte, on_error);
    }

    // 13. Let lead be pointer / 94 + 0x21.
    auto lead = *pointer / 94 + 0x21;

    // 14. Let trail be pointer % 94 + 0x21.
    auto trail = *pointer % 94 + 0x21;

    // 15. Return two bytes whose values are lead and trail.
    TRY(on_byte(static_cast<u8>(lead)));
    TRY(on_byte(static_cast<u8>(trail)));
    return state;
}

// https://encoding.spec.whatwg.org/#iso-2022-jp-encoder
ErrorOr<void> ISO2022JPEncoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error)
{
    // ISO-2022-JP’s encoder has an associated ISO-2022-JP encoder state which is ASCII, Roman, or jis0208 (initially ASCII).
    auto state = State::ASCII;

    for (u32 item : input) {
        state = TRY(process_item(item, state, on_byte, on_error));
    }

    // 1. If code point is end-of-queue and ISO-2022-JP encoder state is not ASCII, set ISO-2022-JP
    //    encoder state to ASCII, and return three bytes 0x1B 0x28 0x42.
    if (state != State::ASCII) {
        state = State::ASCII;
        TRY(on_byte(0x1B));
        TRY(on_byte(0x28));
        TRY(on_byte(0x42));
        return {};
    }

    // 2. If code point is end-of-queue and ISO-2022-JP encoder state is ASCII, return finished.
    return {};
}

static Optional<u32> code_point_jis0208_index_skipping_range(u32 code_point, u32 skip_from, u32 skip_to)
{
    VERIFY(skip_to >= skip_from);
    for (u32 i = 0; i < s_jis0208_index.size(); ++i) {
        if (i >= skip_from && i <= skip_to)
            continue;
        if (s_jis0208_index[i] == code_point)
            return i;
    }
    return {};
}

// https://encoding.spec.whatwg.org/#index-shift_jis-pointer
static Optional<u32> index_shift_jis_pointer(u32 code_point)
{
    // 1. Let index be index jis0208 excluding all entries whose pointer is in the range 8272 to 8835, inclusive.
    auto pointer = code_point_jis0208_index_skipping_range(code_point, 8272, 8835);
    if (!pointer.has_value())
        return {};

    // 2. Return the index pointer for code point in index.
    return *pointer;
}

// https://encoding.spec.whatwg.org/#shift_jis-encoder
ErrorOr<void> ShiftJISEncoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error)
{
    for (u32 item : input) {
        // 1. If code point is end-of-queue, return finished.

        // 2. If code point is an ASCII code point or U+0080, return a byte whose value is code point.
        if (is_ascii(item) || item == 0x0080) {
            TRY(on_byte(static_cast<u8>(item)));
            continue;
        }

        // 3. If code point is U+00A5, return byte 0x5C.
        if (item == 0x00A5) {
            TRY(on_byte(0x5C));
            continue;
        }

        // 4. If code point is U+203E, return byte 0x7E.
        if (item == 0x203E) {
            TRY(on_byte(0x7E));
            continue;
        }

        // 5. If code point is in the range U+FF61 to U+FF9F, inclusive, return a byte whose value is code point − 0xFF61 + 0xA1.
        if (item >= 0xFF61 && item <= 0xFF9F) {
            TRY(on_byte(static_cast<u8>(item - 0xFF61 + 0xA1)));
            continue;
        }

        // 6. If code point is U+2212, set it to U+FF0D.
        if (item == 0x2212)
            item = 0xFF0D;

        // 7. Let pointer be the index Shift_JIS pointer for code point.
        auto pointer = index_shift_jis_pointer(item);

        // 8. If pointer is null, return error with code point.
        if (!pointer.has_value()) {
            TRY(on_error(item));
            continue;
        }

        // 9. Let lead be pointer / 188.
        auto lead = *pointer / 188;

        // 10. Let lead offset be 0x81 if lead is less than 0x1F, otherwise 0xC1.
        auto lead_offset = 0xC1;
        if (lead < 0x1F)
            lead_offset = 0x81;

        // 11. Let trail be pointer % 188.
        auto trail = *pointer % 188;

        // 12. Let offset be 0x40 if trail is less than 0x3F, otherwise 0x41.
        auto offset = 0x41;
        if (trail < 0x3F)
            offset = 0x40;

        // 13. Return two bytes whose values are lead + lead offset and trail + offset.
        TRY(on_byte(static_cast<u8>(lead + lead_offset)));
        TRY(on_byte(static_cast<u8>(trail + offset)));
    }

    return {};
}

// https://encoding.spec.whatwg.org/#euc-kr-encoder
ErrorOr<void> EUCKREncoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error)
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
            TRY(on_error(item));
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
ErrorOr<void> Big5Encoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error)
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
            TRY(on_error(item));
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

// https://encoding.spec.whatwg.org/#index-gb18030-ranges-pointer
static u32 index_gb18030_ranges_pointer(u32 code_point)
{
    // 1. If code point is U+E7C7, return pointer 7457.
    if (code_point == 0xe7c7)
        return 7457;

    // 2. Let offset be the last code point in index gb18030 ranges that is less than
    //    or equal to code point and let pointer offset be its corresponding pointer.
    size_t last_index;
    binary_search(s_gb18030_ranges, code_point, &last_index, [](auto const code_point, auto const& entry) {
        return code_point - entry.code_point;
    });
    auto offset = s_gb18030_ranges[last_index].code_point;
    auto pointer_offset = s_gb18030_ranges[last_index].pointer;

    // 3. Return a pointer whose value is pointer offset + code point − offset.
    return pointer_offset + code_point - offset;
}

GB18030Encoder::GB18030Encoder(IsGBK is_gbk)
    : m_is_gbk(is_gbk)
{
}

// https://encoding.spec.whatwg.org/#gb18030-encoder
ErrorOr<void> GB18030Encoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error)
{
    bool gbk = (m_is_gbk == IsGBK::Yes);

    for (u32 item : input) {
        // 1. If code point is end-of-queue, return finished.

        // 2. If code point is an ASCII code point, return a byte whose value is code point.
        if (is_ascii(item)) {
            TRY(on_byte(static_cast<u8>(item)));
            continue;
        }

        // 3. If code point is U+E5E5, return error with code point.
        if (item == 0xE5E5) {
            TRY(on_error(item));
            continue;
        }

        // 4. If is GBK is true and code point is U+20AC, return byte 0x80.
        if (gbk && item == 0x20AC) {
            TRY(on_byte(0x80));
            continue;
        }

        // 5. Let pointer be the index pointer for code point in index gb18030.
        auto pointer = code_point_gb18030_index(item);

        // 6. If pointer is non-null, then:
        if (pointer.has_value()) {
            // 1. Let lead be pointer / 190 + 0x81.
            auto lead = *pointer / 190 + 0x81;

            // 2. Let trail be pointer % 190.
            auto trail = *pointer % 190;

            // 3. Let offset be 0x40 if trail is less than 0x3F, otherwise 0x41.
            auto offset = 0x41;
            if (trail < 0x3f)
                offset = 0x40;

            // 4. Return two bytes whose values are lead and trail + offset.
            TRY(on_byte(static_cast<u8>(lead)));
            TRY(on_byte(static_cast<u8>(trail + offset)));
            continue;
        }

        // 7. If is GBK is true, return error with code point.
        if (gbk) {
            TRY(on_error(item));
            continue;
        }

        // 8. Set pointer to the index gb18030 ranges pointer for code point.
        pointer = index_gb18030_ranges_pointer(item);

        // 9. Let byte1 be pointer / (10 × 126 × 10).
        auto byte1 = *pointer / (10 * 126 * 10);

        // 10. Set pointer to pointer % (10 × 126 × 10).
        pointer = *pointer % (10 * 126 * 10);

        // 11. Let byte2 be pointer / (10 × 126).
        auto byte2 = *pointer / (10 * 126);

        // 12. Set pointer to pointer % (10 × 126).
        pointer = *pointer % (10 * 126);

        // 13. Let byte3 be pointer / 10.
        auto byte3 = *pointer / 10;

        // 14. Let byte4 be pointer % 10.
        auto byte4 = *pointer % 10;

        // 15. Return four bytes whose values are byte1 + 0x81, byte2 + 0x30, byte3 + 0x81, byte4 + 0x30.
        TRY(on_byte(static_cast<u8>(byte1 + 0x81)));
        TRY(on_byte(static_cast<u8>(byte2 + 0x30)));
        TRY(on_byte(static_cast<u8>(byte3 + 0x81)));
        TRY(on_byte(static_cast<u8>(byte4 + 0x30)));
    }

    return {};
}

// https://encoding.spec.whatwg.org/#single-byte-encoder
template<Integral ArrayType>
ErrorOr<void> SingleByteEncoder<ArrayType>::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error)
{
    for (u32 const code_point : input) {
        if (code_point < 0x80) {
            // 2. If code point is an ASCII code point, return a byte whose value is code point.
            TRY(on_byte(static_cast<u8>(code_point)));
        } else {
            Optional<u8> pointer = {};
            for (u8 i = 0; i < m_translation_table.size(); i++) {
                if (m_translation_table[i] == code_point) {
                    // 3. Let pointer be the index pointer for code point in index single-byte.
                    pointer = i;
                    break;
                }
            }
            if (pointer.has_value()) {
                // 5. Return a byte whose value is pointer + 0x80.
                TRY(on_byte(pointer.value() + 0x80));
            } else {
                // 4. If pointer is null, return error with code point.
                TRY(on_error(code_point));
            }
        }
    }
    // 1. If code point is end-of-queue, return finished.
    return {};
}

}
