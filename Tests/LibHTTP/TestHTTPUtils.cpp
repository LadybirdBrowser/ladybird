/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/GenericLexer.h>
#include <AK/String.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/HTTP.h>
#include <LibHTTP/Header.h>

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

TEST_CASE(extract_cache_control_directive)
{
    EXPECT(!HTTP::contains_cache_control_directive({}, "no-cache"sv));
    EXPECT(!HTTP::contains_cache_control_directive(","sv, "no-cache"sv));

    EXPECT(!HTTP::contains_cache_control_directive("no-cache"sv, "no"sv));
    EXPECT(!HTTP::contains_cache_control_directive("no-cache"sv, "cache"sv));
    EXPECT(!HTTP::contains_cache_control_directive("no-cache"sv, "no cache"sv));

    EXPECT(!HTTP::contains_cache_control_directive("abno-cache"sv, "no-cache"sv));
    EXPECT(!HTTP::contains_cache_control_directive("no-cachecd"sv, "no-cache"sv));
    EXPECT(!HTTP::contains_cache_control_directive("abno-cachecd"sv, "no-cache"sv));

    EXPECT_EQ(HTTP::extract_cache_control_directive("no-cache"sv, "no-cache"sv), ""sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=4"sv, "max-age"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age = 4"sv, "max-age"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age= 4"sv, "max-age"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age =4"sv, "max-age"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age = 4 , no-cache"sv, "max-age"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("no-cache , max-age = 4"sv, "max-age"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("s-maxage=4, max-age=5"sv, "max-age"sv), "5"sv);

    EXPECT_EQ(HTTP::extract_cache_control_directive("Max-Age=4"sv, "max-age"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("MAX-AGE=4"sv, "max-age"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=4"sv, "MAX-AGE"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("No-Cache"sv, "no-cache"sv), ""sv);

    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=4,"sv, "max-age"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("no-cache,"sv, "no-cache"sv), ""sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("no-cache, "sv, "no-cache"sv), ""sv);

    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=4, max-age=5"sv, "max-age"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("no-cache, max-age=4, max-age=5"sv, "max-age"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=4, no-cache"sv, "no-cache"sv), ""sv);

    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=\"4\""sv, "max-age"sv), "\"4\""sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=\"004\""sv, "max-age"sv), "\"004\""sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=\"4\", no-cache"sv, "max-age"sv), "\"4\""sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("foo=\"bar\", max-age=\"4\""sv, "max-age"sv), "\"4\""sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=\"4,5\", no-cache"sv, "max-age"sv), "\"4,5\""sv);

    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=\"4\\5\""sv, "max-age"sv), "\"4\\5\""sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=\"4\\\"5\""sv, "max-age"sv), "\"4\\\"5\""sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=\"4\\\\5\""sv, "max-age"sv), "\"4\\\\5\""sv);

    EXPECT(!HTTP::contains_cache_control_directive("max-age\"4\""sv, "max-age"sv));
    EXPECT(!HTTP::contains_cache_control_directive("max-age=\"4"sv, "max-age"sv));
    EXPECT(!HTTP::contains_cache_control_directive("foo=\"bar, max-age=4"sv, "max-age"sv));
    EXPECT(!HTTP::contains_cache_control_directive("\"unterminated"sv, "max-age"sv));
    EXPECT(!HTTP::contains_cache_control_directive("max-age=\"4, no-cache"sv, "max-age"sv));
    EXPECT(!HTTP::contains_cache_control_directive("max-age=\"4, no-cache"sv, "no-cache"sv));

    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=\"4, no-cache\", foo=bar"sv, "max-age"sv), "\"4, no-cache\""sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=\"4, no-cache\", foo=bar"sv, "foo"sv), "bar"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("foo=\"bar,baz\", max-age=4"sv, "foo"sv), "\"bar,baz\""sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("foo=\"bar,baz\", max-age=4"sv, "max-age"sv), "4"sv);

    EXPECT_EQ(HTTP::extract_cache_control_directive(",,max-age=4"sv, "max-age"sv), "4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age==4"sv, "max-age"sv), "=4"sv);
    EXPECT_EQ(HTTP::extract_cache_control_directive("max-age=4="sv, "max-age"sv), "4="sv);
    EXPECT(!HTTP::contains_cache_control_directive("=4"sv, "max-age"sv));
}

TEST_CASE(extract_header_values)
{
    struct TestHeader {
        StringView name;
        bool requires_at_least_one; // true = 1#token, false = #token
    };

    TestHeader const headers[] = {
        { "Access-Control-Expose-Headers"sv, false },
        { "access-control-expose-headers"sv, false },
        { "Access-Control-Allow-Headers"sv, false },
        { "Access-Control-Allow-Methods"sv, false },
        { "Access-Control-Request-Headers"sv, true },
        { "Accept-Ranges"sv, true },
    };

    for (auto const& [header, requires_at_least_one] : headers) {
        // Valid single token.
        auto result = HTTP::Header { header, "bb-8"sv }.extract_header_values();
        EXPECT_EQ(result, (Vector<ByteString> { "bb-8" }));

        // Valid multiple tokens, whitespace trimmed.
        result = HTTP::Header { header, "bb-8, no"sv }.extract_header_values();
        EXPECT_EQ(result, (Vector<ByteString> { "bb-8", "no" }));

        // Wildcard is a valid token.
        result = HTTP::Header { header, "*"sv }.extract_header_values();
        EXPECT_EQ(result, (Vector<ByteString> { "*" }));

        // Single-quoted tokens are valid (apostrophe is a tchar).
        result = HTTP::Header { header, "'bb-8',bb-8"sv }.extract_header_values();
        EXPECT_EQ(result, (Vector<ByteString> { "'bb-8'", "bb-8" }));

        // Leading/trailing commas: empty parts discarded.
        result = HTTP::Header { header, ",bb-8,"sv }.extract_header_values();
        EXPECT_EQ(result, (Vector<ByteString> { "bb-8" }));

        // Empty value and only-commas.
        result = HTTP::Header { header, ""sv }.extract_header_values();
        if (requires_at_least_one) {
            EXPECT_EQ(result, OptionalNone {});
        } else {
            EXPECT_EQ(result, (Vector<ByteString> {}));
        }

        result = HTTP::Header { header, ",,,"sv }.extract_header_values();
        if (requires_at_least_one) {
            EXPECT_EQ(result, OptionalNone {});
        } else {
            EXPECT_EQ(result, (Vector<ByteString> {}));
        }

        // Space inside a token is invalid.
        result = HTTP::Header { header, "no no"sv }.extract_header_values();
        EXPECT_EQ(result, OptionalNone {});

        // Double-quote is invalid.
        result = HTTP::Header { header, "\"bb-8\",bb-8"sv }.extract_header_values();
        EXPECT_EQ(result, OptionalNone {});

        // @ is invalid.
        result = HTTP::Header { header, "@invalid,bb-8"sv }.extract_header_values();
        EXPECT_EQ(result, OptionalNone {});

        // Vertical tab (0x0B) is invalid.
        result = HTTP::Header { header, "bb-8\x0B"sv }.extract_header_values();
        EXPECT_EQ(result, OptionalNone {});

        // Form feed (0x0C) is invalid.
        result = HTTP::Header { header, "bb-8\x0C"sv }.extract_header_values();
        EXPECT_EQ(result, OptionalNone {});

        // Invalid token alongside a valid one still fails.
        result = HTTP::Header { header, "bb-8,no no"sv }.extract_header_values();
        EXPECT_EQ(result, OptionalNone {});

        // Whitespace-only item between commas fails.
        result = HTTP::Header { header, "bb-8,  ,no"sv }.extract_header_values();
        EXPECT_EQ(result, OptionalNone {});
    }

    // Other headers: returned as a single-element list regardless of content.
    auto result = HTTP::Header { "Content-Type"sv, "text/html; charset=utf-8"sv }.extract_header_values();
    EXPECT_EQ(result, (Vector<ByteString> { "text/html; charset=utf-8" }));
}
