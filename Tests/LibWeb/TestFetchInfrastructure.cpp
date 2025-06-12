/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/GenericLexer.h>
#include <AK/String.h>
#include <LibWeb/Fetch/Infrastructure/HTTP.h>

TEST_CASE(collect_an_http_quoted_string)
{
    {
        auto test = "\"\""_string;
        GenericLexer lexer { test };

        auto result = Web::Fetch::Infrastructure::collect_an_http_quoted_string(lexer);
        EXPECT_EQ(result, "\"\""_string);
    }
    {
        auto test = "\"abc\""_string;
        GenericLexer lexer { test };

        auto result = Web::Fetch::Infrastructure::collect_an_http_quoted_string(lexer);
        EXPECT_EQ(result, "\"abc\""_string);
    }
    {
        auto test = "foo \"abc\""_string;

        GenericLexer lexer { test };
        lexer.ignore(4);

        auto result = Web::Fetch::Infrastructure::collect_an_http_quoted_string(lexer);
        EXPECT_EQ(result, "\"abc\""_string);
    }
    {
        auto test = "foo=\"abc\""_string;

        GenericLexer lexer { test };
        lexer.ignore(4);

        auto result = Web::Fetch::Infrastructure::collect_an_http_quoted_string(lexer);
        EXPECT_EQ(result, "\"abc\""_string);
    }
    {
        auto test = "foo=\"abc\" bar"_string;

        GenericLexer lexer { test };
        lexer.ignore(4);

        auto result = Web::Fetch::Infrastructure::collect_an_http_quoted_string(lexer);
        EXPECT_EQ(result, "\"abc\""_string);
    }
    {
        auto test = "\"abc\" bar"_string;
        GenericLexer lexer { test };

        auto result = Web::Fetch::Infrastructure::collect_an_http_quoted_string(lexer);
        EXPECT_EQ(result, "\"abc\""_string);
    }
}
