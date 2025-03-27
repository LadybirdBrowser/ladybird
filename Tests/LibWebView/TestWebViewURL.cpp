/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
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
    return sanitized_url->to_string() == url;
}

static void expect_url_equals_sanitized_url(StringView test_url, StringView url, WebView::AppendTLD append_tld = WebView::AppendTLD::No)
{
    StringView const search_engine_url = "https://ecosia.org/search?q={}"sv;

    auto sanitized_url = WebView::sanitize_url(url, search_engine_url, append_tld);

    EXPECT(sanitized_url.has_value());
    EXPECT_EQ(sanitized_url->to_string(), test_url);
}

static void expect_search_url_equals_sanitized_url(StringView url)
{
    StringView const search_engine_url = "https://ecosia.org/search?q={}"sv;
    auto const search_url = String::formatted(search_engine_url, URL::percent_encode(url));

    auto sanitized_url = WebView::sanitize_url(url, search_engine_url);

    EXPECT(sanitized_url.has_value());
    EXPECT_EQ(sanitized_url->to_string(), search_url.value());
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

TEST_CASE(location_to_search_or_url)
{
    expect_search_url_equals_sanitized_url("hello"sv); // Search.
    expect_search_url_equals_sanitized_url("hello world"sv);
    expect_search_url_equals_sanitized_url("\"example.org\""sv);
    expect_search_url_equals_sanitized_url("\"example.org"sv);
    expect_search_url_equals_sanitized_url("\"http://example.org\""sv);
    expect_search_url_equals_sanitized_url("example.org hello"sv);
    expect_search_url_equals_sanitized_url("http://example.org and example sites"sv);
    expect_search_url_equals_sanitized_url("ftp://example.org"sv); // ftp:// is not in SUPPORTED_SCHEMES
    expect_search_url_equals_sanitized_url("https://exa\"mple.com/what"sv);

    // If it can feed create_with_url_or_path -- it is a url.
    expect_url_equals_sanitized_url("https://example.com/%20some%20cool%20page"sv, "https://example.com/ some cool page"sv);
    expect_url_equals_sanitized_url("https://example.com/some%20cool%20page"sv, "https://example.com/some cool page"sv);
    expect_url_equals_sanitized_url("https://example.com/%22what%22"sv, "https://example.com/\"what\""sv);

    expect_url_equals_sanitized_url("https://example.org/"sv, "example.org"sv);            // Valid domain.
    expect_url_equals_sanitized_url("https://example.abc/"sv, "example.abc"sv);            // .abc is a recognized TLD.
    expect_url_equals_sanitized_url("https://example.test/path"sv, "example.test/path"sv); // Reserved TLDs.
    expect_url_equals_sanitized_url("https://example.example/path"sv, "example.example/path"sv);
    expect_url_equals_sanitized_url("https://example.invalid/path"sv, "example.invalid/path"sv);
    expect_url_equals_sanitized_url("https://example.localhost/path"sv, "example.localhost/path"sv);

    expect_search_url_equals_sanitized_url("example.def"sv); // Invalid domain but no scheme: search (Like Firefox or Chrome).

    expect_url_equals_sanitized_url("https://example.org/"sv, "https://example.org"sv); // Scheme.
    // Respect the user if the url has a valid scheme but not a public suffix (.def is not a recognized TLD).
    expect_url_equals_sanitized_url("https://example.def/"sv, "https://example.def"sv);

    expect_url_equals_sanitized_url("https://localhost/"sv, "localhost"sv); // Respect localhost.
    expect_url_equals_sanitized_url("https://localhost/hello"sv, "localhost/hello"sv);
    expect_url_equals_sanitized_url("https://localhost/hello.world"sv, "localhost/hello.world"sv);
    expect_url_equals_sanitized_url("https://localhost/hello.world?query=123"sv, "localhost/hello.world?query=123"sv);

    expect_url_equals_sanitized_url("https://example.com/"sv, "example"sv, WebView::AppendTLD::Yes); // User holds down the Ctrl key.
    expect_url_equals_sanitized_url("https://example.def.com/"sv, "example.def"sv, WebView::AppendTLD::Yes);
    expect_url_equals_sanitized_url("https://com.com/"sv, "com"sv, WebView::AppendTLD::Yes);
    expect_url_equals_sanitized_url("https://example.com/index.html"sv, "example/index.html"sv, WebView::AppendTLD::Yes);

    expect_search_url_equals_sanitized_url("whatever:example.com"sv);     // Invalid scheme.
    expect_search_url_equals_sanitized_url("mailto:hello@example.com"sv); // For now, unsupported scheme.
    // FIXME: Add support for opening mailto: scheme (below). Firefox opens mailto: locations
    // expect_url_equals_sanitized_url("mailto:hello@example.com"sv, "mailto:hello@example.com"sv);
}
