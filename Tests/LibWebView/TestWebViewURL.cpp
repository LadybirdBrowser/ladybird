/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/URL.h>

static WebView::SearchEngine s_test_engine {
    .name = "Test"_string,
    .query_url = "https://ecosia.org/search?q=%s"_string
};

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
    auto sanitized_url = WebView::sanitize_url(url, s_test_engine, append_tld);

    EXPECT(sanitized_url.has_value());
    EXPECT_EQ(sanitized_url->to_string(), test_url);
}

static void expect_search_url_equals_sanitized_url(StringView url)
{
    auto search_url = s_test_engine.format_search_query_for_navigation(url);
    auto sanitized_url = WebView::sanitize_url(url, s_test_engine);

    EXPECT(sanitized_url.has_value());
    EXPECT_EQ(sanitized_url->to_string(), search_url);
}

TEST_CASE(invalid_url)
{
    EXPECT(!WebView::break_url_into_parts(""_sv).has_value());
    EXPECT(!WebView::break_url_into_parts(":"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts(":/"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("://"_sv).has_value());

    EXPECT(!WebView::break_url_into_parts("/"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("//"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("/h"_sv).has_value());

    EXPECT(!WebView::break_url_into_parts("f"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("fi"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("fil"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("file"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("file:"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("file:/"_sv).has_value());

    EXPECT(!WebView::break_url_into_parts("h"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("ht"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("htt"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("http"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("http:"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("http:/"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("http://"_sv).has_value());

    EXPECT(!WebView::break_url_into_parts("https"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("https:"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("https:/"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("https://"_sv).has_value());

    EXPECT(!WebView::break_url_into_parts("a"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("ab"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("abo"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("abou"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("about"_sv).has_value());

    EXPECT(!WebView::break_url_into_parts("d"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("da"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("dat"_sv).has_value());
    EXPECT(!WebView::break_url_into_parts("data"_sv).has_value());
}

TEST_CASE(file_url)
{
    compare_url_parts("file://"_sv, { "file://"_sv, ""_sv, {} });
    compare_url_parts("file://a"_sv, { "file://"_sv, "a"_sv, {} });
    compare_url_parts("file:///a"_sv, { "file://"_sv, "/a"_sv, {} });
    compare_url_parts("file:///abc"_sv, { "file://"_sv, "/abc"_sv, {} });
}

TEST_CASE(http_url)
{
    compare_url_parts("http://a"_sv, { "http://"_sv, "a"_sv, {} });
    compare_url_parts("http://abc"_sv, { "http://"_sv, "abc"_sv, {} });
    compare_url_parts("http://com"_sv, { "http://"_sv, "com"_sv, {} });
    compare_url_parts("http://abc."_sv, { "http://"_sv, "abc."_sv, {} });
    compare_url_parts("http://abc.c"_sv, { "http://"_sv, "abc.c"_sv, {} });
    compare_url_parts("http://abc.com"_sv, { "http://"_sv, "abc.com"_sv, {} });
    compare_url_parts("http://abc.com."_sv, { "http://"_sv, "abc.com."_sv, {} });
    compare_url_parts("http://abc.com."_sv, { "http://"_sv, "abc.com."_sv, {} });
    compare_url_parts("http://abc.com.org"_sv, { "http://abc."_sv, "com.org"_sv, {} });
    compare_url_parts("http://abc.com.org.gov"_sv, { "http://abc.com."_sv, "org.gov"_sv, {} });

    compare_url_parts("http://abc/path"_sv, { "http://"_sv, "abc"_sv, "/path"_sv });
    compare_url_parts("http://abc#anchor"_sv, { "http://"_sv, "abc"_sv, "#anchor"_sv });
    compare_url_parts("http://abc?query"_sv, { "http://"_sv, "abc"_sv, "?query"_sv });

    compare_url_parts("http://abc.def.com"_sv, { "http://abc."_sv, "def.com"_sv, {} });
    compare_url_parts("http://abc.def.com/path"_sv, { "http://abc."_sv, "def.com"_sv, "/path"_sv });
    compare_url_parts("http://abc.def.com#anchor"_sv, { "http://abc."_sv, "def.com"_sv, "#anchor"_sv });
    compare_url_parts("http://abc.def.com?query"_sv, { "http://abc."_sv, "def.com"_sv, "?query"_sv });
}

TEST_CASE(about_url)
{
    compare_url_parts("about:"_sv, { "about:"_sv, {}, {} });
    compare_url_parts("about:a"_sv, { "about:"_sv, "a"_sv, {} });
    compare_url_parts("about:ab"_sv, { "about:"_sv, "ab"_sv, {} });
    compare_url_parts("about:abc"_sv, { "about:"_sv, "abc"_sv, {} });
    compare_url_parts("about:abc/def"_sv, { "about:"_sv, "abc/def"_sv, {} });

    EXPECT(!is_sanitized_url_the_same("about"_sv));
    EXPECT(!is_sanitized_url_the_same("about blabla:"_sv));
    EXPECT(!is_sanitized_url_the_same("blabla about:"_sv));

    EXPECT(is_sanitized_url_the_same("about:about"_sv));
    EXPECT(is_sanitized_url_the_same("about:version"_sv));
}

TEST_CASE(data_url)
{
    compare_url_parts("data:"_sv, { "data:"_sv, {}, {} });
    compare_url_parts("data:a"_sv, { "data:"_sv, "a"_sv, {} });
    compare_url_parts("data:ab"_sv, { "data:"_sv, "ab"_sv, {} });
    compare_url_parts("data:abc"_sv, { "data:"_sv, "abc"_sv, {} });
    compare_url_parts("data:abc/def"_sv, { "data:"_sv, "abc/def"_sv, {} });

    EXPECT(is_sanitized_url_the_same("data:text/html"_sv));

    EXPECT(!is_sanitized_url_the_same("data text/html"_sv));
    EXPECT(!is_sanitized_url_the_same("text/html data:"_sv));
}

TEST_CASE(location_to_search_or_url)
{
    expect_search_url_equals_sanitized_url("hello"_sv); // Search.
    expect_search_url_equals_sanitized_url("hello world"_sv);
    expect_search_url_equals_sanitized_url("\"example.org\""_sv);
    expect_search_url_equals_sanitized_url("\"example.org"_sv);
    expect_search_url_equals_sanitized_url("\"http://example.org\""_sv);
    expect_search_url_equals_sanitized_url("example.org hello"_sv);
    expect_search_url_equals_sanitized_url("http://example.org and example sites"_sv);
    expect_search_url_equals_sanitized_url("ftp://example.org"_sv); // ftp:// is not in SUPPORTED_SCHEMES
    expect_search_url_equals_sanitized_url("https://exa\"mple.com/what"_sv);

    // If it can feed create_with_url_or_path -- it is a url.
    expect_url_equals_sanitized_url("https://example.com/%20some%20cool%20page"_sv, "https://example.com/ some cool page"_sv);
    expect_url_equals_sanitized_url("https://example.com/some%20cool%20page"_sv, "https://example.com/some cool page"_sv);
    expect_url_equals_sanitized_url("https://example.com/%22what%22"_sv, "https://example.com/\"what\""_sv);

    expect_url_equals_sanitized_url("https://example.org/"_sv, "example.org"_sv);            // Valid domain.
    expect_url_equals_sanitized_url("https://example.abc/"_sv, "example.abc"_sv);            // .abc is a recognized TLD.
    expect_url_equals_sanitized_url("https://example.test/path"_sv, "example.test/path"_sv); // Reserved TLDs.
    expect_url_equals_sanitized_url("https://example.example/path"_sv, "example.example/path"_sv);
    expect_url_equals_sanitized_url("https://example.invalid/path"_sv, "example.invalid/path"_sv);
    expect_url_equals_sanitized_url("https://example.localhost/path"_sv, "example.localhost/path"_sv);

    expect_search_url_equals_sanitized_url("example.def"_sv); // Invalid domain but no scheme: search (Like Firefox or Chrome).

    expect_url_equals_sanitized_url("https://example.org/"_sv, "https://example.org"_sv); // Scheme.
    // Respect the user if the url has a valid scheme but not a public suffix (.def is not a recognized TLD).
    expect_url_equals_sanitized_url("https://example.def/"_sv, "https://example.def"_sv);

    expect_url_equals_sanitized_url("https://localhost/"_sv, "localhost"_sv); // Respect localhost.
    expect_url_equals_sanitized_url("https://localhost/hello"_sv, "localhost/hello"_sv);
    expect_url_equals_sanitized_url("https://localhost/hello.world"_sv, "localhost/hello.world"_sv);
    expect_url_equals_sanitized_url("https://localhost/hello.world?query=123"_sv, "localhost/hello.world?query=123"_sv);

    expect_url_equals_sanitized_url("https://example.com/"_sv, "example"_sv, WebView::AppendTLD::Yes); // User holds down the Ctrl key.
    expect_url_equals_sanitized_url("https://example.def.com/"_sv, "example.def"_sv, WebView::AppendTLD::Yes);
    expect_url_equals_sanitized_url("https://com.com/"_sv, "com"_sv, WebView::AppendTLD::Yes);
    expect_url_equals_sanitized_url("https://example.com/index.html"_sv, "example/index.html"_sv, WebView::AppendTLD::Yes);

    expect_search_url_equals_sanitized_url("whatever:example.com"_sv);     // Invalid scheme.
    expect_search_url_equals_sanitized_url("mailto:hello@example.com"_sv); // For now, unsupported scheme.
    // FIXME: Add support for opening mailto: scheme (below). Firefox opens mailto: locations
    // expect_url_equals_sanitized_url("mailto:hello@example.com"_sv, "mailto:hello@example.com"_sv);
}
