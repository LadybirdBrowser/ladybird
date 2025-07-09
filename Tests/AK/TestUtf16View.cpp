/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
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
        EXPECT_EQ(MUST(view.to_utf8(AllowLonelySurrogates::Yes)), utf8_string);
        EXPECT_EQ(MUST(view.to_utf8(AllowLonelySurrogates::No)), utf8_string);
    }
    {
        Utf16View view { u"\xd83d"sv };
        EXPECT_EQ(MUST(view.to_utf8(AllowLonelySurrogates::Yes)), "\xed\xa0\xbd"sv);
        EXPECT(view.to_utf8(AllowLonelySurrogates::No).is_error());
    }
}

TEST_CASE(decode_utf16)
{
    Utf16View view { u"Привет, мир! 😀 γειά σου κόσμος こんにちは世界"sv };
    EXPECT_EQ(view.length_in_code_units(), 39uz);

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

TEST_CASE(utf16_code_unit_length_from_utf8)
{
    EXPECT_EQ(AK::utf16_code_unit_length_from_utf8(""sv), 0uz);
    EXPECT_EQ(AK::utf16_code_unit_length_from_utf8("abc"sv), 3uz);
    EXPECT_EQ(AK::utf16_code_unit_length_from_utf8("😀"sv), 2uz);
    EXPECT_EQ(AK::utf16_code_unit_length_from_utf8("Привет, мир! 😀 γειά σου κόσμος こんにちは世界"sv), 39uz);
}

TEST_CASE(null_view)
{
    Utf16View view;
    EXPECT(view.validate());
    EXPECT_EQ(view.length_in_code_units(), 0zu);
    EXPECT_EQ(view.length_in_code_points(), 0zu);
    EXPECT_EQ(MUST(view.to_utf8(AllowLonelySurrogates::No)), ""sv);
    EXPECT_EQ(MUST(view.to_utf8(AllowLonelySurrogates::Yes)), ""sv);

    for ([[maybe_unused]] auto it : view)
        FAIL("Iterating a null UTF-16 string should not produce any values");
}

TEST_CASE(utf16_literal)
{
    {
        Utf16View view { u""sv };
        EXPECT(view.validate());
        EXPECT_EQ(view.length_in_code_units(), 0u);
    }
    {
        Utf16View view { u"a"sv };
        EXPECT(view.validate());
        EXPECT_EQ(view.length_in_code_units(), 1u);
        EXPECT_EQ(view.code_unit_at(0), 0x61u);
    }
    {
        Utf16View view { u"abc"sv };
        EXPECT(view.validate());
        EXPECT_EQ(view.length_in_code_units(), 3u);
        EXPECT_EQ(view.code_unit_at(0), 0x61u);
        EXPECT_EQ(view.code_unit_at(1), 0x62u);
        EXPECT_EQ(view.code_unit_at(2), 0x63u);
    }
    {
        Utf16View view { u"🙃"sv };
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

    EXPECT_DEATH("Dereferencing Utf16CodePointIterator which is at its end.", *iterator);

    EXPECT_DEATH("Incrementing Utf16CodePointIterator which is at its end.", ++iterator);
}

TEST_CASE(validate_invalid_utf16)
{
    size_t valid_code_units = 0;
    Utf16View invalid;
    {
        // Lonely high surrogate.
        invalid = u"\xd800"sv;
        EXPECT(!invalid.validate(valid_code_units, AllowLonelySurrogates::No));
        EXPECT_EQ(valid_code_units, 0uz);

        EXPECT(invalid.validate(valid_code_units, AllowLonelySurrogates::Yes));
        EXPECT_EQ(valid_code_units, 1uz);

        invalid = u"\xdbff"sv;
        EXPECT(!invalid.validate(valid_code_units, AllowLonelySurrogates::No));
        EXPECT_EQ(valid_code_units, 0uz);

        EXPECT(invalid.validate(valid_code_units, AllowLonelySurrogates::Yes));
        EXPECT_EQ(valid_code_units, 1uz);
    }
    {
        // Lonely low surrogate.
        invalid = u"\xdc00"sv;
        EXPECT(!invalid.validate(valid_code_units, AllowLonelySurrogates::No));
        EXPECT_EQ(valid_code_units, 0uz);

        EXPECT(invalid.validate(valid_code_units, AllowLonelySurrogates::Yes));
        EXPECT_EQ(valid_code_units, 1uz);

        invalid = u"\xdfff"sv;
        EXPECT(!invalid.validate(valid_code_units, AllowLonelySurrogates::No));
        EXPECT_EQ(valid_code_units, 0uz);

        EXPECT(invalid.validate(valid_code_units, AllowLonelySurrogates::Yes));
        EXPECT_EQ(valid_code_units, 1uz);
    }
    {
        // High surrogate followed by non-surrogate.
        invalid = u"\xd800\x0000"sv;
        EXPECT(!invalid.validate(valid_code_units, AllowLonelySurrogates::No));
        EXPECT_EQ(valid_code_units, 0uz);

        EXPECT(invalid.validate(valid_code_units, AllowLonelySurrogates::Yes));
        EXPECT_EQ(valid_code_units, 2uz);

        invalid = u"\xd800\xe000"sv;
        EXPECT(!invalid.validate(valid_code_units, AllowLonelySurrogates::No));
        EXPECT_EQ(valid_code_units, 0uz);

        EXPECT(invalid.validate(valid_code_units, AllowLonelySurrogates::Yes));
        EXPECT_EQ(valid_code_units, 2uz);
    }
    {
        // High surrogate followed by high surrogate.
        invalid = u"\xd800\xd800"sv;
        EXPECT(!invalid.validate(valid_code_units, AllowLonelySurrogates::No));
        EXPECT_EQ(valid_code_units, 0uz);

        EXPECT(invalid.validate(valid_code_units, AllowLonelySurrogates::Yes));
        EXPECT_EQ(valid_code_units, 2uz);

        invalid = u"\xd800\xdbff"sv;
        EXPECT(!invalid.validate(valid_code_units, AllowLonelySurrogates::No));
        EXPECT_EQ(valid_code_units, 0uz);

        EXPECT(invalid.validate(valid_code_units, AllowLonelySurrogates::Yes));
        EXPECT_EQ(valid_code_units, 2uz);
    }
    {
        // Valid UTF-16 followed by invalid code units.
        invalid = u"\x0041\x0041\xd800"sv;
        EXPECT(!invalid.validate(valid_code_units, AllowLonelySurrogates::No));
        EXPECT_EQ(valid_code_units, 2uz);

        EXPECT(invalid.validate(valid_code_units, AllowLonelySurrogates::Yes));
        EXPECT_EQ(valid_code_units, 3uz);

        invalid = u"\x0041\x0041\xd800"sv;
        EXPECT(!invalid.validate(valid_code_units, AllowLonelySurrogates::No));
        EXPECT_EQ(valid_code_units, 2uz);

        EXPECT(invalid.validate(valid_code_units, AllowLonelySurrogates::Yes));
        EXPECT_EQ(valid_code_units, 3uz);
    }
}

TEST_CASE(decode_invalid_utf16)
{
    {
        // Lonely high surrogate.
        Utf16View view { u"AB\xd800"sv };
        EXPECT_EQ(view.length_in_code_units(), 3uz);

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
        Utf16View view { u"AB\xdc00"sv };
        EXPECT_EQ(view.length_in_code_units(), 3uz);

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
        Utf16View view { u"AB\xd800\x0000"sv };
        EXPECT_EQ(view.length_in_code_units(), 4uz);

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
        Utf16View view { u"AB\xd800\xd800"sv };
        EXPECT_EQ(view.length_in_code_units(), 4uz);

        auto expected = Array { (u32)0x41, 0x42, 0xfffd, 0xfffd };
        EXPECT_EQ(expected.size(), view.length_in_code_points());

        size_t i = 0;
        for (u32 code_point : view) {
            EXPECT_EQ(code_point, expected[i++]);
        }
        EXPECT_EQ(i, expected.size());
    }
}

TEST_CASE(is_ascii)
{
    EXPECT(Utf16View {}.is_ascii());
    EXPECT(u"a"sv.is_ascii());
    EXPECT(u"foo"sv.is_ascii());
    EXPECT(u"foo\t\n\rbar\v\b123"sv.is_ascii());
    EXPECT(u"The quick (\"brown\") fox can't jump 32.3 feet, right?"sv.is_ascii());

    EXPECT(!u"😀"sv.is_ascii());
    EXPECT(!u"foo 😀"sv.is_ascii());
    EXPECT(!u"😀 foo"sv.is_ascii());
    EXPECT(!u"The quick (“brown”) fox can’t jump 32.3 feet, right?"sv.is_ascii());
}

TEST_CASE(equals_ignoring_case)
{
    auto string1 = MUST(AK::utf8_to_utf16("foobar"sv));
    auto string2 = MUST(AK::utf8_to_utf16("FooBar"sv));
    EXPECT(Utf16View { string1 }.equals_ignoring_case(Utf16View { string2 }));

    string1 = MUST(AK::utf8_to_utf16(""sv));
    string2 = MUST(AK::utf8_to_utf16(""sv));
    EXPECT(Utf16View { string1 }.equals_ignoring_case(Utf16View { string2 }));

    string1 = MUST(AK::utf8_to_utf16(""sv));
    string2 = MUST(AK::utf8_to_utf16("FooBar"sv));
    EXPECT(!Utf16View { string1 }.equals_ignoring_case(Utf16View { string2 }));
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
        EXPECT_EQ(MUST(view.to_utf8(AllowLonelySurrogates::Yes)), "\xed\xa0\xbd"sv);
        EXPECT(view.to_utf8(AllowLonelySurrogates::No).is_error());
    }
}

TEST_CASE(starts_with)
{
    EXPECT(Utf16View {}.starts_with(u""sv));
    EXPECT(!Utf16View {}.starts_with(u" "sv));

    EXPECT(u"a"sv.starts_with(u""sv));
    EXPECT(u"a"sv.starts_with(u"a"sv));
    EXPECT(!u"a"sv.starts_with(u"b"sv));
    EXPECT(!u"a"sv.starts_with(u"ab"sv));

    EXPECT(u"abc"sv.starts_with(u""sv));
    EXPECT(u"abc"sv.starts_with(u"a"sv));
    EXPECT(u"abc"sv.starts_with(u"ab"sv));
    EXPECT(u"abc"sv.starts_with(u"abc"sv));
    EXPECT(!u"abc"sv.starts_with(u"b"sv));
    EXPECT(!u"abc"sv.starts_with(u"bc"sv));

    auto emoji = u"😀🙃"sv;

    EXPECT(emoji.starts_with(u""sv));
    EXPECT(emoji.starts_with(u"😀"sv));
    EXPECT(emoji.starts_with(u"😀🙃"sv));
    EXPECT(!emoji.starts_with(u"a"sv));
    EXPECT(!emoji.starts_with(u"🙃"sv));
}

TEST_CASE(find_code_unit_offset)
{
    auto conversion_result = MUST(AK::utf8_to_utf16("😀foo😀bar"sv));
    Utf16View const view { conversion_result };

    EXPECT_EQ(0u, view.find_code_unit_offset(u""sv).value());
    EXPECT_EQ(4u, view.find_code_unit_offset(u""sv, 4).value());
    EXPECT(!view.find_code_unit_offset(u""sv, 16).has_value());

    EXPECT_EQ(0u, view.find_code_unit_offset(u"😀"sv).value());
    EXPECT_EQ(5u, view.find_code_unit_offset(u"😀"sv, 1).value());
    EXPECT_EQ(2u, view.find_code_unit_offset(u"foo"sv).value());
    EXPECT_EQ(7u, view.find_code_unit_offset(u"bar"sv).value());

    EXPECT(!view.find_code_unit_offset(u"baz"sv).has_value());
}

TEST_CASE(find_code_unit_offset_ignoring_case)
{
    auto conversion_result = MUST(AK::utf8_to_utf16("😀Foo😀Bar"sv));
    Utf16View const view { conversion_result };

    EXPECT_EQ(0u, view.find_code_unit_offset_ignoring_case(u""sv).value());
    EXPECT_EQ(4u, view.find_code_unit_offset_ignoring_case(u""sv, 4).value());
    EXPECT(!view.find_code_unit_offset_ignoring_case(u""sv, 16).has_value());

    EXPECT_EQ(0u, view.find_code_unit_offset_ignoring_case(u"😀"sv).value());
    EXPECT_EQ(5u, view.find_code_unit_offset_ignoring_case(u"😀"sv, 1).value());
    EXPECT_EQ(2u, view.find_code_unit_offset_ignoring_case(u"foO"sv).value());
    EXPECT_EQ(7u, view.find_code_unit_offset_ignoring_case(u"baR"sv).value());
    EXPECT(!view.find_code_unit_offset_ignoring_case(u"baz"sv).has_value());
}
