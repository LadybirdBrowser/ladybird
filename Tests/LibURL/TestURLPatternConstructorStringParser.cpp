/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibURL/Pattern/ConstructorStringParser.h>

TEST_CASE(basic_http_url_no_pattern_or_path)
{
    auto input = "http://www.serenityos.org"_string;
    auto result = MUST(URL::Pattern::ConstructorStringParser::parse(input.code_points()));
    EXPECT_EQ(result.protocol, "http"_sv);
    EXPECT_EQ(result.username, OptionalNone {});
    EXPECT_EQ(result.password, OptionalNone {});
    EXPECT_EQ(result.hostname, "www.serenityos.org"_sv);
    EXPECT_EQ(result.port, ""_sv);
    EXPECT_EQ(result.pathname, OptionalNone {});
    EXPECT_EQ(result.search, OptionalNone {});
    EXPECT_EQ(result.hash, OptionalNone {});
    EXPECT_EQ(result.base_url, OptionalNone {});
}

TEST_CASE(pathname_with_regexp)
{
    auto input = "/books/(\\d+)"_string;
    auto result = MUST(URL::Pattern::ConstructorStringParser::parse(input.code_points()));
    EXPECT_EQ(result.protocol, OptionalNone {});
    EXPECT_EQ(result.username, OptionalNone {});
    EXPECT_EQ(result.password, OptionalNone {});
    EXPECT_EQ(result.hostname, OptionalNone {});
    EXPECT_EQ(result.port, OptionalNone {});
    EXPECT_EQ(result.pathname, "/books/(\\d+)"_sv);
    EXPECT_EQ(result.search, OptionalNone {});
    EXPECT_EQ(result.hash, OptionalNone {});
    EXPECT_EQ(result.base_url, OptionalNone {});
}

TEST_CASE(url_with_pathname_and_regexp)
{
    auto input = "https://example.com/2022/feb/*"_string;
    auto result = MUST(URL::Pattern::ConstructorStringParser::parse(input.code_points()));
    EXPECT_EQ(result.protocol, "https"_sv);
    EXPECT_EQ(result.username, OptionalNone {});
    EXPECT_EQ(result.password, OptionalNone {});
    EXPECT_EQ(result.hostname, "example.com"_sv);
    EXPECT_EQ(result.port, ""_sv);
    EXPECT_EQ(result.pathname, "/2022/feb/*"_sv);
    EXPECT_EQ(result.search, OptionalNone {});
    EXPECT_EQ(result.hash, OptionalNone {});
    EXPECT_EQ(result.base_url, OptionalNone {});
}

TEST_CASE(http_url_regexp_in_pathname_and_hostname)
{
    auto input = "https://cdn-*.example.com/*.jpg"_string;
    auto result = MUST(URL::Pattern::ConstructorStringParser::parse(input.code_points()));
    EXPECT_EQ(result.protocol, "https"_sv);
    EXPECT_EQ(result.username, OptionalNone {});
    EXPECT_EQ(result.password, OptionalNone {});
    EXPECT_EQ(result.hostname, "cdn-*.example.com"_sv);
    EXPECT_EQ(result.port, ""_sv);
    EXPECT_EQ(result.pathname, "/*.jpg"_sv);
    EXPECT_EQ(result.search, OptionalNone {});
    EXPECT_EQ(result.hash, OptionalNone {});
    EXPECT_EQ(result.base_url, OptionalNone {});
}

TEST_CASE(https_url_with_fragment)
{
    auto input = "https://example.com/#foo"_string;
    auto result = MUST(URL::Pattern::ConstructorStringParser::parse(input.code_points()));
    EXPECT_EQ(result.protocol, "https"_sv);
    EXPECT_EQ(result.username, OptionalNone {});
    EXPECT_EQ(result.password, OptionalNone {});
    EXPECT_EQ(result.hostname, "example.com"_sv);
    EXPECT_EQ(result.port, ""_sv);
    EXPECT_EQ(result.pathname, "/"_sv);
    EXPECT_EQ(result.search, ""_sv);
    EXPECT_EQ(result.hash, "foo"_sv);
    EXPECT_EQ(result.base_url, OptionalNone {});
}

TEST_CASE(http_url_with_query)
{
    auto input = "https://example.com/?q=*&v=?&hmm={}&umm=()"_string;
    auto result = MUST(URL::Pattern::ConstructorStringParser::parse(input.code_points()));
    EXPECT_EQ(result.protocol, "https"_sv);
    EXPECT_EQ(result.username, OptionalNone {});
    EXPECT_EQ(result.password, OptionalNone {});
    EXPECT_EQ(result.hostname, "example.com"_sv);
    EXPECT_EQ(result.port, ""_sv);
    EXPECT_EQ(result.pathname, "/"_sv);
    EXPECT_EQ(result.search, "q=*&v=?&hmm={}&umm=()"_sv);
    EXPECT_EQ(result.hash, OptionalNone {});
    EXPECT_EQ(result.base_url, OptionalNone {});
}

TEST_CASE(matches_on_sub_url)
{
    auto input = "https://{sub.}?example.com/foo"_string;
    auto result = MUST(URL::Pattern::ConstructorStringParser::parse(input.code_points()));
    EXPECT_EQ(result.protocol, "https"_sv);
    EXPECT_EQ(result.username, OptionalNone {});
    EXPECT_EQ(result.password, OptionalNone {});
    EXPECT_EQ(result.hostname, "{sub.}?example.com"_sv);
    EXPECT_EQ(result.port, ""_sv);
    EXPECT_EQ(result.pathname, "/foo"_sv);
    EXPECT_EQ(result.search, OptionalNone {});
    EXPECT_EQ(result.hash, OptionalNone {});
    EXPECT_EQ(result.base_url, OptionalNone {});
}

TEST_CASE(ipv6_with_port_number)
{
    auto input = "http://[\\:\\:1]:8080"_string;
    auto result = MUST(URL::Pattern::ConstructorStringParser::parse(input.code_points()));
    EXPECT_EQ(result.protocol, "http"_sv);
    EXPECT_EQ(result.username, OptionalNone {});
    EXPECT_EQ(result.password, OptionalNone {});
    EXPECT_EQ(result.hostname, "[\\:\\:1]"_sv);
    EXPECT_EQ(result.port, "8080"_sv);
    EXPECT_EQ(result.pathname, OptionalNone {});
    EXPECT_EQ(result.search, OptionalNone {});
    EXPECT_EQ(result.hash, OptionalNone {});
    EXPECT_EQ(result.base_url, OptionalNone {});
}

TEST_CASE(data_url)
{
    auto input = "data\\:foobar"_string;
    auto result = MUST(URL::Pattern::ConstructorStringParser::parse(input.code_points()));
    EXPECT_EQ(result.protocol, "data"_sv);
    EXPECT_EQ(result.username, OptionalNone {});
    EXPECT_EQ(result.password, OptionalNone {});
    EXPECT_EQ(result.hostname, ""_sv);
    EXPECT_EQ(result.port, ""_sv);
    EXPECT_EQ(result.pathname, "foobar"_sv);
    EXPECT_EQ(result.search, OptionalNone {});
    EXPECT_EQ(result.hash, OptionalNone {});
    EXPECT_EQ(result.base_url, OptionalNone {});
}

TEST_CASE(non_special_scheme_and_arbitary_hostname)
{
    auto input = "foo://bar"_string;
    auto result = MUST(URL::Pattern::ConstructorStringParser::parse(input.code_points()));
    EXPECT_EQ(result.protocol, "foo"_sv);
    EXPECT_EQ(result.username, OptionalNone {});
    EXPECT_EQ(result.password, OptionalNone {});
    EXPECT_EQ(result.hostname, "bar"_sv);
    EXPECT_EQ(result.port, ""_sv);
    EXPECT_EQ(result.pathname, OptionalNone {});
    EXPECT_EQ(result.search, OptionalNone {});
    EXPECT_EQ(result.hash, OptionalNone {});
    EXPECT_EQ(result.base_url, OptionalNone {});
}

TEST_CASE(ipv6_with_named_group)
{
    auto input = "http://[:address]/"_string;
    auto result = MUST(URL::Pattern::ConstructorStringParser::parse(input.code_points()));
    EXPECT_EQ(result.protocol, "http"_sv);
    EXPECT_EQ(result.username, OptionalNone {});
    EXPECT_EQ(result.password, OptionalNone {});
    EXPECT_EQ(result.hostname, "[:address]"_sv);
    EXPECT_EQ(result.port, ""_sv);
    EXPECT_EQ(result.pathname, "/"_sv);
    EXPECT_EQ(result.search, OptionalNone {});
    EXPECT_EQ(result.hash, OptionalNone {});
    EXPECT_EQ(result.base_url, OptionalNone {});
}
