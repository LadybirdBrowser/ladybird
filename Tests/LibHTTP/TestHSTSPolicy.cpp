/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibHTTP/HSTS/ParsedHSTSPolicy.h>

TEST_CASE(parses_max_age_token)
{
    auto policy = HTTP::HSTS::parse_header("max-age=31536000"sv);
    EXPECT(policy.has_value());
    EXPECT_EQ(policy->max_age, AK::Duration::from_seconds(31536000));
    EXPECT(!policy->include_sub_domains);
}

TEST_CASE(parses_max_age_quoted)
{
    auto policy = HTTP::HSTS::parse_header("max-age=\"31536000\""sv);
    EXPECT(policy.has_value());
    EXPECT_EQ(policy->max_age, AK::Duration::from_seconds(31536000));
}

TEST_CASE(parses_include_sub_domains)
{
    auto policy = HTTP::HSTS::parse_header("max-age=100; includeSubDomains"sv);
    EXPECT(policy.has_value());
    EXPECT_EQ(policy->max_age, AK::Duration::from_seconds(100));
    EXPECT(policy->include_sub_domains);
}

TEST_CASE(directive_order_does_not_matter)
{
    auto policy = HTTP::HSTS::parse_header("includeSubDomains; max-age=100"sv);
    EXPECT(policy.has_value());
    EXPECT_EQ(policy->max_age, AK::Duration::from_seconds(100));
    EXPECT(policy->include_sub_domains);
}

TEST_CASE(directive_names_are_case_insensitive)
{
    auto policy = HTTP::HSTS::parse_header("MAX-AGE=100; INCLUDESUBDOMAINS"sv);
    EXPECT(policy.has_value());
    EXPECT_EQ(policy->max_age, AK::Duration::from_seconds(100));
    EXPECT(policy->include_sub_domains);
}

TEST_CASE(unknown_directives_are_ignored)
{
    auto policy = HTTP::HSTS::parse_header("max-age=100; foo=bar; baz"sv);
    EXPECT(policy.has_value());
    EXPECT_EQ(policy->max_age, AK::Duration::from_seconds(100));
}

TEST_CASE(quoted_value_in_unknown_directive_does_not_synthesise_directives)
{
    // A ';' inside an unknown directive's quoted-string value must not be mistaken for a directive
    // separator. With a quote-blind parser the embedded "max-age=42" would be parsed as a real
    // directive.
    auto policy = HTTP::HSTS::parse_header("foo=\"x; max-age=42; y\""sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(missing_max_age_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header("includeSubDomains"sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(empty_header_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header(""sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(non_numeric_max_age_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header("max-age=abc"sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(duplicate_max_age_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header("max-age=100; max-age=200"sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(duplicate_include_sub_domains_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header("max-age=100; includeSubDomains; includeSubDomains"sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(value_on_include_sub_domains_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header("max-age=100; includeSubDomains=false"sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(unterminated_quoted_max_age_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header("max-age=\"31536000"sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(trailing_garbage_after_quoted_value_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header("max-age=\"100\" trailing"sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(empty_token_value_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header("foo=; max-age=100"sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(non_token_directive_name_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header("max-age=100; @=bar"sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(non_token_unquoted_value_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header("foo=@; max-age=100"sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(internal_whitespace_in_directive_name_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header("max age=100"sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(internal_whitespace_in_unquoted_value_is_rejected)
{
    auto policy = HTTP::HSTS::parse_header("max-age=10 0"sv);
    EXPECT(!policy.has_value());
}

TEST_CASE(consecutive_semicolons_are_tolerated)
{
    auto policy = HTTP::HSTS::parse_header(";; max-age=100;"sv);
    EXPECT(policy.has_value());
    EXPECT_EQ(policy->max_age, AK::Duration::from_seconds(100));
}
