/*
 * Copyright (c) 2023, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibWeb/CSS/Keyword.h>

TEST_CASE(basic)
{
    EXPECT_EQ(Web::CSS::keyword_from_string("italic"_sv).value(), Web::CSS::Keyword::Italic);
    EXPECT_EQ(Web::CSS::keyword_from_string("inline"_sv).value(), Web::CSS::Keyword::Inline);
    EXPECT_EQ(Web::CSS::keyword_from_string("small"_sv).value(), Web::CSS::Keyword::Small);
    EXPECT_EQ(Web::CSS::keyword_from_string("smalL"_sv).value(), Web::CSS::Keyword::Small);
    EXPECT_EQ(Web::CSS::keyword_from_string("SMALL"_sv).value(), Web::CSS::Keyword::Small);
    EXPECT_EQ(Web::CSS::keyword_from_string("Small"_sv).value(), Web::CSS::Keyword::Small);
    EXPECT_EQ(Web::CSS::keyword_from_string("smALl"_sv).value(), Web::CSS::Keyword::Small);
}

BENCHMARK_CASE(keyword_from_string)
{
    for (size_t i = 0; i < 10'000'000; ++i) {
        EXPECT_EQ(Web::CSS::keyword_from_string("inline"_sv).value(), Web::CSS::Keyword::Inline);
    }
}
