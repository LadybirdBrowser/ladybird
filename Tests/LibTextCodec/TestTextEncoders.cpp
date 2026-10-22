/*
 * Copyright (c) 2024, Ben Jilks <benjyjilks@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibTest/TestCase.h>
#include <LibTextCodec/Encoder.h>

TEST_CASE(test_utf8_encode)
{
    auto encoder = TextCodec::encoder_for("UTF-8"sv).value();
    // Unicode character U+1F600 GRINNING FACE
    auto test_string = "\U0001F600"sv;

    Vector<u8> processed_bytes;
    MUST(encoder.process(
        Utf8View(test_string),
        [&](ReadonlyBytes bytes) { return processed_bytes.try_append(bytes.data(), bytes.size()); },
        [&](u32) -> ErrorOr<void> { EXPECT(false); return {}; }));
    EXPECT(processed_bytes.size() == 4);
    EXPECT(processed_bytes[0] == 0xF0);
    EXPECT(processed_bytes[1] == 0x9F);
    EXPECT(processed_bytes[2] == 0x98);
    EXPECT(processed_bytes[3] == 0x80);
}

TEST_CASE(test_euc_jp_encoder)
{
    auto encoder = TextCodec::encoder_for("EUC-JP"sv).value();
    // U+A5 Yen Sign
    // U+3088 Hiragana Letter Yo
    // U+30C4 Katakana Letter Tu
    auto test_string = "\U000000A5\U00003088\U000030C4"sv;

    Vector<u8> processed_bytes;
    MUST(encoder.process(
        Utf8View(test_string),
        [&](ReadonlyBytes bytes) { return processed_bytes.try_append(bytes.data(), bytes.size()); },
        [&](u32) -> ErrorOr<void> { EXPECT(false); return {}; }));
    EXPECT(processed_bytes.size() == 5);
    EXPECT(processed_bytes[0] == 0x5C);
    EXPECT(processed_bytes[1] == 0xA4);
    EXPECT(processed_bytes[2] == 0xE8);
    EXPECT(processed_bytes[3] == 0xA5);
    EXPECT(processed_bytes[4] == 0xC4);
}

TEST_CASE(test_iso_2022_jp_encoder)
{
    auto encoder = TextCodec::encoder_for("ISO-2022-JP"sv).value();
    // U+A5 Yen Sign
    // U+3088 Hiragana Letter Yo
    // U+30C4 Katakana Letter Tu
    auto test_string = "\U000000A5\U00003088\U000030C4"sv;

    Vector<u8> processed_bytes;
    MUST(encoder.process(
        Utf8View(test_string),
        [&](ReadonlyBytes bytes) { return processed_bytes.try_append(bytes.data(), bytes.size()); },
        [&](u32) -> ErrorOr<void> { EXPECT(false); return {}; }));
    EXPECT(processed_bytes.size() == 14);
    EXPECT(processed_bytes[0] == 0x1B);
    EXPECT(processed_bytes[1] == 0x28);
    EXPECT(processed_bytes[2] == 0x4A);
    EXPECT(processed_bytes[3] == 0x5C);
    EXPECT(processed_bytes[4] == 0x1B);
    EXPECT(processed_bytes[5] == 0x24);
    EXPECT(processed_bytes[6] == 0x42);
    EXPECT(processed_bytes[7] == 0x24);
    EXPECT(processed_bytes[8] == 0x68);
    EXPECT(processed_bytes[9] == 0x25);
    EXPECT(processed_bytes[10] == 0x44);
    EXPECT(processed_bytes[11] == 0x1B);
    EXPECT(processed_bytes[12] == 0x28);
    EXPECT(processed_bytes[13] == 0x42);
}

TEST_CASE(test_shift_jis_encoder)
{
    auto encoder = TextCodec::encoder_for("Shift_JIS"sv).value();
    // U+A5 Yen Sign
    // U+3088 Hiragana Letter Yo
    // U+30C4 Katakana Letter Tu
    auto test_string = "\U000000A5\U00003088\U000030C4"sv;

    Vector<u8> processed_bytes;
    MUST(encoder.process(
        Utf8View(test_string),
        [&](ReadonlyBytes bytes) { return processed_bytes.try_append(bytes.data(), bytes.size()); },
        [&](u32) -> ErrorOr<void> { EXPECT(false); return {}; }));
    EXPECT(processed_bytes.size() == 5);
    EXPECT(processed_bytes[0] == 0x5C);
    EXPECT(processed_bytes[1] == 0x82);
    EXPECT(processed_bytes[2] == 0xE6);
    EXPECT(processed_bytes[3] == 0x83);
    EXPECT(processed_bytes[4] == 0x63);
}

TEST_CASE(test_euc_kr_encoder)
{
    auto encoder = TextCodec::encoder_for("EUC-KR"sv).value();
    // U+B29F Hangul Syllable Neulh
    // U+7C97 CJK Unified Ideograph-7C97
    auto test_string = "\U0000B29F\U00007C97"sv;

    Vector<u8> processed_bytes;
    MUST(encoder.process(
        Utf8View(test_string),
        [&](ReadonlyBytes bytes) { return processed_bytes.try_append(bytes.data(), bytes.size()); },
        [&](u32) -> ErrorOr<void> { EXPECT(false); return {}; }));
    EXPECT(processed_bytes.size() == 4);
    EXPECT(processed_bytes[0] == 0x88);
    EXPECT(processed_bytes[1] == 0x6B);
    EXPECT(processed_bytes[2] == 0xF0);
    EXPECT(processed_bytes[3] == 0xD8);
}

TEST_CASE(test_big5_encoder)
{
    auto encoder = TextCodec::encoder_for("Big5"sv).value();
    // U+A7 Section Sign
    // U+70D7 CJK Unified Ideograph-70D7
    auto test_string = "\U000000A7\U000070D7"sv;

    Vector<u8> processed_bytes;
    MUST(encoder.process(
        Utf8View(test_string),
        [&](ReadonlyBytes bytes) { return processed_bytes.try_append(bytes.data(), bytes.size()); },
        [&](u32) -> ErrorOr<void> { EXPECT(false); return {}; }));
    EXPECT(processed_bytes.size() == 4);
    EXPECT(processed_bytes[0] == 0xA1);
    EXPECT(processed_bytes[1] == 0xB1);
    EXPECT(processed_bytes[2] == 0xD2);
    EXPECT(processed_bytes[3] == 0x71);
}

TEST_CASE(test_gb18030_encoder)
{
    auto encoder = TextCodec::encoder_for("gb18030"sv).value();
    // U+20AC Euro Sign
    // U+E4C5 Private Use Area
    auto test_string = "\U000020AC\U0000E4C5"sv;

    Vector<u8> processed_bytes;
    MUST(encoder.process(
        Utf8View(test_string),
        [&](ReadonlyBytes bytes) { return processed_bytes.try_append(bytes.data(), bytes.size()); },
        [&](u32) -> ErrorOr<void> { EXPECT(false); return {}; }));
    EXPECT(processed_bytes.size() == 4);
    EXPECT(processed_bytes[0] == 0xA2);
    EXPECT(processed_bytes[1] == 0xE3);
    EXPECT(processed_bytes[2] == 0xFE);
    EXPECT(processed_bytes[3] == 0xFE);
}

TEST_CASE(test_windows1252_encoder)
{
    auto encoder = TextCodec::encoder_for("windows-1252"sv).value();
    auto test_string = "ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏfoo€"sv;
    Vector<u8> processed_bytes;
    MUST(encoder.process(
        Utf8View(test_string),
        [&](ReadonlyBytes bytes) { return processed_bytes.try_append(bytes.data(), bytes.size()); },
        [&](u32) -> ErrorOr<void> { EXPECT(false); return {}; }));
    EXPECT(processed_bytes.size() == 20);
    for (u8 i = 0; i < 15; i++) {
        EXPECT(processed_bytes[i] == (0xC0 + i));
    }
    EXPECT(processed_bytes[16] == 0x66);
    EXPECT(processed_bytes[17] == 0x6F);
    EXPECT(processed_bytes[18] == 0x6F);
    EXPECT(processed_bytes[19] == 0x80);
}

TEST_CASE(test_x_user_defined_encoder)
{
    auto encoder = TextCodec::encoder_for("x-user-defined"sv).value();
    auto test_string = "a\uF780\uF7FF\u0100"sv;
    Vector<u8> processed_bytes;
    Vector<u32> unmappable_code_points;
    MUST(encoder.process(
        Utf8View(test_string),
        [&](ReadonlyBytes bytes) { return processed_bytes.try_append(bytes.data(), bytes.size()); },
        [&](u32 code_point) { return unmappable_code_points.try_append(code_point); }));
    EXPECT_EQ(processed_bytes, (Vector<u8> { 0x61, 0x80, 0xFF }));
    EXPECT_EQ(unmappable_code_points, (Vector<u32> { 0x100 }));
}

TEST_CASE(test_encoder_output_larger_than_buffer)
{
    auto encoder = TextCodec::encoder_for("windows-1252"sv).value();
    StringBuilder builder;
    builder.append_repeated('a', 10000);
    builder.append("\u0100"sv);
    builder.append_repeated('b', 10000);
    auto test_string = builder.to_byte_string();

    StringBuilder processed_bytes;
    Vector<u32> unmappable_code_points;
    MUST(encoder.process(
        Utf8View(test_string),
        [&](ReadonlyBytes bytes) { return processed_bytes.try_append(StringView { bytes }); },
        [&](u32 code_point) { return unmappable_code_points.try_append(code_point); }));
    EXPECT_EQ(processed_bytes.string_view(), test_string.view().replace("\u0100"sv, ""sv, ReplaceMode::FirstOnly));
    EXPECT_EQ(unmappable_code_points, (Vector<u32> { 0x100 }));
}

TEST_CASE(test_encoder_uses_output_encoding)
{
    for (auto label : { "UTF-16LE"sv, "UTF-16BE"sv, "replacement"sv }) {
        auto encoder = TextCodec::encoder_for(label).value();
        Vector<u8> processed_bytes;
        MUST(encoder.process(
            Utf8View("\u00e9"sv),
            [&](ReadonlyBytes bytes) { return processed_bytes.try_append(bytes.data(), bytes.size()); },
            [&](u32) -> ErrorOr<void> { EXPECT(false); return {}; }));
        EXPECT_EQ(processed_bytes, (Vector<u8> { 0xC3, 0xA9 }));
    }
}
