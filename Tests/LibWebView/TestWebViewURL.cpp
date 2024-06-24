/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/URL.h>

static void compare_url_parts(StringView url, WebView::URLParts const& expected)
{
    auto result = WebView::break_url_into_parts(url);
    VERIFY(result.has_value());

    EXPECT_EQ(result->scheme_and_subdomain, expected.scheme_and_subdomain);
    EXPECT_EQ(result->effective_tld_plus_one, expected.effective_tld_plus_one);
    EXPECT_EQ(result->remainder, expected.remainder);
}

static bool is_sanitized_url_the_same(StringView url)
{
    auto sanitized_url = WebView::sanitize_url(url);
    if (!sanitized_url.has_value())
        return false;
    return sanitized_url->to_string().value() == url;
}

TEST_CASE(invalid_url)
{
    EXPECT(!WebView::break_url_into_parts(""sv).has_value());
    EXPECT(!WebView::break_url_into_parts(":"sv).has_value());
    EXPECT(!WebView::break_url_into_parts(":/"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("://"sv).has_value());

    EXPECT(!WebView::break_url_into_parts("/"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("//"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("/h"sv).has_value());

    EXPECT(!WebView::break_url_into_parts("f"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("fi"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("fil"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("file"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("file:"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("file:/"sv).has_value());

    EXPECT(!WebView::break_url_into_parts("h"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("ht"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("htt"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("http"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("http:"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("http:/"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("http://"sv).has_value());

    EXPECT(!WebView::break_url_into_parts("https"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("https:"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("https:/"sv).has_value());
    EXPECT(!WebView::break_url_into_parts("https://"sv).has_value());
}

TEST_CASE(file_url)
{
    compare_url_parts("file://"sv, { "file://"sv, ""sv, {} });
    compare_url_parts("file://a"sv, { "file://"sv, "a"sv, {} });
    compare_url_parts("file:///a"sv, { "file://"sv, "/a"sv, {} });
    compare_url_parts("file:///abc"sv, { "file://"sv, "/abc"sv, {} });
}

TEST_CASE(http_url)
{
    compare_url_parts("http://a"sv, { "http://"sv, "a"sv, {} });
    compare_url_parts("http://abc"sv, { "http://"sv, "abc"sv, {} });
    compare_url_parts("http://com"sv, { "http://"sv, "com"sv, {} });
    compare_url_parts("http://abc."sv, { "http://"sv, "abc."sv, {} });
    compare_url_parts("http://abc.c"sv, { "http://"sv, "abc.c"sv, {} });
    compare_url_parts("http://abc.com"sv, { "http://"sv, "abc.com"sv, {} });
    compare_url_parts("http://abc.com."sv, { "http://"sv, "abc.com."sv, {} });
    compare_url_parts("http://abc.com."sv, { "http://"sv, "abc.com."sv, {} });
    compare_url_parts("http://abc.com.org"sv, { "http://abc."sv, "com.org"sv, {} });
    compare_url_parts("http://abc.com.org.gov"sv, { "http://abc.com."sv, "org.gov"sv, {} });

    compare_url_parts("http://abc/path"sv, { "http://"sv, "abc"sv, "/path"sv });
    compare_url_parts("http://abc#anchor"sv, { "http://"sv, "abc"sv, "#anchor"sv });
    compare_url_parts("http://abc?query"sv, { "http://"sv, "abc"sv, "?query"sv });

    compare_url_parts("http://abc.def.com"sv, { "http://abc."sv, "def.com"sv, {} });
    compare_url_parts("http://abc.def.com/path"sv, { "http://abc."sv, "def.com"sv, "/path"sv });
    compare_url_parts("http://abc.def.com#anchor"sv, { "http://abc."sv, "def.com"sv, "#anchor"sv });
    compare_url_parts("http://abc.def.com?query"sv, { "http://abc."sv, "def.com"sv, "?query"sv });
}

TEST_CASE(about_url)
{
    EXPECT(!is_sanitized_url_the_same("about"sv));
    EXPECT(!is_sanitized_url_the_same("about blabla:"sv));
    EXPECT(!is_sanitized_url_the_same("blabla about:"sv));

    EXPECT(is_sanitized_url_the_same("about:about"sv));
    EXPECT(is_sanitized_url_the_same("about:version"sv));
}

TEST_CASE(data_url)
{
    EXPECT(is_sanitized_url_the_same("data:text/html"sv));

    EXPECT(!is_sanitized_url_the_same("data text/html"sv));
    EXPECT(!is_sanitized_url_the_same("text/html data:"sv));
}
