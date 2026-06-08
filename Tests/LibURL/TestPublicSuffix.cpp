/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibURL/Parser.h>
#include <LibURL/PublicSuffixData.h>

TEST_CASE(public_suffix_matching_for_psl_rules)
{
    struct TestCase {
        StringView input;
        bool is_public_suffix;
        Optional<StringView> public_suffix;
        Optional<StringView> registrable_domain;
    };

    TestCase test_cases[] {
        { "com"sv, true, "com"sv, OptionalNone {} },
        { "COM"sv, true, "com"sv, OptionalNone {} },
        { ".com"sv, true, "com"sv, OptionalNone {} },
        { "com."sv, true, "com."sv, OptionalNone {} },
        { ".com."sv, true, "com."sv, OptionalNone {} },
        { "..com."sv, true, "com."sv, OptionalNone {} },
        { "example.com"sv, false, "com"sv, "example.com"sv },
        { "EXAMPLE.COM"sv, false, "com"sv, "example.com"sv },
        { ".example.com"sv, false, "com"sv, "example.com"sv },
        { "www.example.com"sv, false, "com"sv, "example.com"sv },
        { "sub.www.example.com"sv, false, "com"sv, "example.com"sv },
        { "www.example.com."sv, false, "com."sv, "example.com."sv },
        { "not-a-public-suffix.com"sv, false, "com"sv, "not-a-public-suffix.com"sv },
        { "com.br"sv, true, "com.br"sv, OptionalNone {} },
        { "not-a-public-suffix.com.br"sv, false, "com.br"sv, "not-a-public-suffix.com.br"sv },
        { "co.uk"sv, true, "co.uk"sv, OptionalNone {} },
        { "ac.uk"sv, true, "ac.uk"sv, OptionalNone {} },
        { "gov.uk"sv, true, "gov.uk"sv, OptionalNone {} },
        { "com.au"sv, true, "com.au"sv, OptionalNone {} },
        { "co.jp"sv, true, "co.jp"sv, OptionalNone {} },
        { "bbc.co.uk"sv, false, "co.uk"sv, "bbc.co.uk"sv },
        { "www.bbc.co.uk"sv, false, "co.uk"sv, "bbc.co.uk"sv },
        { "github.io"sv, true, "github.io"sv, OptionalNone {} },
        { "ladybird.github.io"sv, false, "github.io"sv, "ladybird.github.io"sv },
        { "whatwg.github.io"sv, false, "github.io"sv, "whatwg.github.io"sv },
        { "公司.cn"sv, true, "xn--55qx5d.cn"sv, OptionalNone {} },
        { "www.公司.cn"sv, false, "xn--55qx5d.cn"sv, "www.xn--55qx5d.cn"sv },
        { "www.xn--55qx5d.cn"sv, false, "xn--55qx5d.cn"sv, "www.xn--55qx5d.cn"sv },
    };

    for (auto const& test_case : test_cases) {
        EXPECT_EQ(URL::PublicSuffixData::is_matching_public_suffix(test_case.input), test_case.is_public_suffix);
        EXPECT_EQ(URL::PublicSuffixData::find_matching_public_suffix(test_case.input), test_case.public_suffix);
        EXPECT_EQ(URL::PublicSuffixData::find_matching_registrable_domain(test_case.input), test_case.registrable_domain);

        auto host = URL::Parser::parse_host(test_case.input);
        VERIFY(host.has_value());
        EXPECT_EQ(URL::PublicSuffixData::is_matching_public_suffix(*host), test_case.is_public_suffix);
        EXPECT_EQ(URL::PublicSuffixData::find_matching_public_suffix(*host), test_case.public_suffix);
        EXPECT_EQ(URL::PublicSuffixData::find_matching_registrable_domain(*host), test_case.registrable_domain);
        EXPECT_EQ(host->public_suffix(), test_case.public_suffix);
        EXPECT_EQ(host->registrable_domain(), test_case.registrable_domain);
    }
}

TEST_CASE(public_suffix_matching_without_psl_rule)
{
    struct TestCase {
        StringView input;
        Optional<StringView> host_public_suffix;
        Optional<StringView> host_registrable_domain;
    };

    TestCase test_cases[] {
        { "foobar"sv, "foobar"sv, OptionalNone {} },
        { "foobar."sv, "foobar."sv, OptionalNone {} },
        { "not-a-public-suffix"sv, "not-a-public-suffix"sv, OptionalNone {} },
        { "a.example"sv, "example"sv, "a.example"sv },
        { "a.example."sv, "example."sv, "a.example."sv },
        { "b.b.example"sv, "example"sv, "b.example"sv },
        { "b.b.example."sv, "example."sv, "b.example."sv },
        { "foo.not-a-public-suffix"sv, "not-a-public-suffix"sv, "foo.not-a-public-suffix"sv },
        { "sub.foo.not-a-public-suffix"sv, "not-a-public-suffix"sv, "foo.not-a-public-suffix"sv },
        { "إختبار"sv, "xn--kgbechtv"sv, OptionalNone {} },
        { "example.إختبار"sv, "xn--kgbechtv"sv, "example.xn--kgbechtv"sv },
        { "example.إختبار."sv, "xn--kgbechtv."sv, "example.xn--kgbechtv."sv },
        { "sub.example.إختبار"sv, "xn--kgbechtv"sv, "example.xn--kgbechtv"sv },
    };

    for (auto const& test_case : test_cases) {
        EXPECT(!URL::PublicSuffixData::is_matching_public_suffix(test_case.input));
        EXPECT_EQ(URL::PublicSuffixData::find_matching_public_suffix(test_case.input), OptionalNone {});
        EXPECT_EQ(URL::PublicSuffixData::find_matching_registrable_domain(test_case.input), OptionalNone {});

        auto host = URL::Parser::parse_host(test_case.input);
        VERIFY(host.has_value());
        EXPECT(!URL::PublicSuffixData::is_matching_public_suffix(*host));
        EXPECT_EQ(URL::PublicSuffixData::find_matching_public_suffix(*host), OptionalNone {});
        EXPECT_EQ(URL::PublicSuffixData::find_matching_registrable_domain(*host), OptionalNone {});
        EXPECT_EQ(host->public_suffix(), test_case.host_public_suffix);
        EXPECT_EQ(host->registrable_domain(), test_case.host_registrable_domain);
    }
}

TEST_CASE(invalid_hosts)
{
    StringView raw_invalid_inputs[] {
        " "sv,
        "/"sv,
        "com/"sv,
        "/com"sv,
        " com"sv,
        "com "sv,
    };

    // Above inputs are not valid hosts, so should not be able to be parsed or matched in the PSL.
    for (auto const& input : raw_invalid_inputs) {
        auto host = URL::Parser::parse_host(input);
        EXPECT(!host.has_value());
        EXPECT(!URL::PublicSuffixData::is_matching_public_suffix(input));
        EXPECT_EQ(URL::PublicSuffixData::find_matching_public_suffix(input), OptionalNone {});
        EXPECT_EQ(URL::PublicSuffixData::find_matching_registrable_domain(input), OptionalNone {});
    }
}

TEST_CASE(public_suffix_matching_for_ip_addresses)
{
    StringView test_cases[] {
        "127.0.0.1"sv,
        "[2001:0db8:85a3:0000:0000:8a2e:0370:7334]"sv,
    };

    for (auto const& input : test_cases) {
        EXPECT(!URL::PublicSuffixData::is_matching_public_suffix(input));
        EXPECT_EQ(URL::PublicSuffixData::find_matching_public_suffix(input), OptionalNone {});
        EXPECT_EQ(URL::PublicSuffixData::find_matching_registrable_domain(input), OptionalNone {});

        auto host = URL::Parser::parse_host(input);
        VERIFY(host.has_value());
        EXPECT(!URL::PublicSuffixData::is_matching_public_suffix(*host));
        EXPECT_EQ(URL::PublicSuffixData::find_matching_public_suffix(*host), OptionalNone {});
        EXPECT_EQ(URL::PublicSuffixData::find_matching_registrable_domain(*host), OptionalNone {});
        EXPECT_EQ(host->public_suffix(), OptionalNone {});
        EXPECT_EQ(host->registrable_domain(), OptionalNone {});
    }
}
