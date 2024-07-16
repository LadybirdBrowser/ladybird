/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Array.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Utf16View.h>

TEST_CASE(decode_ascii)
{
    auto string = MUST(AK::utf8_to_utf16("Hello World!11"sv));
    Utf16View view { string };

    size_t valid_code_units = 0;
    EXPECT(view.validate(valid_code_units));
    EXPECT_EQ(valid_code_units, view.length_in_code_units());

    auto expected = Array { (u32)72, 101, 108, 108, 111, 32, 87, 111, 114, 108, 100, 33, 49, 49 };
    EXPECT_EQ(expected.size(), view.length_in_code_points());

    size_t i = 0;
    for (u32 code_point : view) {
        EXPECT_EQ(code_point, expected[i++]);
    }
    EXPECT_EQ(i, expected.size());
}

TEST_CASE(decode_utf8)
{
    auto string = MUST(AK::utf8_to_utf16("Привет, мир! 😀 γειά σου κόσμος こんにちは世界"sv));
    Utf16View view { string };

    size_t valid_code_units = 0;
    EXPECT(view.validate(valid_code_units));
    EXPECT_EQ(valid_code_units, view.length_in_code_units());

    auto expected = Array { (u32)1055, 1088, 1080, 1074, 1077, 1090, 44, 32, 1084, 1080, 1088, 33, 32, 128512, 32, 947, 949, 953, 940, 32, 963, 959, 965, 32, 954, 972, 963, 956, 959, 962, 32, 12371, 12435, 12395, 12385, 12399, 19990, 30028 };
    EXPECT_EQ(expected.size(), view.length_in_code_points());

    size_t i = 0;
    for (u32 code_point : view) {
        EXPECT_EQ(code_point, expected[i++]);
    }
    EXPECT_EQ(i, expected.size());
}

TEST_CASE(encode_utf8)
{
    {
        auto utf8_string = "Привет, мир! 😀 γειά σου κόσμος こんにちは世界"_string;
        auto string = MUST(AK::utf8_to_utf16(utf8_string));
        Utf16View view { string };
        EXPECT_EQ(MUST(view.to_utf8(Utf16View::AllowInvalidCodeUnits::Yes)), utf8_string);
        EXPECT_EQ(MUST(view.to_utf8(Utf16View::AllowInvalidCodeUnits::No)), utf8_string);
    }
    {
        auto encoded = Array { (u16)0xd83d };
        Utf16View view { encoded };
        EXPECT_EQ(MUST(view.to_utf8(Utf16View::AllowInvalidCodeUnits::Yes)), "\xed\xa0\xbd"sv);
        EXPECT(view.to_utf8(Utf16View::AllowInvalidCodeUnits::No).is_error());
    }
}

TEST_CASE(decode_utf16)
{
    // Same string as the decode_utf8 test.
    auto encoded = Array { (u16)0x041f, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442, 0x002c, 0x0020, 0x043c, 0x0438, 0x0440, 0x0021, 0x0020, 0xd83d, 0xde00, 0x0020, 0x03b3, 0x03b5, 0x03b9, 0x03ac, 0x0020, 0x03c3, 0x03bf, 0x03c5, 0x0020, 0x03ba, 0x03cc, 0x03c3, 0x03bc, 0x03bf, 0x03c2, 0x0020, 0x3053, 0x3093, 0x306b, 0x3061, 0x306f, 0x4e16, 0x754c };

    Utf16View view { encoded };
    EXPECT_EQ(encoded.size(), view.length_in_code_units());

    size_t valid_code_units = 0;
    EXPECT(view.validate(valid_code_units));
    EXPECT_EQ(valid_code_units, view.length_in_code_units());

    auto expected = Array { (u32)1055, 1088, 1080, 1074, 1077, 1090, 44, 32, 1084, 1080, 1088, 33, 32, 128512, 32, 947, 949, 953, 940, 32, 963, 959, 965, 32, 954, 972, 963, 956, 959, 962, 32, 12371, 12435, 12395, 12385, 12399, 19990, 30028 };
    EXPECT_EQ(expected.size(), view.length_in_code_points());

    size_t i = 0;
    for (u32 code_point : view) {
        EXPECT_EQ(code_point, expected[i++]);
    }
    EXPECT_EQ(i, expected.size());
}

TEST_CASE(utf16_literal)
{
    {
        Utf16View view { u"" };
        EXPECT(view.validate());
        EXPECT_EQ(view.length_in_code_units(), 0u);
    }
    {
        Utf16View view { u"a" };
        EXPECT(view.validate());
        EXPECT_EQ(view.length_in_code_units(), 1u);
        EXPECT_EQ(view.code_unit_at(0), 0x61u);
    }
    {
        Utf16View view { u"abc" };
        EXPECT(view.validate());
        EXPECT_EQ(view.length_in_code_units(), 3u);
        EXPECT_EQ(view.code_unit_at(0), 0x61u);
        EXPECT_EQ(view.code_unit_at(1), 0x62u);
        EXPECT_EQ(view.code_unit_at(2), 0x63u);
    }
    {
        Utf16View view { u"🙃" };
        EXPECT(view.validate());
        EXPECT_EQ(view.length_in_code_units(), 2u);
        EXPECT_EQ(view.code_unit_at(0), 0xd83du);
        EXPECT_EQ(view.code_unit_at(1), 0xde43u);
    }
}

TEST_CASE(iterate_utf16)
{
    auto string = MUST(AK::utf8_to_utf16("Привет 😀"sv));
    Utf16View view { string };
    auto iterator = view.begin();

    EXPECT(*iterator == 1055);
    EXPECT(iterator.length_in_code_units() == 1);

    EXPECT(++iterator != view.end());
    EXPECT(*iterator == 1088);
    EXPECT(iterator.length_in_code_units() == 1);

    EXPECT(++iterator != view.end());
    EXPECT(*iterator == 1080);
    EXPECT(iterator.length_in_code_units() == 1);

    EXPECT(++iterator != view.end());
    EXPECT(*iterator == 1074);
    EXPECT(iterator.length_in_code_units() == 1);

    EXPECT(++iterator != view.end());
    EXPECT(*iterator == 1077);
    EXPECT(iterator.length_in_code_units() == 1);

    EXPECT(++iterator != view.end());
    EXPECT(*iterator == 1090);
    EXPECT(iterator.length_in_code_units() == 1);

    EXPECT(++iterator != view.end());
    EXPECT(*iterator == 32);
    EXPECT(iterator.length_in_code_units() == 1);

    EXPECT(++iterator != view.end());
    EXPECT(*iterator == 128512);
    EXPECT(iterator.length_in_code_units() == 2);

    EXPECT(++iterator == view.end());
    EXPECT_CRASH("Dereferencing Utf16CodePointIterator which is at its end.", [&iterator] {
        *iterator;
        return Test::Crash::Failure::DidNotCrash;
    });

    EXPECT_CRASH("Incrementing Utf16CodePointIterator which is at its end.", [&iterator] {
        ++iterator;
        return Test::Crash::Failure::DidNotCrash;
    });
}

TEST_CASE(validate_invalid_utf16)
{
    size_t valid_code_units = 0;
    {
        // Lonely high surrogate.
        auto invalid = Array { (u16)0xd800 };
        EXPECT(!Utf16View(invalid).validate(valid_code_units));
        EXPECT(valid_code_units == 0);

        invalid = Array { (u16)0xdbff };
        EXPECT(!Utf16View(invalid).validate(valid_code_units));
        EXPECT(valid_code_units == 0);
    }
    {
        // Lonely low surrogate.
        auto invalid = Array { (u16)0xdc00 };
        EXPECT(!Utf16View(invalid).validate(valid_code_units));
        EXPECT(valid_code_units == 0);

        invalid = Array { (u16)0xdfff };
        EXPECT(!Utf16View(invalid).validate(valid_code_units));
        EXPECT(valid_code_units == 0);
    }
    {
        // High surrogate followed by non-surrogate.
        auto invalid = Array { (u16)0xd800, 0 };
        EXPECT(!Utf16View(invalid).validate(valid_code_units));
        EXPECT(valid_code_units == 0);

        invalid = Array { (u16)0xd800, 0xe000 };
        EXPECT(!Utf16View(invalid).validate(valid_code_units));
        EXPECT(valid_code_units == 0);
    }
    {
        // High surrogate followed by high surrogate.
        auto invalid = Array { (u16)0xd800, 0xd800 };
        EXPECT(!Utf16View(invalid).validate(valid_code_units));
        EXPECT(valid_code_units == 0);

        invalid = Array { (u16)0xd800, 0xdbff };
        EXPECT(!Utf16View(invalid).validate(valid_code_units));
        EXPECT(valid_code_units == 0);
    }
    {
        // Valid UTF-16 followed by invalid code units.
        auto invalid = Array { (u16)0x41, 0x41, 0xd800 };
        EXPECT(!Utf16View(invalid).validate(valid_code_units));
        EXPECT(valid_code_units == 2);

        invalid = Array { (u16)0x41, 0x41, 0xd800 };
        EXPECT(!Utf16View(invalid).validate(valid_code_units));
        EXPECT(valid_code_units == 2);
    }
}

TEST_CASE(decode_invalid_utf16)
{
    {
        // Lonely high surrogate.
        auto invalid = Array { (u16)0x41, 0x42, 0xd800 };

        Utf16View view { invalid };
        EXPECT_EQ(invalid.size(), view.length_in_code_units());

        auto expected = Array { (u32)0x41, 0x42, 0xfffd };
        EXPECT_EQ(expected.size(), view.length_in_code_points());

        size_t i = 0;
        for (u32 code_point : view) {
            EXPECT_EQ(code_point, expected[i++]);
        }
        EXPECT_EQ(i, expected.size());
    }
    {
        // Lonely low surrogate.
        auto invalid = Array { (u16)0x41, 0x42, 0xdc00 };

        Utf16View view { invalid };
        EXPECT_EQ(invalid.size(), view.length_in_code_units());

        auto expected = Array { (u32)0x41, 0x42, 0xfffd };
        EXPECT_EQ(expected.size(), view.length_in_code_points());

        size_t i = 0;
        for (u32 code_point : view) {
            EXPECT_EQ(code_point, expected[i++]);
        }
        EXPECT_EQ(i, expected.size());
    }
    {
        // High surrogate followed by non-surrogate.
        auto invalid = Array { (u16)0x41, 0x42, 0xd800, 0 };

        Utf16View view { invalid };
        EXPECT_EQ(invalid.size(), view.length_in_code_units());

        auto expected = Array { (u32)0x41, 0x42, 0xfffd, 0 };
        EXPECT_EQ(expected.size(), view.length_in_code_points());

        size_t i = 0;
        for (u32 code_point : view) {
            EXPECT_EQ(code_point, expected[i++]);
        }
        EXPECT_EQ(i, expected.size());
    }
    {
        // High surrogate followed by high surrogate.
        auto invalid = Array { (u16)0x41, 0x42, 0xd800, 0xd800 };

        Utf16View view { invalid };
        EXPECT_EQ(invalid.size(), view.length_in_code_units());

        auto expected = Array { (u32)0x41, 0x42, 0xfffd, 0xfffd };
        EXPECT_EQ(expected.size(), view.length_in_code_points());

        size_t i = 0;
        for (u32 code_point : view) {
            EXPECT_EQ(code_point, expected[i++]);
        }
        EXPECT_EQ(i, expected.size());
    }
}

TEST_CASE(substring_view)
{
    auto string = MUST(AK::utf8_to_utf16("Привет 😀"sv));
    {
        Utf16View view { string };
        view = view.substring_view(7, 2);

        EXPECT(view.length_in_code_units() == 2);
        EXPECT_EQ(MUST(view.to_utf8()), "😀"sv);
    }
    {
        Utf16View view { string };
        view = view.substring_view(7, 1);

        EXPECT(view.length_in_code_units() == 1);
        EXPECT_EQ(MUST(view.to_utf8(Utf16View::AllowInvalidCodeUnits::Yes)), "\xed\xa0\xbd"sv);
        EXPECT(view.to_utf8(Utf16View::AllowInvalidCodeUnits::No).is_error());
    }
}

TEST_CASE(starts_with)
{
    EXPECT(Utf16View {}.starts_with(u""));
    EXPECT(!Utf16View {}.starts_with(u" "));

    EXPECT(Utf16View { u"a" }.starts_with(u""));
    EXPECT(Utf16View { u"a" }.starts_with(u"a"));
    EXPECT(!Utf16View { u"a" }.starts_with(u"b"));
    EXPECT(!Utf16View { u"a" }.starts_with(u"ab"));

    EXPECT(Utf16View { u"abc" }.starts_with(u""));
    EXPECT(Utf16View { u"abc" }.starts_with(u"a"));
    EXPECT(Utf16View { u"abc" }.starts_with(u"ab"));
    EXPECT(Utf16View { u"abc" }.starts_with(u"abc"));
    EXPECT(!Utf16View { u"abc" }.starts_with(u"b"));
    EXPECT(!Utf16View { u"abc" }.starts_with(u"bc"));

    auto emoji = Utf16View { u"😀🙃" };

    EXPECT(emoji.starts_with(u""));
    EXPECT(emoji.starts_with(u"😀"));
    EXPECT(emoji.starts_with(u"😀🙃"));
    EXPECT(!emoji.starts_with(u"a"));
    EXPECT(!emoji.starts_with(u"🙃"));
}
