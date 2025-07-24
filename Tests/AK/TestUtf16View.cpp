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
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>

TEST_CASE(decode_ascii)
{
    auto string = Utf16String::from_utf8("Hello World!11"sv);
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
    auto string = Utf16String::from_utf8("Привет, мир! 😀 γειά σου κόσμος こんにちは世界"sv);
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
        auto string = Utf16String::from_utf8(utf8_string);
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
    auto string = Utf16String::from_utf8("Привет 😀"sv);
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

TEST_CASE(is_ascii_whitespace)
{
    EXPECT(Utf16View {}.is_ascii_whitespace());
    EXPECT(u" "sv.is_ascii_whitespace());
    EXPECT(u"\t"sv.is_ascii_whitespace());
    EXPECT(u"\r"sv.is_ascii_whitespace());
    EXPECT(u"\n"sv.is_ascii_whitespace());
    EXPECT(u" \t\r\n\v "sv.is_ascii_whitespace());

    EXPECT(!u"a"sv.is_ascii_whitespace());
    EXPECT(!u"😀"sv.is_ascii_whitespace());
    EXPECT(!u"\u00a0"sv.is_ascii_whitespace());
    EXPECT(!u"\ufeff"sv.is_ascii_whitespace());
    EXPECT(!u"  \t \u00a0 \ufeff  "sv.is_ascii_whitespace());
}

TEST_CASE(to_ascii_lowercase)
{
    EXPECT_EQ(u""sv.to_ascii_lowercase(), u""sv);
    EXPECT_EQ(u"foobar"sv.to_ascii_lowercase(), u"foobar"sv);
    EXPECT_EQ(u"FooBar"sv.to_ascii_lowercase(), u"foobar"sv);
    EXPECT_EQ(u"FOOBAR"sv.to_ascii_lowercase(), u"foobar"sv);
    EXPECT_EQ(u"FOO 😀 BAR"sv.to_ascii_lowercase(), u"foo 😀 bar"sv);
}

TEST_CASE(to_ascii_uppercase)
{
    EXPECT_EQ(u""sv.to_ascii_uppercase(), u""sv);
    EXPECT_EQ(u"foobar"sv.to_ascii_uppercase(), u"FOOBAR"sv);
    EXPECT_EQ(u"FooBar"sv.to_ascii_uppercase(), u"FOOBAR"sv);
    EXPECT_EQ(u"FOOBAR"sv.to_ascii_uppercase(), u"FOOBAR"sv);
    EXPECT_EQ(u"foo 😀 bar"sv.to_ascii_uppercase(), u"FOO 😀 BAR"sv);
}

TEST_CASE(to_ascii_titlecase)
{
    EXPECT_EQ(u""sv.to_ascii_titlecase(), u""sv);
    EXPECT_EQ(u"foobar"sv.to_ascii_titlecase(), u"Foobar"sv);
    EXPECT_EQ(u"FooBar"sv.to_ascii_titlecase(), u"Foobar"sv);
    EXPECT_EQ(u"foo bar"sv.to_ascii_titlecase(), u"Foo Bar"sv);
    EXPECT_EQ(u"FOO BAR"sv.to_ascii_titlecase(), u"Foo Bar"sv);
    EXPECT_EQ(u"foo 😀 bar"sv.to_ascii_titlecase(), u"Foo 😀 Bar"sv);
}

TEST_CASE(equals_ignoring_case)
{
    auto string1 = Utf16String::from_utf8("foobar"sv);
    auto string2 = Utf16String::from_utf8("FooBar"sv);
    EXPECT(Utf16View { string1 }.equals_ignoring_case(Utf16View { string2 }));

    string1 = Utf16String::from_utf8(""sv);
    string2 = Utf16String::from_utf8(""sv);
    EXPECT(Utf16View { string1 }.equals_ignoring_case(Utf16View { string2 }));

    string1 = Utf16String::from_utf8(""sv);
    string2 = Utf16String::from_utf8("FooBar"sv);
    EXPECT(!Utf16View { string1 }.equals_ignoring_case(Utf16View { string2 }));
}

TEST_CASE(code_unit_offset_of)
{
    Utf16View view { u"😂 foo 😀 bar"sv };

    EXPECT_EQ(view.code_unit_offset_of(0), 0uz);
    EXPECT_EQ(view.code_unit_offset_of(1), 2uz);
    EXPECT_EQ(view.code_unit_offset_of(2), 3uz);
    EXPECT_EQ(view.code_unit_offset_of(3), 4uz);
    EXPECT_EQ(view.code_unit_offset_of(4), 5uz);
    EXPECT_EQ(view.code_unit_offset_of(5), 6uz);
    EXPECT_EQ(view.code_unit_offset_of(6), 7uz);
    EXPECT_EQ(view.code_unit_offset_of(7), 9uz);
    EXPECT_EQ(view.code_unit_offset_of(8), 10uz);
    EXPECT_EQ(view.code_unit_offset_of(9), 11uz);
    EXPECT_EQ(view.code_unit_offset_of(10), 12uz);
    EXPECT_EQ(view.code_unit_offset_of(11), 13uz);
}

TEST_CASE(code_point_offset_of)
{
    Utf16View view { u"😂 foo 😀 bar"sv };

    EXPECT_EQ(view.code_point_offset_of(0), 0uz);
    EXPECT_EQ(view.code_point_offset_of(1), 0uz);
    EXPECT_EQ(view.code_point_offset_of(2), 1uz);
    EXPECT_EQ(view.code_point_offset_of(3), 2uz);
    EXPECT_EQ(view.code_point_offset_of(4), 3uz);
    EXPECT_EQ(view.code_point_offset_of(5), 4uz);
    EXPECT_EQ(view.code_point_offset_of(6), 5uz);
    EXPECT_EQ(view.code_point_offset_of(7), 6uz);
    EXPECT_EQ(view.code_point_offset_of(8), 6uz);
    EXPECT_EQ(view.code_point_offset_of(9), 7uz);
    EXPECT_EQ(view.code_point_offset_of(10), 8uz);
    EXPECT_EQ(view.code_point_offset_of(11), 9uz);
    EXPECT_EQ(view.code_point_offset_of(12), 10uz);
    EXPECT_EQ(view.code_point_offset_of(13), 11uz);
}

TEST_CASE(replace)
{
    auto result = u""sv.replace({}, {}, ReplaceMode::FirstOnly);
    EXPECT_EQ(result, u""sv);

    result = u""sv.replace(u"foo"sv, u"bar"sv, ReplaceMode::FirstOnly);
    EXPECT_EQ(result, u""sv);

    result = u"foo"sv.replace(u"bar"sv, u"baz"sv, ReplaceMode::FirstOnly);
    EXPECT_EQ(result, u"foo"sv);

    result = u"foo"sv.replace(u"foo"sv, u"bar"sv, ReplaceMode::FirstOnly);
    EXPECT_EQ(result, u"bar"sv);

    result = u"foo"sv.replace(u"o"sv, u"e"sv, ReplaceMode::FirstOnly);
    EXPECT_EQ(result, u"feo"sv);

    result = u"foo"sv.replace(u"o"sv, u"e"sv, ReplaceMode::All);
    EXPECT_EQ(result, u"fee"sv);

    result = u"foo boo"sv.replace(u"o"sv, u"e"sv, ReplaceMode::FirstOnly);
    EXPECT_EQ(result, u"feo boo"sv);

    result = u"foo boo"sv.replace(u"o"sv, u"e"sv, ReplaceMode::All);
    EXPECT_EQ(result, u"fee bee"sv);

    result = u"foo 😀 boo 😀"sv.replace(u"o"sv, u"e"sv, ReplaceMode::All);
    EXPECT_EQ(result, u"fee 😀 bee 😀"sv);

    result = u"foo 😀 boo 😀"sv.replace(u"😀"sv, u"🙃"sv, ReplaceMode::FirstOnly);
    EXPECT_EQ(result, u"foo 🙃 boo 😀"sv);

    result = u"foo 😀 boo 😀"sv.replace(u"😀"sv, u"🙃"sv, ReplaceMode::All);
    EXPECT_EQ(result, u"foo 🙃 boo 🙃"sv);

    result = u"foo 😀 boo 😀"sv.replace(u"😀 "sv, u"🙃 "sv, ReplaceMode::All);
    EXPECT_EQ(result, u"foo 🙃 boo 😀"sv);
}

TEST_CASE(substring_view)
{
    auto string = Utf16String::from_utf8("Привет 😀"sv);
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

TEST_CASE(trim)
{
    Utf16View whitespace { u" "sv };
    {
        Utf16View view { u"word"sv };
        EXPECT_EQ(view.trim(whitespace, TrimMode::Both), u"word"sv);
        EXPECT_EQ(view.trim(whitespace, TrimMode::Left), u"word"sv);
        EXPECT_EQ(view.trim(whitespace, TrimMode::Right), u"word"sv);
    }
    {
        Utf16View view { u"   word"sv };
        EXPECT_EQ(view.trim(whitespace, TrimMode::Both), u"word"sv);
        EXPECT_EQ(view.trim(whitespace, TrimMode::Left), u"word"sv);
        EXPECT_EQ(view.trim(whitespace, TrimMode::Right), u"   word"sv);
    }
    {
        Utf16View view { u"word   "sv };
        EXPECT_EQ(view.trim(whitespace, TrimMode::Both), u"word"sv);
        EXPECT_EQ(view.trim(whitespace, TrimMode::Left), u"word   "sv);
        EXPECT_EQ(view.trim(whitespace, TrimMode::Right), u"word"sv);
    }
    {
        Utf16View view { u"   word   "sv };
        EXPECT_EQ(view.trim(whitespace, TrimMode::Both), u"word"sv);
        EXPECT_EQ(view.trim(whitespace, TrimMode::Left), u"word   "sv);
        EXPECT_EQ(view.trim(whitespace, TrimMode::Right), u"   word"sv);
    }
    {
        Utf16View view { u"   \u180E   "sv };
        EXPECT_EQ(view.trim(whitespace, TrimMode::Both), u"\u180E"sv);
        EXPECT_EQ(view.trim(whitespace, TrimMode::Left), u"\u180E   "sv);
        EXPECT_EQ(view.trim(whitespace, TrimMode::Right), u"   \u180E"sv);
    }
    {
        Utf16View view { u"😀wfh😀"sv };
        EXPECT_EQ(view.trim(u"😀"sv, TrimMode::Both), u"wfh"sv);
        EXPECT_EQ(view.trim(u"😀"sv, TrimMode::Left), u"wfh😀"sv);
        EXPECT_EQ(view.trim(u"😀"sv, TrimMode::Right), u"😀wfh"sv);
    }
}

TEST_CASE(contains)
{
    EXPECT(!u""sv.contains(u'a'));
    EXPECT(u"a"sv.contains(u'a'));
    EXPECT(!u"b"sv.contains(u'a'));
    EXPECT(u"ab"sv.contains(u'a'));
    EXPECT(u"😀"sv.contains(u'\xd83d'));
    EXPECT(u"😀"sv.contains(u'\xde00'));

    EXPECT(u""sv.contains(u""sv));
    EXPECT(!u""sv.contains(u"a"sv));
    EXPECT(u"a"sv.contains(u"a"sv));
    EXPECT(!u"b"sv.contains(u"a"sv));
    EXPECT(u"ab"sv.contains(u"a"sv));
    EXPECT(u"😀"sv.contains(u"\xd83d"sv));
    EXPECT(u"😀"sv.contains(u"\xde00"sv));
    EXPECT(u"😀"sv.contains(u"😀"sv));
    EXPECT(u"ab😀"sv.contains(u"😀"sv));
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

TEST_CASE(ends_with)
{
    EXPECT(Utf16View {}.ends_with(u""sv));
    EXPECT(!Utf16View {}.ends_with(u" "sv));

    EXPECT(u"a"sv.ends_with(u""sv));
    EXPECT(u"a"sv.ends_with(u"a"sv));
    EXPECT(!u"a"sv.ends_with(u"b"sv));
    EXPECT(!u"a"sv.ends_with(u"ab"sv));

    EXPECT(u"abc"sv.ends_with(u""sv));
    EXPECT(u"abc"sv.ends_with(u"c"sv));
    EXPECT(u"abc"sv.ends_with(u"bc"sv));
    EXPECT(u"abc"sv.ends_with(u"abc"sv));
    EXPECT(!u"abc"sv.ends_with(u"b"sv));
    EXPECT(!u"abc"sv.ends_with(u"ab"sv));

    auto emoji = u"😀🙃"sv;

    EXPECT(emoji.ends_with(u""sv));
    EXPECT(emoji.ends_with(u"🙃"sv));
    EXPECT(emoji.ends_with(u"😀🙃"sv));
    EXPECT(!emoji.ends_with(u"a"sv));
    EXPECT(!emoji.ends_with(u"😀"sv));
}

TEST_CASE(find_code_unit_offset)
{
    auto conversion_result = Utf16String::from_utf8("😀foo😀bar"sv);
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
    auto conversion_result = Utf16String::from_utf8("😀Foo😀Bar"sv);
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
