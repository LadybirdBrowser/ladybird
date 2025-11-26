/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/GenericLexer.h>
#include <AK/String.h>
#include <LibHTTP/HTTP.h>

TEST_CASE(collect_an_http_quoted_string)
{
    {
        auto test = "\"\""_string;
        GenericLexer lexer { test };

        auto result = HTTP::collect_an_http_quoted_string(lexer);
        EXPECT_EQ(result, "\"\""_string);
    }
    {
        auto test = "\"abc\""_string;
        GenericLexer lexer { test };

        auto result = HTTP::collect_an_http_quoted_string(lexer);
        EXPECT_EQ(result, "\"abc\""_string);
    }
    {
        auto test = "foo \"abc\""_string;

        GenericLexer lexer { test };
        lexer.ignore(4);

        auto result = HTTP::collect_an_http_quoted_string(lexer);
        EXPECT_EQ(result, "\"abc\""_string);
    }
    {
        auto test = "foo=\"abc\""_string;

        GenericLexer lexer { test };
        lexer.ignore(4);

        auto result = HTTP::collect_an_http_quoted_string(lexer);
        EXPECT_EQ(result, "\"abc\""_string);
    }
    {
        auto test = "foo=\"abc\" bar"_string;

        GenericLexer lexer { test };
        lexer.ignore(4);

        auto result = HTTP::collect_an_http_quoted_string(lexer);
        EXPECT_EQ(result, "\"abc\""_string);
    }
    {
        auto test = "\"abc\" bar"_string;
        GenericLexer lexer { test };

        auto result = HTTP::collect_an_http_quoted_string(lexer);
        EXPECT_EQ(result, "\"abc\""_string);
    }
}
