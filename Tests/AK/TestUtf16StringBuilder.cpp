/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <AK/Utf16StringBuilder.h>
#include <LibTest/TestCase.h>

TEST_CASE(build_ascii_string)
{
    Utf16StringBuilder builder;
    builder.append_ascii("hello"sv);
    builder.append_ascii('!');

    EXPECT_EQ(builder.view(), "hello!"sv);

    auto string = builder.to_string();
    EXPECT_EQ(string, "hello!"sv);
    EXPECT(string.has_short_ascii_storage());
}

TEST_CASE(widen_to_utf16_storage)
{
    Utf16StringBuilder builder;
    builder.append_ascii("hello "sv);
    builder.append_code_unit(0x263A);

    EXPECT_EQ(builder.view(), u"hello \u263A"sv);

    auto string = builder.to_string();
    EXPECT_EQ(string, u"hello \u263A"sv);
    EXPECT(!string.is_ascii());
}

TEST_CASE(append_code_point)
{
    Utf16StringBuilder builder;
    builder.append_ascii("wave "sv);
    builder.append_code_point(0x1F44B);

    auto string = builder.to_string();
    EXPECT_EQ(string, u"wave \U0001F44B"sv);
    EXPECT_EQ(string.length_in_code_units(), 7uz);
    EXPECT_EQ(string.length_in_code_points(), 6uz);
}

TEST_CASE(trim_ascii_storage)
{
    Utf16StringBuilder builder;
    builder.append_ascii("abc"sv);

    builder.trim(1);

    EXPECT_EQ(builder.view(), "ab"sv);
    EXPECT_EQ(builder.to_string(), "ab"sv);
}

TEST_CASE(trim_utf16_storage)
{
    Utf16StringBuilder builder;
    builder.append(u"ab\u0100"sv);

    builder.trim(1);

    EXPECT_EQ(builder.view(), "ab"sv);
    EXPECT_EQ(builder.to_string(), "ab"sv);
}

TEST_CASE(long_ascii_string)
{
    Utf16StringBuilder builder;
    builder.append_repeated_ascii('x', 300);

    auto string = builder.to_string();
    EXPECT_EQ(string.length_in_code_units(), 300uz);
    EXPECT(string.has_long_ascii_storage());
    EXPECT_EQ(string.utf16_view().code_unit_at(0), 'x');
    EXPECT_EQ(string.utf16_view().code_unit_at(299), 'x');
}

TEST_CASE(long_utf16_string)
{
    Utf16StringBuilder builder;
    builder.append_repeated(u"\u0100"sv, 300);

    auto string = builder.to_string();
    EXPECT_EQ(string.length_in_code_units(), 300uz);
    EXPECT(!string.is_ascii());
    EXPECT_EQ(string.utf16_view().code_unit_at(0), 0x0100);
    EXPECT_EQ(string.utf16_view().code_unit_at(299), 0x0100);
}

TEST_CASE(reuse_after_creating_long_utf16_string)
{
    Utf16StringBuilder builder;
    builder.append_repeated(u"\u0100"sv, 300);

    auto string = builder.to_string();
    EXPECT_EQ(string.length_in_code_units(), 300uz);
    EXPECT(!string.is_ascii());

    EXPECT(builder.is_empty());

    builder.append_ascii("ok"sv);

    EXPECT_EQ(builder.view(), "ok"sv);
    EXPECT_EQ(builder.to_string(), "ok"sv);
}

TEST_CASE(formatted_append)
{
    Utf16StringBuilder builder;
    builder.appendff("{} {} {}", "hello"sv, Utf16View { u"\u263A"sv }, 42);
    MUST(builder.try_appendff(" {}", Utf16String::formatted("{}", 3.14)));

    auto string = builder.to_string();
    EXPECT_EQ(string, u"hello \u263A 42 3.14"sv);
    EXPECT(!string.is_ascii());
}
