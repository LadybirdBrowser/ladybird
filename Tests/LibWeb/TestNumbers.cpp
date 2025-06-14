/*
 * Copyright (c) 2023, Jonatan Klemets <jonatan.r.klemets@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibWeb/HTML/Numbers.h>

TEST_CASE(parse_integer)
{
    auto optional_value = Web::HTML::parse_integer(""_sv);
    EXPECT(!optional_value.has_value());

    optional_value = Web::HTML::parse_integer("123"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 123);

    optional_value = Web::HTML::parse_integer(" 456"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 456);

    optional_value = Web::HTML::parse_integer("789 "_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 789);

    optional_value = Web::HTML::parse_integer("   22   "_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 22);

    optional_value = Web::HTML::parse_integer(" \n\t31\t\t\n\n"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 31);

    optional_value = Web::HTML::parse_integer("765foo"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 765);

    optional_value = Web::HTML::parse_integer("3;"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 3);

    optional_value = Web::HTML::parse_integer("foo765"_sv);
    EXPECT(!optional_value.has_value());

    optional_value = Web::HTML::parse_integer("1"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 1);

    optional_value = Web::HTML::parse_integer("+2"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 2);

    optional_value = Web::HTML::parse_integer("-3"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), -3);
}

TEST_CASE(parse_non_negative_integer)
{
    auto optional_value = Web::HTML::parse_non_negative_integer(""_sv);
    EXPECT(!optional_value.has_value());

    optional_value = Web::HTML::parse_non_negative_integer("123"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 123u);

    optional_value = Web::HTML::parse_non_negative_integer(" 456"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 456u);

    optional_value = Web::HTML::parse_non_negative_integer("789 "_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 789u);

    optional_value = Web::HTML::parse_non_negative_integer("   22   "_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 22u);

    optional_value = Web::HTML::parse_non_negative_integer(" \n\t31\t\t\n\n"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 31u);

    optional_value = Web::HTML::parse_non_negative_integer("765foo"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 765u);

    optional_value = Web::HTML::parse_non_negative_integer("3;"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 3u);

    optional_value = Web::HTML::parse_non_negative_integer("foo765"_sv);
    EXPECT(!optional_value.has_value());

    optional_value = Web::HTML::parse_non_negative_integer("1"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 1u);

    optional_value = Web::HTML::parse_non_negative_integer("+2"_sv);
    EXPECT(optional_value.has_value());
    EXPECT_EQ(optional_value.value(), 2u);

    optional_value = Web::HTML::parse_non_negative_integer("-3"_sv);
    EXPECT(!optional_value.has_value());

    EXPECT(Web::HTML::is_valid_floating_point_number("11"_sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("11.12"_sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("-11111"_sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("-11111.123"_sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("1e2"_sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("1E2"_sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("1e+2"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("1d+2"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("foobar"_sv));
    EXPECT(Web::HTML::is_valid_floating_point_number(".1"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("1."_sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("-0"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("Infinity"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("-Infinity"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("NaN"_sv));
    EXPECT(Web::HTML::is_valid_floating_point_number("9007199254740993"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("1e"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("+1"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("+"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("-"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("\t1"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("\n1"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("\f1"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("\r1"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number(" 1"_sv));
    EXPECT(!Web::HTML::is_valid_floating_point_number("1trailing junk"_sv));
}
