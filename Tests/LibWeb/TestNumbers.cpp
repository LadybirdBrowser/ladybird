/*
 * Copyright (c) 2023, Jonatan Klemets <jonatan.r.klemets@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Utf16String.h>
#include <LibWeb/HTML/Numbers.h>

TEST_CASE(parse_integer)
{
    auto optional_value = Web::HTML::parse_integer(u""sv);
    EXPECT(!optional_value.has_value());

    optional_value = Web::HTML::parse_integer(u"123"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 123);

    optional_value = Web::HTML::parse_integer(u" 456"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 456);

    optional_value = Web::HTML::parse_integer(u"789 "sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 789);

    optional_value = Web::HTML::parse_integer(u"   22   "sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 22);

    optional_value = Web::HTML::parse_integer(u" \n\t31\t\t\n\n"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 31);

    optional_value = Web::HTML::parse_integer(u"765foo"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 765);

    optional_value = Web::HTML::parse_integer(u"3;"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 3);

    optional_value = Web::HTML::parse_integer(u"foo765"sv);
    EXPECT(!optional_value.has_value());

    optional_value = Web::HTML::parse_integer(u"1"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 1);

    optional_value = Web::HTML::parse_integer(u"+2"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 2);

    optional_value = Web::HTML::parse_integer(u"-3"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), -3);
}

TEST_CASE(parse_non_negative_integer)
{
    auto optional_value = Web::HTML::parse_non_negative_integer(u""sv);
    EXPECT(!optional_value.has_value());

    optional_value = Web::HTML::parse_non_negative_integer(u"123"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 123u);

    optional_value = Web::HTML::parse_non_negative_integer(u" 456"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 456u);

    optional_value = Web::HTML::parse_non_negative_integer(u"789 "sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 789u);

    optional_value = Web::HTML::parse_non_negative_integer(u"   22   "sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 22u);

    optional_value = Web::HTML::parse_non_negative_integer(u" \n\t31\t\t\n\n"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 31u);

    optional_value = Web::HTML::parse_non_negative_integer(u"765foo"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 765u);

    optional_value = Web::HTML::parse_non_negative_integer(u"3;"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 3u);

    optional_value = Web::HTML::parse_non_negative_integer(u"foo765"sv);
    EXPECT(!optional_value.has_value());

    optional_value = Web::HTML::parse_non_negative_integer(u"1"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 1u);

    optional_value = Web::HTML::parse_non_negative_integer(u"+2"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 2u);

    optional_value = Web::HTML::parse_non_negative_integer(u"-3"sv);
    EXPECT(!optional_value.has_value());

    EXPECT(Web::HTML::is_valid_floating_point_number(u"11"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number(u"11.12"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number(u"-11111"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number(u"-11111.123"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number(u"1e2"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number(u"1E2"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number(u"1e+2"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"1d+2"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"foobar"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number(u".1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"1."sv));
    EXPECT(Web::HTML::is_valid_floating_point_number(u"-0"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"Infinity"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"-Infinity"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"NaN"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number(u"9007199254740993"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"1e"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"+1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"+"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"-"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"\t1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"\n1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"\f1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"\r1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u" 1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(u"1trailing junk"sv));
}

TEST_CASE(parse_numbers_from_utf16)
{
    auto integer_string = Utf16String::from_utf8(" 456foo"sv);
    auto integer_value = Web::HTML::parse_integer(integer_string);
    EXPECT(integer_value.has_value());
    EXPECT_EQ(integer_value.value(), 456);

    integer_string = Utf16String::from_utf8("foo456"sv);
    integer_value = Web::HTML::parse_integer(integer_string);
    EXPECT(!integer_value.has_value());

    integer_string = Utf16String::from_utf8("１２3"sv);
    integer_value = Web::HTML::parse_integer(integer_string);
    EXPECT(!integer_value.has_value());

    auto non_negative_integer_string = Utf16String::from_utf8("+123foo"sv);
    auto non_negative_integer_value = Web::HTML::parse_non_negative_integer(non_negative_integer_string);
    EXPECT(non_negative_integer_value.has_value());
    EXPECT_EQ(non_negative_integer_value.value(), 123u);

    non_negative_integer_string = Utf16String::from_utf8("-123"sv);
    non_negative_integer_value = Web::HTML::parse_non_negative_integer(non_negative_integer_string);
    EXPECT(!non_negative_integer_value.has_value());

    auto floating_point_string = Utf16String::from_utf8(" 1.25e2suffix"sv);
    auto floating_point_value = Web::HTML::parse_floating_point_number(floating_point_string);
    EXPECT(floating_point_value.has_value());
    EXPECT_EQ(floating_point_value.value(), 125);

    floating_point_string = Utf16String::from_utf8("1.25e2"sv);
    EXPECT(Web::HTML::is_valid_floating_point_number(floating_point_string));

    floating_point_string = Utf16String::from_utf8("1.25e"sv);
    EXPECT(!Web::HTML::is_valid_floating_point_number(floating_point_string));

    floating_point_string = Utf16String::from_utf8("１.25"sv);
    EXPECT(!Web::HTML::is_valid_floating_point_number(floating_point_string));
}
