/*
 * Copyright (c) 2023, Jonatan Klemets <jonatan.r.klemets@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibWeb/HTML/Numbers.h>

TEST_CASE(parse_integer)
{
    auto optional_value = Web::HTML::parse_integer(""sv);
    EXPECT(!optional_value.has_value());

    optional_value = Web::HTML::parse_integer("123"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 123);

    optional_value = Web::HTML::parse_integer(" 456"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 456);

    optional_value = Web::HTML::parse_integer("789 "sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 789);

    optional_value = Web::HTML::parse_integer("   22   "sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 22);

    optional_value = Web::HTML::parse_integer(" \n\t31\t\t\n\n"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 31);

    optional_value = Web::HTML::parse_integer("765foo"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 765);

    optional_value = Web::HTML::parse_integer("3;"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 3);

    optional_value = Web::HTML::parse_integer("foo765"sv);
    EXPECT(!optional_value.has_value());

    optional_value = Web::HTML::parse_integer("1"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 1);

    optional_value = Web::HTML::parse_integer("+2"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 2);

    optional_value = Web::HTML::parse_integer("-3"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), -3);
}

TEST_CASE(parse_non_negative_integer)
{
    auto optional_value = Web::HTML::parse_non_negative_integer(""sv);
    EXPECT(!optional_value.has_value());

    optional_value = Web::HTML::parse_non_negative_integer("123"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 123u);

    optional_value = Web::HTML::parse_non_negative_integer(" 456"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 456u);

    optional_value = Web::HTML::parse_non_negative_integer("789 "sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 789u);

    optional_value = Web::HTML::parse_non_negative_integer("   22   "sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 22u);

    optional_value = Web::HTML::parse_non_negative_integer(" \n\t31\t\t\n\n"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 31u);

    optional_value = Web::HTML::parse_non_negative_integer("765foo"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 765u);

    optional_value = Web::HTML::parse_non_negative_integer("3;"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 3u);

    optional_value = Web::HTML::parse_non_negative_integer("foo765"sv);
    EXPECT(!optional_value.has_value());

    optional_value = Web::HTML::parse_non_negative_integer("1"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 1u);

    optional_value = Web::HTML::parse_non_negative_integer("+2"sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 2u);

    optional_value = Web::HTML::parse_non_negative_integer("-3"sv);
    EXPECT(!optional_value.has_value());

    EXPECT(Web::HTML::is_valid_floating_point_number("11"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("11.12"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("-11111"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("-11111.123"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("1e2"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("1E2"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("1e+2"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("1d+2"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("foobar"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number(".1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("1."sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("-0"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("Infinity"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("-Infinity"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("NaN"sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("9007199254740993"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("1e"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("+1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("+"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("-"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("\t1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("\n1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("\f1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("\r1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(" 1"sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("1trailing junk"sv));
}
