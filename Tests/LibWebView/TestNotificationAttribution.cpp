/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibWebView/NotificationAttribution.h>

static URL::Origin origin_of(StringView url)
{
    return URL::Parser::basic_parse(url)->origin();
}

TEST_CASE(https_origin_shows_its_host_without_the_scheme)
{
    EXPECT_EQ(WebView::attribution_for_origin(origin_of("https://example.com/mail/inbox"sv)), "example.com"_string);
}

TEST_CASE(http_origin_shows_its_host_without_the_scheme)
{
    EXPECT_EQ(WebView::attribution_for_origin(origin_of("http://example.com/"sv)), "example.com"_string);
}

TEST_CASE(non_default_port_stays)
{
    EXPECT_EQ(WebView::attribution_for_origin(origin_of("http://example.com:8080/"sv)), "example.com:8080"_string);
}

TEST_CASE(ipv6_host_keeps_its_brackets_and_port)
{
    EXPECT_EQ(WebView::attribution_for_origin(origin_of("https://[::1]:8443/"sv)), "[::1]:8443"_string);
}

TEST_CASE(other_schemes_keep_their_scheme)
{
    EXPECT_EQ(WebView::attribution_for_origin(origin_of("ftp://files.example.com/"sv)), "ftp://files.example.com"_string);
}

TEST_CASE(opaque_origin_is_this_page)
{
    EXPECT_EQ(WebView::attribution_for_origin(URL::Origin::create_opaque()), "this page"_string);
}
