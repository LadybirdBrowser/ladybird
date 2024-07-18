/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibTextCodec/Decoder.h>

TEST_CASE(test_utf8_decode)
{
    auto decoder = TextCodec::UTF8Decoder();
    // Bytes for U+1F600 GRINNING FACE
    auto test_string = "\xf0\x9f\x98\x80"sv;

    EXPECT(decoder.validate(test_string));

    Vector<u32> processed_code_points;
    MUST(decoder.process(test_string, [&](u32 code_point) {
        return processed_code_points.try_append(code_point);
    }));
    EXPECT(processed_code_points.size() == 1);
    EXPECT(processed_code_points[0] == 0x1F600);

    EXPECT(MUST(decoder.to_utf8(test_string)) == test_string);
}

TEST_CASE(test_utf16be_decode)
{
    auto decoder = TextCodec::UTF16BEDecoder();
    // This is the output of `python3 -c "print('säk😀'.encode('utf-16be'))"`.
    auto test_string = "\x00s\x00\xe4\x00k\xd8=\xde\x00"sv;

    EXPECT(decoder.validate(test_string));

    Vector<u32> processed_code_points;
    MUST(decoder.process(test_string, [&](u32 code_point) {
        return processed_code_points.try_append(code_point);
    }));
    EXPECT(processed_code_points.size() == 4);
    EXPECT(processed_code_points[0] == 0x73);
    EXPECT(processed_code_points[1] == 0xE4);
    EXPECT(processed_code_points[2] == 0x6B);
    EXPECT(processed_code_points[3] == 0x1F600);

    auto utf8 = MUST(decoder.to_utf8(test_string));
    EXPECT_EQ(utf8, "säk😀"sv);
}

TEST_CASE(test_utf16le_decode)
{
    auto decoder = TextCodec::UTF16LEDecoder();
    // This is the output of `python3 -c "print('säk😀'.encode('utf-16le'))"`.
    auto test_string = "s\x00\xe4\x00k\x00=\xd8\x00\xde"sv;

    EXPECT(decoder.validate(test_string));

    Vector<u32> processed_code_points;
    MUST(decoder.process(test_string, [&](u32 code_point) {
        return processed_code_points.try_append(code_point);
    }));
    EXPECT(processed_code_points.size() == 4);
    EXPECT(processed_code_points[0] == 0x73);
    EXPECT(processed_code_points[1] == 0xE4);
    EXPECT(processed_code_points[2] == 0x6B);
    EXPECT(processed_code_points[3] == 0x1F600);

    auto utf8 = MUST(decoder.to_utf8(test_string));
    EXPECT_EQ(utf8, "säk😀"sv);
}
