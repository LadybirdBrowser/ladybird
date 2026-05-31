/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/SecureContexts/AbstractOperations.h>

static bool is_potentially_trustworthy(StringView url_string)
{
    auto url = URL::Parser::basic_parse(url_string);
    VERIFY(url.has_value());
    return Web::SecureContexts::is_url_potentially_trustworthy(*url) == Web::SecureContexts::Trustworthiness::PotentiallyTrustworthy;
}

TEST_CASE(ipv4_loopback_is_potentially_trustworthy)
{
    EXPECT(is_potentially_trustworthy("http://127.0.0.1/"sv));
    // The whole 127.0.0.0/8 block is loopback, not just 127.0.0.1.
    EXPECT(is_potentially_trustworthy("http://127.0.0.2/"sv));
    EXPECT(is_potentially_trustworthy("http://127.0.0.0/"sv));
    EXPECT(is_potentially_trustworthy("http://127.255.255.255/"sv));
}

TEST_CASE(non_loopback_ipv4_is_not_potentially_trustworthy)
{
    EXPECT(!is_potentially_trustworthy("http://8.8.8.8/"sv));
    EXPECT(!is_potentially_trustworthy("http://1.1.1.1/"sv));
    EXPECT(!is_potentially_trustworthy("http://192.168.1.1/"sv));
    // A non-zero final octet of 127 must not be mistaken for the leading octet.
    EXPECT(!is_potentially_trustworthy("http://8.8.8.127/"sv));
    // Just outside 127.0.0.0/8 on either side.
    EXPECT(!is_potentially_trustworthy("http://126.0.0.1/"sv));
    EXPECT(!is_potentially_trustworthy("http://128.0.0.1/"sv));
}

TEST_CASE(ipv6_loopback_is_potentially_trustworthy)
{
    EXPECT(is_potentially_trustworthy("http://[::1]/"sv));
    EXPECT(!is_potentially_trustworthy("http://[::2]/"sv));
}

TEST_CASE(localhost_is_potentially_trustworthy)
{
    EXPECT(is_potentially_trustworthy("http://localhost/"sv));
    EXPECT(is_potentially_trustworthy("http://foo.localhost/"sv));
    EXPECT(!is_potentially_trustworthy("http://example.com/"sv));
}

TEST_CASE(https_and_wss_are_potentially_trustworthy)
{
    EXPECT(is_potentially_trustworthy("https://example.com/"sv));
    EXPECT(is_potentially_trustworthy("wss://example.com/"sv));
    EXPECT(!is_potentially_trustworthy("http://example.com/"sv));
    EXPECT(!is_potentially_trustworthy("ws://example.com/"sv));
}
