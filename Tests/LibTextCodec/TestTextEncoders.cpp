/*
 * Copyright (c) 2024, Ben Jilks <benjyjilks@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibTextCodec/Encoder.h>

TEST_CASE(test_utf8_encode)
{
    TextCodec::UTF8Encoder encoder;
    // Unicode character U+1F600 GRINNING FACE
    auto test_string = "\U0001F600"sv;

    Vector<u8> processed_bytes;
    MUST(encoder.process(Utf8View(test_string), [&](u8 byte) {
        return processed_bytes.try_append(byte);
    }));
    EXPECT(processed_bytes.size() == 4);
    EXPECT(processed_bytes[0] == 0xF0);
    EXPECT(processed_bytes[1] == 0x9F);
    EXPECT(processed_bytes[2] == 0x98);
    EXPECT(processed_bytes[3] == 0x80);
}

TEST_CASE(test_euc_jp_encoder)
{
    TextCodec::EUCJPEncoder encoder;
    // U+A5 Yen Sign
    // U+3088 Hiragana Letter Yo
    // U+30C4 Katakana Letter Tu
    auto test_string = "\U000000A5\U00003088\U000030C4"sv;

    Vector<u8> processed_bytes;
    MUST(encoder.process(Utf8View(test_string), [&](u8 byte) {
        return processed_bytes.try_append(byte);
    }));
    EXPECT(processed_bytes.size() == 5);
    EXPECT(processed_bytes[0] == 0x5C);
    EXPECT(processed_bytes[1] == 0xA4);
    EXPECT(processed_bytes[2] == 0xE8);
    EXPECT(processed_bytes[3] == 0xA5);
    EXPECT(processed_bytes[4] == 0xC4);
}

TEST_CASE(test_euc_kr_encoder)
{
    TextCodec::EUCKREncoder encoder;
    // U+B29F Hangul Syllable Neulh
    // U+7C97 CJK Unified Ideograph-7C97
    auto test_string = "\U0000B29F\U00007C97"sv;

    Vector<u8> processed_bytes;
    MUST(encoder.process(Utf8View(test_string), [&](u8 byte) {
        return processed_bytes.try_append(byte);
    }));
    EXPECT(processed_bytes.size() == 4);
    EXPECT(processed_bytes[0] == 0x88);
    EXPECT(processed_bytes[1] == 0x6B);
    EXPECT(processed_bytes[2] == 0xF0);
    EXPECT(processed_bytes[3] == 0xD8);
}

TEST_CASE(test_big5_encoder)
{
    TextCodec::Big5Encoder encoder;
    // U+A7 Section Sign
    // U+70D7 CJK Unified Ideograph-70D7
    auto test_string = "\U000000A7\U000070D7"sv;

    Vector<u8> processed_bytes;
    MUST(encoder.process(Utf8View(test_string), [&](u8 byte) {
        return processed_bytes.try_append(byte);
    }));
    EXPECT(processed_bytes.size() == 4);
    EXPECT(processed_bytes[0] == 0xA1);
    EXPECT(processed_bytes[1] == 0xB1);
    EXPECT(processed_bytes[2] == 0xD2);
    EXPECT(processed_bytes[3] == 0x71);
}

TEST_CASE(test_gb18030_encoder)
{
    TextCodec::GB18030Encoder encoder;
    // U+20AC Euro Sign
    // U+E4C5 Private Use Area
    auto test_string = "\U000020AC\U0000E4C5"sv;

    Vector<u8> processed_bytes;
    MUST(encoder.process(Utf8View(test_string), [&](u8 byte) {
        return processed_bytes.try_append(byte);
    }));

    EXPECT(processed_bytes.size() == 4);
    EXPECT(processed_bytes[0] == 0xA2);
    EXPECT(processed_bytes[1] == 0xE3);
    EXPECT(processed_bytes[2] == 0xFE);
    EXPECT(processed_bytes[3] == 0xFE);
}
