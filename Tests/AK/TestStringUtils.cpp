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

    EXPECT(!AK::StringUtils::matches(StringView(), ""sv));
    EXPECT(!AK::StringUtils::matches(StringView(), "*"sv));
    EXPECT(!AK::StringUtils::matches(StringView(), "?"sv));
    EXPECT(!AK::StringUtils::matches(StringView(), "a"sv));

    EXPECT(!AK::StringUtils::matches(""sv, StringView()));
    EXPECT(!AK::StringUtils::matches("a"sv, StringView()));
}

TEST_CASE(matches_empty)
{
    EXPECT(AK::StringUtils::matches(""sv, ""sv));

    EXPECT(AK::StringUtils::matches(""sv, "*"sv));
    EXPECT(!AK::StringUtils::matches(""sv, "?"sv));
    EXPECT(!AK::StringUtils::matches(""sv, "a"sv));

    EXPECT(!AK::StringUtils::matches("a"sv, ""sv));
}

TEST_CASE(matches_case_sensitive)
{
    EXPECT(AK::StringUtils::matches("a"sv, "a"sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::matches("a"sv, "A"sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::matches("A"sv, "a"sv, CaseSensitivity::CaseSensitive));
}

TEST_CASE(matches_case_insensitive)
{
    EXPECT(!AK::StringUtils::matches("aa"sv, "a"sv));
    EXPECT(AK::StringUtils::matches("aa"sv, "*"sv));
    EXPECT(!AK::StringUtils::matches("cb"sv, "?a"sv));
    EXPECT(AK::StringUtils::matches("adceb"sv, "a*b"sv));
    EXPECT(!AK::StringUtils::matches("acdcb"sv, "a*c?b"sv));
}

TEST_CASE(matches_with_positions)
{
    Vector<AK::MaskSpan> spans;
    EXPECT(AK::StringUtils::matches("abbb"sv, "a*"sv, CaseSensitivity::CaseSensitive, &spans));
    EXPECT(spans == Vector<AK::MaskSpan>({ { 1, 3 } }));

    spans.clear();
    EXPECT(AK::StringUtils::matches("abbb"sv, "?*"sv, CaseSensitivity::CaseSensitive, &spans));
    EXPECT_EQ(spans, Vector<AK::MaskSpan>({ { 0, 1 }, { 1, 3 } }));

    spans.clear();
    EXPECT(AK::StringUtils::matches("acdcxb"sv, "a*c?b"sv, CaseSensitivity::CaseSensitive, &spans));
    EXPECT_EQ(spans, Vector<AK::MaskSpan>({ { 1, 2 }, { 4, 1 } }));

    spans.clear();
    EXPECT(AK::StringUtils::matches("aaaa"sv, "A*"sv, CaseSensitivity::CaseInsensitive, &spans));
    EXPECT_EQ(spans, Vector<AK::MaskSpan>({ { 1, 3 } }));
}

// #4607
TEST_CASE(matches_trailing)
{
    EXPECT(AK::StringUtils::matches("ab"sv, "ab*"sv));
    EXPECT(AK::StringUtils::matches("ab"sv, "ab****"sv));
    EXPECT(AK::StringUtils::matches("ab"sv, "*ab****"sv));
}

TEST_CASE(match_backslash_escape)
{
    EXPECT(AK::StringUtils::matches("ab*"sv, "ab\\*"sv));
    EXPECT(!AK::StringUtils::matches("abc"sv, "ab\\*"sv));
    EXPECT(!AK::StringUtils::matches("abcd"sv, "ab\\*"sv));
    EXPECT(AK::StringUtils::matches("ab?"sv, "ab\\?"sv));
    EXPECT(!AK::StringUtils::matches("abc"sv, "ab\\?"sv));
}

TEST_CASE(match_trailing_backslash)
{
    EXPECT(AK::StringUtils::matches("x\\"sv, "x\\"sv));
    EXPECT(AK::StringUtils::matches("x\\"sv, "x\\\\"sv));
}

TEST_CASE(ends_with)
{
    ByteString test_string = "ABCDEF";
    EXPECT(AK::StringUtils::ends_with(test_string, "DEF"sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::ends_with(test_string, "ABCDEF"sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::ends_with(test_string, "ABCDE"sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::ends_with(test_string, "ABCDEFG"sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::ends_with(test_string, "def"sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!AK::StringUtils::ends_with(test_string, "def"sv, CaseSensitivity::CaseSensitive));
}

TEST_CASE(starts_with)
{
    ByteString test_string = "ABCDEF";
    EXPECT(AK::StringUtils::starts_with(test_string, "ABC"sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::starts_with(test_string, "ABCDEF"sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::starts_with(test_string, "BCDEF"sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::starts_with(test_string, "ABCDEFG"sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::starts_with(test_string, "abc"sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!AK::StringUtils::starts_with(test_string, "abc"sv, CaseSensitivity::CaseSensitive));
}

TEST_CASE(contains)
{
    ByteString test_string = "ABCDEFABCXYZ";
    EXPECT(AK::StringUtils::contains(test_string, "ABC"sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::contains(test_string, "ABC"sv, CaseSensitivity::CaseInsensitive));
    EXPECT(AK::StringUtils::contains(test_string, "AbC"sv, CaseSensitivity::CaseInsensitive));
    EXPECT(AK::StringUtils::contains(test_string, "BCX"sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::contains(test_string, "BCX"sv, CaseSensitivity::CaseInsensitive));
    EXPECT(AK::StringUtils::contains(test_string, "BcX"sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!AK::StringUtils::contains(test_string, "xyz"sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::contains(test_string, "xyz"sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!AK::StringUtils::contains(test_string, "EFG"sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::contains(test_string, "EfG"sv, CaseSensitivity::CaseInsensitive));
    EXPECT(AK::StringUtils::contains(test_string, ""sv, CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::contains(test_string, ""sv, CaseSensitivity::CaseInsensitive));
    EXPECT(!AK::StringUtils::contains(""sv, test_string, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::contains(""sv, test_string, CaseSensitivity::CaseInsensitive));
    EXPECT(!AK::StringUtils::contains(test_string, "L"sv, CaseSensitivity::CaseSensitive));
    EXPECT(!AK::StringUtils::contains(test_string, "L"sv, CaseSensitivity::CaseInsensitive));

    ByteString command_palette_bug_string = "Go Go Back";
    EXPECT(AK::StringUtils::contains(command_palette_bug_string, "Go Back"sv, AK::CaseSensitivity::CaseSensitive));
    EXPECT(AK::StringUtils::contains(command_palette_bug_string, "gO bAcK"sv, AK::CaseSensitivity::CaseInsensitive));
}

TEST_CASE(is_whitespace)
{
    EXPECT(AK::StringUtils::is_whitespace(""sv));
    EXPECT(AK::StringUtils::is_whitespace("   "sv));
    EXPECT(AK::StringUtils::is_whitespace("  \t"sv));
    EXPECT(AK::StringUtils::is_whitespace("  \t\n"sv));
    EXPECT(AK::StringUtils::is_whitespace("  \t\n\r\v"sv));
    EXPECT(!AK::StringUtils::is_whitespace("  a "sv));
    EXPECT(!AK::StringUtils::is_whitespace("a\t"sv));
}

TEST_CASE(trim)
{
    EXPECT_EQ(AK::StringUtils::trim("aaa.a."sv, "."sv, TrimMode::Right), "aaa.a"sv);
    EXPECT_EQ(AK::StringUtils::trim("...aaa"sv, "."sv, TrimMode::Left), "aaa"sv);
    EXPECT_EQ(AK::StringUtils::trim("...aaa.a..."sv, "."sv, TrimMode::Both), "aaa.a"sv);
    EXPECT_EQ(AK::StringUtils::trim("."sv, "."sv, TrimMode::Right), ""sv);
    EXPECT_EQ(AK::StringUtils::trim("."sv, "."sv, TrimMode::Left), ""sv);
    EXPECT_EQ(AK::StringUtils::trim("."sv, "."sv, TrimMode::Both), ""sv);
    EXPECT_EQ(AK::StringUtils::trim("..."sv, "."sv, TrimMode::Both), ""sv);
}

TEST_CASE(find)
{
    ByteString test_string = "1234567";
    EXPECT_EQ(AK::StringUtils::find(test_string, "1"sv).value_or(1), 0u);
    EXPECT_EQ(AK::StringUtils::find(test_string, "2"sv).value_or(2), 1u);
    EXPECT_EQ(AK::StringUtils::find(test_string, "3"sv).value_or(3), 2u);
    EXPECT_EQ(AK::StringUtils::find(test_string, "4"sv).value_or(4), 3u);
    EXPECT_EQ(AK::StringUtils::find(test_string, "5"sv).value_or(5), 4u);
    EXPECT_EQ(AK::StringUtils::find(test_string, "34"sv).value_or(3), 2u);
    EXPECT_EQ(AK::StringUtils::find(test_string, "78"sv).has_value(), false);
}

TEST_CASE(find_last)
{
    auto test_string = "abcdabc"sv;

    EXPECT_EQ(AK::StringUtils::find_last(test_string, ""sv), 7u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "a"sv), 4u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "b"sv), 5u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "c"sv), 6u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "ab"sv), 4u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "bc"sv), 5u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "abc"sv), 4u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, "abcd"sv), 0u);
    EXPECT_EQ(AK::StringUtils::find_last(test_string, test_string), 0u);

    EXPECT(!AK::StringUtils::find_last(test_string, "1"sv).has_value());
    EXPECT(!AK::StringUtils::find_last(test_string, "e"sv).has_value());
    EXPECT(!AK::StringUtils::find_last(test_string, "abd"sv).has_value());
}

TEST_CASE(replace_all_overlapping)
{
    // Replace only should take into account non-overlapping instances of the
    // needle, since it is looking to replace them.

    // These samples were grabbed from ADKaster's sample code in
    // https://github.com/SerenityOS/jakt/issues/1159. This is the equivalent
    // C++ code that triggered the same bug from Jakt's code generator.

    auto const replace_like_in_jakt = [](StringView source) -> ByteString {
        ByteString replaced = AK::StringUtils::replace(source, "\\\""sv, "\""sv, ReplaceMode::All);
        replaced = AK::StringUtils::replace(replaced.view(), "\\\\"sv, "\\"sv, ReplaceMode::All);
        return replaced;
    };

    EXPECT_EQ(replace_like_in_jakt("\\\\\\\\\\\\\\\\"sv), "\\\\\\\\"sv);
    EXPECT_EQ(replace_like_in_jakt(" auto str4 = \"\\\";"sv), " auto str4 = \"\";"sv);
    EXPECT_EQ(replace_like_in_jakt(" auto str5 = \"\\\\\";"sv), " auto str5 = \"\\\";"sv);
}

TEST_CASE(to_snakecase)
{
    EXPECT_EQ(AK::StringUtils::to_snakecase("foobar"sv), "foobar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("Foobar"sv), "foobar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("FOOBAR"sv), "foobar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("fooBar"sv), "foo_bar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("FooBar"sv), "foo_bar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("fooBAR"sv), "foo_bar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("FOOBar"sv), "foo_bar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("foo_bar"sv), "foo_bar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("FBar"sv), "f_bar");
    EXPECT_EQ(AK::StringUtils::to_snakecase("FooB"sv), "foo_b");
}

TEST_CASE(to_titlecase)
{
    EXPECT_EQ(AK::StringUtils::to_titlecase(""sv), ""sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("f"sv), "F"sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("foobar"sv), "Foobar"sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("Foobar"sv), "Foobar"sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("FOOBAR"sv), "Foobar"sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("foo bar"sv), "Foo Bar"sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("foo bAR"sv), "Foo Bar"sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("foo  bar"sv), "Foo  Bar"sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("foo   bar"sv), "Foo   Bar"sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("   foo   bar   "sv), "   Foo   Bar   "sv);
    EXPECT_EQ(AK::StringUtils::to_titlecase("\xc3\xa7"sv), "\xc3\xa7"sv);         // U+00E7 LATIN SMALL LETTER C WITH CEDILLA
    EXPECT_EQ(AK::StringUtils::to_titlecase("\xe1\x80\x80"sv), "\xe1\x80\x80"sv); // U+1000 MYANMAR LETTER KA
}
