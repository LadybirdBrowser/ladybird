/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibURL/Pattern/Pattern.h>

TEST_CASE(url_pattern_matches_named_groups)
{
    auto pattern = MUST(URL::Pattern::Pattern::create("https://example.com/:category/:id"_string));
    auto result = MUST(pattern.match("https://example.com/books/42"_string, {}));
    VERIFY(result.has_value());

    EXPECT_EQ(result->protocol.input, "https"_string);
    EXPECT_EQ(result->pathname.input, "/books/42"_string);
    EXPECT_EQ(result->pathname.groups.get("category"sv).value(), (Variant<String, Empty> { "books"_string }));
    EXPECT_EQ(result->pathname.groups.get("id"sv).value(), (Variant<String, Empty> { "42"_string }));
}

TEST_CASE(url_pattern_ignore_case_matching)
{
    auto pattern = MUST(URL::Pattern::Pattern::create("https://example.com/:value"_string, {}, URL::Pattern::IgnoreCase::Yes));
    auto result = MUST(pattern.match("https://example.com/CaseSensitive"_string, {}));
    VERIFY(result.has_value());

    EXPECT_EQ(result->pathname.groups.get("value"sv).value(), (Variant<String, Empty> { "CaseSensitive"_string }));
}
