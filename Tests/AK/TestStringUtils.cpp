/*
 * Copyright (c) 2020, Fei Wu <f.eiwu@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteBuffer.h>
#include <AK/Concepts.h>
#include <AK/FlyString.h>
#include <AK/String.h>
#include <AK/StringUtils.h>
#include <AK/Vector.h>

TEST_CASE(hash_compatible)
{
    static_assert(AK::Concepts::HashCompatible<String, StringView>);
    static_assert(AK::Concepts::HashCompatible<String, FlyString>);
    static_assert(AK::Concepts::HashCompatible<StringView, String>);
    static_assert(AK::Concepts::HashCompatible<StringView, FlyString>);
    static_assert(AK::Concepts::HashCompatible<FlyString, String>);
    static_assert(AK::Concepts::HashCompatible<FlyString, StringView>);

    static_assert(AK::Concepts::HashCompatible<ByteString, StringView>);
    static_assert(AK::Concepts::HashCompatible<StringView, ByteString>);

    static_assert(AK::Concepts::HashCompatible<StringView, ByteBuffer>);
    static_assert(AK::Concepts::HashCompatible<ByteBuffer, StringView>);
}

TEST_CASE(matches_null)
{
    EXPECT(AK::StringUtils::matches(StringView(), StringView()));

    EXPECT(!AK::StringUtils::matches(StringView(), ""_sv));
    EXPECT(!AK::StringUtils::matches(StringView(), "*"_sv));
    EXPECT(!AK::StringUtils::matches(StringView(), "?"_sv));
    EXPECT(!AK::StringUtils::matches(StringView(), "a"_sv));

    EXPECT(!AK::StringUtils::matches(""_sv, StringView()));
    EXPECT(!AK::StringUtils::matches("a"_sv, StringView()));
}

TEST_CASE(matches_empty)
{
    EXPECT(AK::StringUtils::matches(""_sv, ""_sv));

    EXPECT(AK::StringUtils::matches(""_sv, "*"_sv));
    EXPECT(!AK::StringUtils::matches(""_sv, "?"_sv));
    EXPECT(!AK::StringUtils::matches(""_sv, "a"_sv));

    EXPECT(!AK::StringUtils::matches("a"_sv, ""_sv));
}

TEST_CASE(matches_case_sensitive)
{
    EXPECT(AK::StringUtils::matches("a"_sv, "a"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::matches("a"_sv, "A"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::matches("A"_sv, "a"_sv, CaseSensitivity::CaseSensitive));
}

TEST_CASE(matches_case_insensitive)
{
    EXPECT(!AK::StringUtils::matches("aa"_sv, "a"_sv));
    EXPECT(AK::StringUtils::matches("aa"_sv, "*"_sv));
    EXPECT(!AK::StringUtils::matches("cb"_sv, "?a"_sv));
    EXPECT(AK::StringUtils::matches("adceb"_sv, "a*b"_sv));
    EXPECT(!AK::StringUtils::matches("acdcb"_sv, "a*c?b"_sv));
}

TEST_CASE(matches_with_positions)
{
    Vector<AK::MaskSpan> spans;
    EXPECT(AK::StringUtils::matches("abbb"_sv, "a*"_sv, CaseSensitivity::CaseSensitive, &spans));
    EXPECT(spans == Vector<AK::MaskSpan>({ { 1, 3 } }));

    spans.clear();
    EXPECT(AK::StringUtils::matches("abbb"_sv, "?*"_sv, CaseSensitivity::CaseSensitive, &spans));
    EXPECT_EQ(spans, Vector<AK::MaskSpan>({ { 0, 1 }, { 1, 3 } }));

    spans.clear();
    EXPECT(AK::StringUtils::matches("acdcxb"_sv, "a*c?b"_sv, CaseSensitivity::CaseSensitive, &spans));
    EXPECT_EQ(spans, Vector<AK::MaskSpan>({ { 1, 2 }, { 4, 1 } }));

    spans.clear();
    EXPECT(AK::StringUtils::matches("aaaa"_sv, "A*"_sv, CaseSensitivity::CaseInsensitive, &spans));
    EXPECT_EQ(spans, Vector<AK::MaskSpan>({ { 1, 3 } }));
}

// #4607
TEST_CASE(matches_trailing)
{
    EXPECT(AK::StringUtils::matches("ab"_sv, "ab*"_sv));
    EXPECT(AK::StringUtils::matches("ab"_sv, "ab****"_sv));
    EXPECT(AK::StringUtils::matches("ab"_sv, "*ab****"_sv));
}

TEST_CASE(match_backslash_escape)
{
    EXPECT(AK::StringUtils::matches("ab*"_sv, "ab\\*"_sv));
    EXPECT(!AK::StringUtils::matches("abc"_sv, "ab\\*"_sv));
    EXPECT(!AK::StringUtils::matches("abcd"_sv, "ab\\*"_sv));
    EXPECT(AK::StringUtils::matches("ab?"_sv, "ab\\?"_sv));
    EXPECT(!AK::StringUtils::matches("abc"_sv, "ab\\?"_sv));
}

TEST_CASE(match_trailing_backslash)
{
    EXPECT(AK::StringUtils::matches("x\\"_sv, "x\\"_sv));
    EXPECT(AK::StringUtils::matches("x\\"_sv, "x\\\\"_sv));
}

TEST_CASE(convert_to_int)
{
    auto value = AK::StringUtils::convert_to_int(StringView());
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_int(""_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_int("a"_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_int("+"_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_int("-"_sv);
    EXPECT(!value.has_value());

    auto actual = AK::StringUtils::convert_to_int("0"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 0);

    actual = AK::StringUtils::convert_to_int("1"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 1);

    actual = AK::StringUtils::convert_to_int("+1"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 1);

    actual = AK::StringUtils::convert_to_int("-1"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), -1);

    actual = AK::StringUtils::convert_to_int("01"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 1);

    actual = AK::StringUtils::convert_to_int("12345"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 12345);

    actual = AK::StringUtils::convert_to_int("-12345"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), -12345);

    actual = AK::StringUtils::convert_to_int(" \t-12345 \n\n"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), -12345);

    auto actual_i8 = AK::StringUtils::convert_to_int<i8>("-1"_sv);
    EXPECT(actual_i8.has_value());
    EXPECT_EQ(actual_i8.value(), -1);
    EXPECT_EQ(sizeof(actual_i8.value()), (size_t)1);
    actual_i8 = AK::StringUtils::convert_to_int<i8>("128"_sv);
    EXPECT(!actual_i8.has_value());

    auto actual_i16 = AK::StringUtils::convert_to_int<i16>("-1"_sv);
    EXPECT(actual_i16.has_value());
    EXPECT_EQ(actual_i16.value(), -1);
    EXPECT_EQ(sizeof(actual_i16.value()), (size_t)2);
    actual_i16 = AK::StringUtils::convert_to_int<i16>("32768"_sv);
    EXPECT(!actual_i16.has_value());

    auto actual_i32 = AK::StringUtils::convert_to_int<i32>("-1"_sv);
    EXPECT(actual_i32.has_value());
    EXPECT_EQ(actual_i32.value(), -1);
    EXPECT_EQ(sizeof(actual_i32.value()), (size_t)4);
    actual_i32 = AK::StringUtils::convert_to_int<i32>("2147483648"_sv);
    EXPECT(!actual_i32.has_value());

    auto actual_i64 = AK::StringUtils::convert_to_int<i64>("-1"_sv);
    EXPECT(actual_i64.has_value());
    EXPECT_EQ(actual_i64.value(), -1);
    EXPECT_EQ(sizeof(actual_i64.value()), (size_t)8);
    actual_i64 = AK::StringUtils::convert_to_int<i64>("9223372036854775808"_sv);
    EXPECT(!actual_i64.has_value());
}

TEST_CASE(convert_to_uint)
{
    auto value = AK::StringUtils::convert_to_uint(StringView());
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint(""_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint("a"_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint("+"_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint("-"_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint("+1"_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint("-1"_sv);
    EXPECT(!value.has_value());

    auto actual = AK::StringUtils::convert_to_uint("0"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 0u);

    actual = AK::StringUtils::convert_to_uint("1"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 1u);

    actual = AK::StringUtils::convert_to_uint("01"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 1u);

    actual = AK::StringUtils::convert_to_uint("12345"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 12345u);

    actual = AK::StringUtils::convert_to_uint(" \t12345 \n\n"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 12345u);

    auto actual_u8 = AK::StringUtils::convert_to_uint<u8>("255"_sv);
    EXPECT(actual_u8.has_value());
    EXPECT_EQ(actual_u8.value(), 255u);
    EXPECT_EQ(sizeof(actual_u8.value()), (size_t)1);
    actual_u8 = AK::StringUtils::convert_to_uint<u8>("256"_sv);
    EXPECT(!actual_u8.has_value());

    auto actual_u16 = AK::StringUtils::convert_to_uint<u16>("65535"_sv);
    EXPECT(actual_u16.has_value());
    EXPECT_EQ(actual_u16.value(), 65535u);
    EXPECT_EQ(sizeof(actual_u16.value()), (size_t)2);
    actual_u16 = AK::StringUtils::convert_to_uint<u16>("65536"_sv);
    EXPECT(!actual_u16.has_value());

    auto actual_u32 = AK::StringUtils::convert_to_uint<u32>("4294967295"_sv);
    EXPECT(actual_u32.has_value());
    EXPECT_EQ(actual_u32.value(), 4294967295ul);
    EXPECT_EQ(sizeof(actual_u32.value()), (size_t)4);
    actual_u32 = AK::StringUtils::convert_to_uint<u32>("4294967296"_sv);
    EXPECT(!actual_u32.has_value());

    auto actual_u64 = AK::StringUtils::convert_to_uint<u64>("18446744073709551615"_sv);
    EXPECT(actual_u64.has_value());
    EXPECT_EQ(actual_u64.value(), 18446744073709551615ull);
    EXPECT_EQ(sizeof(actual_u64.value()), (size_t)8);
    actual_u64 = AK::StringUtils::convert_to_uint<u64>("18446744073709551616"_sv);
    EXPECT(!actual_u64.has_value());
}

TEST_CASE(convert_to_uint_from_octal)
{
    auto value = AK::StringUtils::convert_to_uint_from_octal<u16>(StringView());
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint_from_octal<u16>(""_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint_from_octal<u16>("a"_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint_from_octal<u16>("+"_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint_from_octal<u16>("-"_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint_from_octal<u16>("+1"_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint_from_octal<u16>("-1"_sv);
    EXPECT(!value.has_value());

    value = AK::StringUtils::convert_to_uint_from_octal<u16>("8"_sv);
    EXPECT(!value.has_value());

    auto actual = AK::StringUtils::convert_to_uint_from_octal<u16>("77777777"_sv);
    EXPECT(!actual.has_value());

    actual = AK::StringUtils::convert_to_uint_from_octal<u16>("0"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 0u);

    actual = AK::StringUtils::convert_to_uint_from_octal<u16>("1"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 1u);

    actual = AK::StringUtils::convert_to_uint_from_octal<u16>("0755"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 0755u);

    actual = AK::StringUtils::convert_to_uint_from_octal<u16>("755"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 0755u);

    actual = AK::StringUtils::convert_to_uint_from_octal<u16>(" \t644 \n\n"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 0644u);

    actual = AK::StringUtils::convert_to_uint_from_octal<u16>("177777"_sv);
    EXPECT_EQ(actual.has_value(), true);
    EXPECT_EQ(actual.value(), 0177777u);
}

TEST_CASE(convert_to_floating_point)
{
    auto number_string = "  123.45  "_sv;
    auto maybe_number = AK::StringUtils::convert_to_floating_point<float>(number_string, TrimWhitespace::Yes);
    EXPECT_APPROXIMATE(maybe_number.value(), 123.45f);
}

TEST_CASE(ends_with)
{
    ByteString test_string = "ABCDEF";
    EXPECT(AK::StringUtils::ends_with(test_string, "DEF"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::ends_with(test_string, "ABCDEF"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::ends_with(test_string, "ABCDE"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::ends_with(test_string, "ABCDEFG"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::ends_with(test_string, "def"_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!AK::StringUtils::ends_with(test_string, "def"_sv, CaseSensitivity::CaseSensitive));
}

TEST_CASE(starts_with)
{
    ByteString test_string = "ABCDEF";
    EXPECT(AK::StringUtils::starts_with(test_string, "ABC"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::starts_with(test_string, "ABCDEF"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::starts_with(test_string, "BCDEF"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::starts_with(test_string, "ABCDEFG"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::starts_with(test_string, "abc"_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!AK::StringUtils::starts_with(test_string, "abc"_sv, CaseSensitivity::CaseSensitive));
}

TEST_CASE(contains)
{
    ByteString test_string = "ABCDEFABCXYZ";
    EXPECT(AK::StringUtils::contains(test_string, "ABC"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::contains(test_string, "ABC"_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(AK::StringUtils::contains(test_string, "AbC"_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(AK::StringUtils::contains(test_string, "BCX"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::contains(test_string, "BCX"_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(AK::StringUtils::contains(test_string, "BcX"_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!AK::StringUtils::contains(test_string, "xyz"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::contains(test_string, "xyz"_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!AK::StringUtils::contains(test_string, "EFG"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::contains(test_string, "EfG"_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(AK::StringUtils::contains(test_string, ""_sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::contains(test_string, ""_sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!AK::StringUtils::contains(""_sv, test_string, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::contains(""_sv, test_string, CaseSensitivity::CaseInsensitive));
    EXPECT(!AK::StringUtils::contains(test_string, "L"_sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::contains(test_string, "L"_sv, CaseSensitivity::CaseInsensitive));

    ByteString command_palette_bug_string = "Go Go Back";
    EXPECT(AK::StringUtils::contains(command_palette_bug_string, "Go Back"_sv, AK::CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::contains(command_palette_bug_string, "gO bAcK"_sv, AK::CaseSensitivity::CaseInsensitive));
}

TEST_CASE(is_whitespace)
{
    EXPECT(AK::StringUtils::is_whitespace(""_sv));
    EXPECT(AK::StringUtils::is_whitespace("   "_sv));
    EXPECT(AK::StringUtils::is_whitespace("  \t"_sv));
    EXPECT(AK::StringUtils::is_whitespace("  \t\n"_sv));
    EXPECT(AK::StringUtils::is_whitespace("  \t\n\r\v"_sv));
    EXPECT(!AK::StringUtils::is_whitespace("  a "_sv));
    EXPECT(!AK::StringUtils::is_whitespace("a\t"_sv));
}

TEST_CASE(trim)
{
    EXPECT_EQ(AK::StringUtils::trim("aaa.a."_sv, "."_sv, TrimMode::Right), "aaa.a"_sv);
    EXPECT_EQ(AK::StringUtils::trim("...aaa"_sv, "."_sv, TrimMode::Left), "aaa"_sv);
    EXPECT_EQ(AK::StringUtils::trim("...aaa.a..."_sv, "."_sv, TrimMode::Both), "aaa.a"_sv);
    EXPECT_EQ(AK::StringUtils::trim("."_sv, "."_sv, TrimMode::Right), ""_sv);
    EXPECT_EQ(AK::StringUtils::trim("."_sv, "."_sv, TrimMode::Left), ""_sv);
    EXPECT_EQ(AK::StringUtils::trim("."_sv, "."_sv, TrimMode::Both), ""_sv);
    EXPECT_EQ(AK::StringUtils::trim("..."_sv, "."_sv, TrimMode::Both), ""_sv);
}

TEST_CASE(find)
{
    ByteString test_string = "1234567";
    EXPECT_EQ(AK::StringUtils::find(test_string, "1"_sv).value_or(1), 0u);
    EXPECT_EQ(AK::StringUtils::find(test_string, "2"_sv).value_or(2), 1u);
    EXPECT_EQ(AK::StringUtils::find(test_string, "3"_sv).value_or(3), 2u);
    EXPECT_EQ(AK::StringUtils::find(test_string, "4"_sv).value_or(4), 3u);
    EXPECT_EQ(AK::StringUtils::find(test_string, "5"_sv).value_or(5), 4u);
    EXPECT_EQ(AK::StringUtils::find(test_string, "34"_sv).value_or(3), 2u);
    EXPECT_EQ(AK::StringUtils::find(test_string, "78"_sv).has_value(), false);
}

TEST_CASE(find_last)
{
    auto test_string = "abcdabc"_sv;

    EXPECT_EQ(AK::StringUtils::find_last(test_string, ""_sv), 7u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "a"_sv), 4u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "b"_sv), 5u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "c"_sv), 6u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "ab"_sv), 4u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "bc"_sv), 5u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "abc"_sv), 4u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "abcd"_sv), 0u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, test_string), 0u);

    EXPECT(!AK::StringUtils::find_last(test_string, "1"_sv).has_value());
    EXPECT(!AK::StringUtils::find_last(test_string, "e"_sv).has_value());
    EXPECT(!AK::StringUtils::find_last(test_string, "abd"_sv).has_value());
}

TEST_CASE(replace_all_overlapping)
{
    // Replace only should take into account non-overlapping instances of the
    // needle, since it is looking to replace them.

    // These samples were grabbed from ADKaster's sample code in
    // https://github.com/SerenityOS/jakt/issues/1159. This is the equivalent
    // C++ code that triggered the same bug from Jakt's code generator.

    auto const replace_like_in_jakt = [](StringView source) -> ByteString {
        ByteString replaced = AK::StringUtils::replace(source, "\\\""_sv, "\""_sv, ReplaceMode::All);
        replaced = AK::StringUtils::replace(replaced.view(), "\\\\"_sv, "\\"_sv, ReplaceMode::All);
        return replaced;
    };

    EXPECT_EQ(replace_like_in_jakt("\\\\\\\\\\\\\\\\"_sv), "\\\\\\\\"_sv);
    EXPECT_EQ(replace_like_in_jakt(" auto str4 = \"\\\";"_sv), " auto str4 = \"\";"_sv);
    EXPECT_EQ(replace_like_in_jakt(" auto str5 = \"\\\\\";"_sv), " auto str5 = \"\\\";"_sv);
}

TEST_CASE(to_snakecase)
{
    EXPECT_EQ(AK::StringUtils::to_snakecase("foobar"_sv), "foobar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("Foobar"_sv), "foobar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("FOOBAR"_sv), "foobar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("fooBar"_sv), "foo_bar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("FooBar"_sv), "foo_bar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("fooBAR"_sv), "foo_bar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("FOOBar"_sv), "foo_bar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("foo_bar"_sv), "foo_bar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("FBar"_sv), "f_bar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("FooB"_sv), "foo_b");
}

TEST_CASE(to_titlecase)
{
    EXPECT_EQ(AK::StringUtils::to_titlecase(""_sv), ""_sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("f"_sv), "F"_sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("foobar"_sv), "Foobar"_sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("Foobar"_sv), "Foobar"_sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("FOOBAR"_sv), "Foobar"_sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("foo bar"_sv), "Foo Bar"_sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("foo bAR"_sv), "Foo Bar"_sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("foo  bar"_sv), "Foo  Bar"_sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("foo   bar"_sv), "Foo   Bar"_sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("   foo   bar   "_sv), "   Foo   Bar   "_sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("\xc3\xa7"_sv), "\xc3\xa7"_sv);         // U+00E7 LATIN SMALL LETTER C WITH CEDILLA
    EXPECT_EQ(AK::StringUtils::to_titlecase("\xe1\x80\x80"_sv), "\xe1\x80\x80"_sv); // U+1000 MYANMAR LETTER KA
}
