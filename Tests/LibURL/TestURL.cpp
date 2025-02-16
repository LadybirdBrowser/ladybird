/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibURL/Parser.h>
#include <LibURL/URL.h>

TEST_CASE(construct)
{
    EXPECT_EQ(URL::URL().is_valid(), false);
}

TEST_CASE(basic)
{
    {
        auto url = URL::Parser::basic_parse("http://www.serenityos.org"sv);
        EXPECT_EQ(url.has_value(), true);
        EXPECT_EQ(url->scheme(), "http");
        EXPECT_EQ(url->serialized_host(), "www.serenityos.org");
        EXPECT_EQ(url->port_or_default(), 80);
        EXPECT_EQ(url->serialize_path(), "/");
        EXPECT(!url->query().has_value());
        EXPECT(!url->fragment().has_value());
    }
    {
        auto url = URL::Parser::basic_parse("https://www.serenityos.org/index.html"sv);
        EXPECT_EQ(url.has_value(), true);
        EXPECT_EQ(url->scheme(), "https");
        EXPECT_EQ(url->serialized_host(), "www.serenityos.org");
        EXPECT_EQ(url->port_or_default(), 443);
        EXPECT_EQ(url->serialize_path(), "/index.html");
        EXPECT(!url->query().has_value());
        EXPECT(!url->fragment().has_value());
    }
    {
        auto url = URL::Parser::basic_parse("https://www.serenityos.org1/index.html"sv);
        EXPECT_EQ(url.has_value(), true);
        EXPECT_EQ(url->scheme(), "https");
        EXPECT_EQ(url->serialized_host(), "www.serenityos.org1");
        EXPECT_EQ(url->port_or_default(), 443);
        EXPECT_EQ(url->serialize_path(), "/index.html");
        EXPECT(!url->query().has_value());
        EXPECT(!url->fragment().has_value());
    }
    {
        auto url = URL::Parser::basic_parse("https://localhost:1234/~anon/test/page.html"sv);
        EXPECT_EQ(url.has_value(), true);
        EXPECT_EQ(url->scheme(), "https");
        EXPECT_EQ(url->serialized_host(), "localhost");
        EXPECT_EQ(url->port_or_default(), 1234);
        EXPECT_EQ(url->serialize_path(), "/~anon/test/page.html");
        EXPECT(!url->query().has_value());
        EXPECT(!url->fragment().has_value());
    }
    {
        auto url = URL::Parser::basic_parse("http://www.serenityos.org/index.html?#"sv);
        EXPECT_EQ(url.has_value(), true);
        EXPECT_EQ(url->scheme(), "http");
        EXPECT_EQ(url->serialized_host(), "www.serenityos.org");
        EXPECT_EQ(url->port_or_default(), 80);
        EXPECT_EQ(url->serialize_path(), "/index.html");
        EXPECT_EQ(url->query(), "");
        EXPECT_EQ(url->fragment(), "");
    }
    {
        auto url = URL::Parser::basic_parse("http://www.serenityos.org/index.html?foo=1&bar=2"sv);
        EXPECT_EQ(url.has_value(), true);
        EXPECT_EQ(url->scheme(), "http");
        EXPECT_EQ(url->serialized_host(), "www.serenityos.org");
        EXPECT_EQ(url->port_or_default(), 80);
        EXPECT_EQ(url->serialize_path(), "/index.html");
        EXPECT_EQ(url->query(), "foo=1&bar=2");
        EXPECT(!url->fragment().has_value());
    }
    {
        auto url = URL::Parser::basic_parse("http://www.serenityos.org/index.html#fragment"sv);
        EXPECT_EQ(url.has_value(), true);
        EXPECT_EQ(url->scheme(), "http");
        EXPECT_EQ(url->serialized_host(), "www.serenityos.org");
        EXPECT_EQ(url->port_or_default(), 80);
        EXPECT_EQ(url->serialize_path(), "/index.html");
        EXPECT(!url->query().has_value());
        EXPECT_EQ(url->fragment(), "fragment");
    }
    {
        auto url = URL::Parser::basic_parse("http://www.serenityos.org/index.html?foo=1&bar=2&baz=/?#frag/ment?test#"sv);
        EXPECT_EQ(url.has_value(), true);
        EXPECT_EQ(url->scheme(), "http");
        EXPECT_EQ(url->serialized_host(), "www.serenityos.org");
        EXPECT_EQ(url->port_or_default(), 80);
        EXPECT_EQ(url->serialize_path(), "/index.html");
        EXPECT_EQ(url->query(), "foo=1&bar=2&baz=/?");
        EXPECT_EQ(url->fragment(), "frag/ment?test#");
    }
}

TEST_CASE(some_bad_urls)
{
    EXPECT_EQ(URL::Parser::basic_parse("http//serenityos.org"sv).has_value(), false);
    EXPECT_EQ(URL::Parser::basic_parse("serenityos.org"sv).has_value(), false);
    EXPECT_EQ(URL::Parser::basic_parse("://serenityos.org"sv).has_value(), false);
    EXPECT_EQ(URL::Parser::basic_parse("://:80"sv).has_value(), false);
    EXPECT_EQ(URL::Parser::basic_parse("http://serenityos.org:80:80/"sv).has_value(), false);
    EXPECT_EQ(URL::Parser::basic_parse("http://serenityos.org:80:80"sv).has_value(), false);
    EXPECT_EQ(URL::Parser::basic_parse("http://serenityos.org:abc"sv).has_value(), false);
    EXPECT_EQ(URL::Parser::basic_parse("http://serenityos.org:abc:80"sv).has_value(), false);
    EXPECT_EQ(URL::Parser::basic_parse("http://serenityos.org:abc:80/"sv).has_value(), false);
}

TEST_CASE(serialization)
{
    EXPECT_EQ(URL::Parser::basic_parse("http://www.serenityos.org/"sv)->serialize(), "http://www.serenityos.org/");
    EXPECT_EQ(URL::Parser::basic_parse("http://www.serenityos.org:0/"sv)->serialize(), "http://www.serenityos.org:0/");
    EXPECT_EQ(URL::Parser::basic_parse("http://www.serenityos.org:80/"sv)->serialize(), "http://www.serenityos.org/");
    EXPECT_EQ(URL::Parser::basic_parse("http://www.serenityos.org:81/"sv)->serialize(), "http://www.serenityos.org:81/");
    EXPECT_EQ(URL::Parser::basic_parse("https://www.serenityos.org:443/foo/bar.html?query#fragment"sv)->serialize(), "https://www.serenityos.org/foo/bar.html?query#fragment");
}

TEST_CASE(file_url_with_hostname)
{
    auto url = URL::Parser::basic_parse("file://courage/my/file"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "file");
    EXPECT_EQ(url->serialized_host(), "courage");
    EXPECT_EQ(url->port_or_default(), 0);
    EXPECT_EQ(url->serialize_path(), "/my/file");
    EXPECT_EQ(url->serialize(), "file://courage/my/file");
    EXPECT(!url->query().has_value());
    EXPECT(!url->fragment().has_value());
}

TEST_CASE(file_url_with_localhost)
{
    auto url = URL::Parser::basic_parse("file://localhost/my/file"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "file");
    EXPECT_EQ(url->serialized_host(), "");
    EXPECT_EQ(url->serialize_path(), "/my/file");
    EXPECT_EQ(url->serialize(), "file:///my/file");
}

TEST_CASE(file_url_without_hostname)
{
    auto url = URL::Parser::basic_parse("file:///my/file"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "file");
    EXPECT_EQ(url->serialized_host(), "");
    EXPECT_EQ(url->serialize_path(), "/my/file");
    EXPECT_EQ(url->serialize(), "file:///my/file");
}

TEST_CASE(file_url_with_encoded_characters)
{
    auto url = URL::Parser::basic_parse("file:///my/file/test%23file.txt"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "file");
    EXPECT_EQ(url->serialize_path(), "/my/file/test%23file.txt");
    EXPECT_EQ(URL::percent_decode(url->serialize_path()), "/my/file/test#file.txt");
    EXPECT(!url->query().has_value());
    EXPECT(!url->fragment().has_value());
}

TEST_CASE(file_url_with_fragment)
{
    auto url = URL::Parser::basic_parse("file:///my/file#fragment"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "file");
    EXPECT_EQ(url->serialize_path(), "/my/file");
    EXPECT(!url->query().has_value());
    EXPECT_EQ(url->fragment(), "fragment");
}

TEST_CASE(file_url_with_root_path)
{
    auto url = URL::Parser::basic_parse("file:///"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "file");
    EXPECT_EQ(url->serialize_path(), "/");
}

TEST_CASE(file_url_serialization)
{
    EXPECT_EQ(URL::Parser::basic_parse("file://courage/my/file"sv)->serialize(), "file://courage/my/file");
    EXPECT_EQ(URL::Parser::basic_parse("file://localhost/my/file"sv)->serialize(), "file:///my/file");
    EXPECT_EQ(URL::Parser::basic_parse("file:///my/file"sv)->serialize(), "file:///my/file");
    EXPECT_EQ(URL::Parser::basic_parse("file:///my/directory/"sv)->serialize(), "file:///my/directory/");
    EXPECT_EQ(URL::Parser::basic_parse("file:///my/file%23test"sv)->serialize(), "file:///my/file%23test");
    EXPECT_EQ(URL::Parser::basic_parse("file:///my/file#fragment"sv)->serialize(), "file:///my/file#fragment");
}

TEST_CASE(file_url_relative)
{
    EXPECT_EQ(URL::Parser::basic_parse("https://vkoskiv.com/index.html"sv)->complete_url("/static/foo.js"sv)->serialize(), "https://vkoskiv.com/static/foo.js");
    EXPECT_EQ(URL::Parser::basic_parse("file:///home/vkoskiv/test/index.html"sv)->complete_url("/static/foo.js"sv)->serialize(), "file:///static/foo.js");
    EXPECT_EQ(URL::Parser::basic_parse("https://vkoskiv.com/index.html"sv)->complete_url("static/foo.js"sv)->serialize(), "https://vkoskiv.com/static/foo.js");
    EXPECT_EQ(URL::Parser::basic_parse("file:///home/vkoskiv/test/index.html"sv)->complete_url("static/foo.js"sv)->serialize(), "file:///home/vkoskiv/test/static/foo.js");
}

TEST_CASE(about_url)
{
    auto url = URL::Parser::basic_parse("about:blank"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "about");
    EXPECT(!url->host().has_value());
    EXPECT_EQ(url->serialize_path(), "blank");
    EXPECT(!url->query().has_value());
    EXPECT(!url->fragment().has_value());
    EXPECT_EQ(url->serialize(), "about:blank");
}

TEST_CASE(mailto_url)
{
    auto url = URL::Parser::basic_parse("mailto:mail@example.com"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "mailto");
    EXPECT(!url->host().has_value());
    EXPECT_EQ(url->port_or_default(), 0);
    EXPECT_EQ(url->path_segment_count(), 1u);
    EXPECT_EQ(url->path_segment_at_index(0), "mail@example.com");
    EXPECT(!url->query().has_value());
    EXPECT(!url->fragment().has_value());
    EXPECT_EQ(url->serialize(), "mailto:mail@example.com");
}

TEST_CASE(mailto_url_with_subject)
{
    auto url = URL::Parser::basic_parse("mailto:mail@example.com?subject=test"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "mailto");
    EXPECT(!url->host().has_value());
    EXPECT_EQ(url->port_or_default(), 0);
    EXPECT_EQ(url->path_segment_count(), 1u);
    EXPECT_EQ(url->path_segment_at_index(0), "mail@example.com");
    EXPECT_EQ(url->query(), "subject=test");
    EXPECT(!url->fragment().has_value());
    EXPECT_EQ(url->serialize(), "mailto:mail@example.com?subject=test");
}

TEST_CASE(trailing_slash_with_complete_url)
{
    EXPECT_EQ(URL::Parser::basic_parse("http://a/b/"sv)->complete_url("c/"sv)->serialize(), "http://a/b/c/");
    EXPECT_EQ(URL::Parser::basic_parse("http://a/b/"sv)->complete_url("c"sv)->serialize(), "http://a/b/c");
    EXPECT_EQ(URL::Parser::basic_parse("http://a/b"sv)->complete_url("c/"sv)->serialize(), "http://a/c/");
    EXPECT_EQ(URL::Parser::basic_parse("http://a/b"sv)->complete_url("c"sv)->serialize(), "http://a/c");
}

TEST_CASE(trailing_port)
{
    auto url = URL::Parser::basic_parse("http://example.com:8086"sv);
    EXPECT_EQ(url->port_or_default(), 8086);
}

TEST_CASE(port_overflow)
{
    EXPECT_EQ(URL::Parser::basic_parse("http://example.com:123456789/"sv).has_value(), false);
}

TEST_CASE(equality)
{
    EXPECT(URL::Parser::basic_parse("http://serenityos.org"sv)->equals(URL::Parser::basic_parse("http://serenityos.org#test"sv).value(), URL::ExcludeFragment::Yes));
    EXPECT_EQ(URL::Parser::basic_parse("http://example.com/index.html"sv), URL::Parser::basic_parse("http://ex%61mple.com/index.html"sv));
    EXPECT_EQ(URL::Parser::basic_parse("file:///my/file"sv), URL::Parser::basic_parse("file://localhost/my/file"sv));
    EXPECT_NE(URL::Parser::basic_parse("http://serenityos.org/index.html"sv), URL::Parser::basic_parse("http://serenityos.org/test.html"sv));
}

TEST_CASE(create_with_file_scheme)
{
    auto url = URL::create_with_file_scheme("/home/anon/README.md");
    EXPECT(url.is_valid());
    EXPECT_EQ(url.scheme(), "file");
    EXPECT_EQ(url.port_or_default(), 0);
    EXPECT_EQ(url.path_segment_count(), 3u);
    EXPECT_EQ(url.path_segment_at_index(0), "home");
    EXPECT_EQ(url.path_segment_at_index(1), "anon");
    EXPECT_EQ(url.path_segment_at_index(2), "README.md");
    EXPECT_EQ(url.serialize_path(), "/home/anon/README.md");
    EXPECT(!url.query().has_value());
    EXPECT(!url.fragment().has_value());

    url = URL::create_with_file_scheme("/home/anon/");
    EXPECT(url.is_valid());
    EXPECT_EQ(url.path_segment_count(), 3u);
    EXPECT_EQ(url.path_segment_at_index(0), "home");
    EXPECT_EQ(url.path_segment_at_index(1), "anon");
    EXPECT_EQ(url.path_segment_at_index(2), "");
    EXPECT_EQ(url.serialize_path(), "/home/anon/");

    url = URL::Parser::basic_parse("file:///home/anon/"sv).value();
    EXPECT_EQ(url.serialize_path(), "/home/anon/");
}

TEST_CASE(complete_url)
{
    auto base_url = URL::Parser::basic_parse("http://serenityos.org/index.html#fragment"sv);
    auto url = base_url->complete_url("test.html"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "http");
    EXPECT_EQ(url->serialized_host(), "serenityos.org");
    EXPECT_EQ(url->serialize_path(), "/test.html");
    EXPECT(!url->query().has_value());
    EXPECT_EQ(url->cannot_be_a_base_url(), false);

    EXPECT(base_url->complete_url("../index.html#fragment"sv)->equals(*base_url));
}

TEST_CASE(leading_whitespace)
{
    auto url = URL::Parser::basic_parse("   https://foo.com/"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->to_byte_string(), "https://foo.com/");
}

TEST_CASE(trailing_whitespace)
{
    auto url = URL::Parser::basic_parse("https://foo.com/   "sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->to_byte_string(), "https://foo.com/");
}

TEST_CASE(leading_and_trailing_whitespace)
{
    auto url = URL::Parser::basic_parse("      https://foo.com/   "sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->to_byte_string(), "https://foo.com/");
}

TEST_CASE(unicode)
{
    auto url = URL::Parser::basic_parse("http://example.com/_ünicöde_téxt_©"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->serialize_path(), "/_%C3%BCnic%C3%B6de_t%C3%A9xt_%C2%A9");
    EXPECT_EQ(URL::percent_decode(url->serialize_path()), "/_ünicöde_téxt_©");
    EXPECT(!url->query().has_value());
    EXPECT(!url->fragment().has_value());
}

TEST_CASE(query_with_non_ascii)
{
    {
        Optional<URL::URL> url = URL::Parser::basic_parse("http://example.com/?utf8=✓"sv);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialize_path(), "/"sv);
        EXPECT_EQ(url->query(), "utf8=%E2%9C%93");
        EXPECT(!url->fragment().has_value());
    }
    {
        Optional<URL::URL> url = URL::Parser::basic_parse("http://example.com/?shift_jis=✓"sv, {}, nullptr, {}, "shift_jis"sv);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialize_path(), "/"sv);
        EXPECT_EQ(url->query(), "shift_jis=%26%2310003%3B");
        EXPECT(!url->fragment().has_value());
    }
}

TEST_CASE(fragment_with_non_ascii)
{
    {
        Optional<URL::URL> url = URL::Parser::basic_parse("http://example.com/#✓"sv);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialize_path(), "/"sv);
        EXPECT(!url->query().has_value());
        EXPECT_EQ(url->fragment(), "%E2%9C%93");
    }
    {
        Optional<URL::URL> url = URL::Parser::basic_parse("http://example.com/#✓"sv, {}, nullptr, {}, "shift_jis"sv);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialize_path(), "/"sv);
        EXPECT(!url->query().has_value());
        EXPECT_EQ(url->fragment(), "%E2%9C%93");
    }
}

TEST_CASE(complete_file_url_with_base)
{
    auto url = URL::Parser::basic_parse("file:///home/index.html"sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->serialize_path(), "/home/index.html");
    EXPECT_EQ(url->path_segment_count(), 2u);
    EXPECT_EQ(url->path_segment_at_index(0), "home");
    EXPECT_EQ(url->path_segment_at_index(1), "index.html");

    auto sub_url = url->complete_url("js/app.js"sv);
    EXPECT(sub_url.has_value());
    EXPECT_EQ(sub_url->serialize_path(), "/home/js/app.js");
}

TEST_CASE(empty_url_with_base_url)
{
    auto base_url = URL::Parser::basic_parse("https://foo.com/"sv);
    Optional<URL::URL> parsed_url = URL::Parser::basic_parse(""sv, base_url);
    EXPECT_EQ(parsed_url.has_value(), true);
    EXPECT(base_url->equals(*parsed_url));
}

TEST_CASE(google_street_view)
{
    constexpr auto streetview_url = "https://www.google.co.uk/maps/@53.3354159,-1.9573545,3a,75y,121.1h,75.67t/data=!3m7!1e1!3m5!1sSY8xCv17jAX4S7SRdV38hg!2e0!6shttps:%2F%2Fstreetviewpixels-pa.googleapis.com%2Fv1%2Fthumbnail%3Fpanoid%3DSY8xCv17jAX4S7SRdV38hg%26cb_client%3Dmaps_sv.tactile.gps%26w%3D203%26h%3D100%26yaw%3D188.13148%26pitch%3D0%26thumbfov%3D100!7i13312!8i6656"sv;
    auto url = URL::Parser::basic_parse(streetview_url);
    EXPECT_EQ(url->serialize(), streetview_url);
}

TEST_CASE(ipv6_address)
{
    {
        constexpr auto ipv6_url = "http://[::1]/index.html"sv;
        auto url = URL::Parser::basic_parse(ipv6_url);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialized_host(), "[::1]"sv);
        EXPECT_EQ(url->to_string(), ipv6_url);
    }

    {
        constexpr auto ipv6_url = "http://[0:f:0:0:f:f:0:0]/index.html"sv;
        auto url = URL::Parser::basic_parse(ipv6_url);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialized_host(), "[0:f::f:f:0:0]"sv);
        EXPECT_EQ(url->to_string(), "http://[0:f::f:f:0:0]/index.html"sv);
    }

    {
        constexpr auto ipv6_url = "https://[2001:0db8:85a3:0000:0000:8a2e:0370:7334]/index.html"sv;
        auto url = URL::Parser::basic_parse(ipv6_url);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialized_host(), "[2001:db8:85a3::8a2e:370:7334]"sv);
        EXPECT_EQ(url->to_string(), "https://[2001:db8:85a3::8a2e:370:7334]/index.html"sv);
    }

    {
        constexpr auto bad_ipv6_url = "https://[oops]/index.html"sv;
        auto url = URL::Parser::basic_parse(bad_ipv6_url);
        EXPECT_EQ(url.has_value(), false);
    }
}

TEST_CASE(ipv4_address)
{
    {
        constexpr auto ipv4_url = "http://127.0.0.1/index.html"sv;
        auto url = URL::Parser::basic_parse(ipv4_url);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialized_host(), "127.0.0.1"sv);
    }

    {
        constexpr auto ipv4_url = "http://0x.0x.0"sv;
        auto url = URL::Parser::basic_parse(ipv4_url);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialized_host(), "0.0.0.0"sv);
    }

    {
        constexpr auto bad_ipv4_url = "https://127..0.0.1"sv;
        auto url = URL::Parser::basic_parse(bad_ipv4_url);
        EXPECT(!url.has_value());
    }

    {
        constexpr auto ipv4_url = "http://256"sv;
        auto url = URL::Parser::basic_parse(ipv4_url);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialized_host(), "0.0.1.0"sv);
    }

    {
        constexpr auto ipv4_url = "http://888888888"sv;
        auto url = URL::Parser::basic_parse(ipv4_url);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialized_host(), "52.251.94.56"sv);
    }

    {
        constexpr auto ipv4_url = "http://9111111111"sv;
        auto url = URL::Parser::basic_parse(ipv4_url);
        EXPECT(!url.has_value());
    }
}

TEST_CASE(username_and_password)
{
    {
        constexpr auto url_with_username_and_password = "http://username:password@test.com/index.html"sv;
        auto url = URL::Parser::basic_parse(url_with_username_and_password);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialized_host(), "test.com"sv);
        EXPECT_EQ(url->username(), "username"sv);
        EXPECT_EQ(url->password(), "password"sv);
    }

    {
        constexpr auto url_with_percent_encoded_credentials = "http://username%21%24%25:password%21%24%25@test.com/index.html"sv;
        auto url = URL::Parser::basic_parse(url_with_percent_encoded_credentials);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialized_host(), "test.com"sv);
        EXPECT_EQ(url->username(), "username%21%24%25");
        EXPECT_EQ(url->password(), "password%21%24%25");
        EXPECT_EQ(URL::percent_decode(url->username()), "username!$%"sv);
        EXPECT_EQ(URL::percent_decode(url->password()), "password!$%"sv);
    }

    {
        auto const& username = MUST(String::repeated('a', 50000));
        auto const& url_with_long_username = MUST(String::formatted("http://{}:@test.com/index.html", username));
        auto url = URL::Parser::basic_parse(url_with_long_username);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialized_host(), "test.com"sv);
        EXPECT_EQ(url->username(), username);
        EXPECT(url->password().is_empty());
    }

    {
        auto const& password = MUST(String::repeated('a', 50000));
        auto const& url_with_long_password = MUST(String::formatted("http://:{}@test.com/index.html", password));
        auto url = URL::Parser::basic_parse(url_with_long_password);
        EXPECT(url.has_value());
        EXPECT_EQ(url->serialized_host(), "test.com"sv);
        EXPECT(url->username().is_empty());
        EXPECT_EQ(url->password(), password);
    }
}

TEST_CASE(ascii_only_url)
{
    {
        constexpr auto upper_case_url = "HTTP://EXAMPLE.COM:80/INDEX.HTML#FRAGMENT"sv;
        auto url = URL::Parser::basic_parse(upper_case_url);
        EXPECT(url.has_value());
        EXPECT_EQ(url->scheme(), "http");
        EXPECT_EQ(url->serialized_host(), "example.com"sv);
        EXPECT_EQ(url->to_byte_string(), "http://example.com/INDEX.HTML#FRAGMENT");
    }

    {
        constexpr auto mixed_case_url = "hTtP://eXaMpLe.CoM:80/iNdEx.HtMl#fRaGmEnT"sv;
        auto url = URL::Parser::basic_parse(mixed_case_url);
        EXPECT(url.has_value());
        EXPECT_EQ(url->scheme(), "http");
        EXPECT_EQ(url->serialized_host(), "example.com"sv);
        EXPECT_EQ(url->to_byte_string(), "http://example.com/iNdEx.HtMl#fRaGmEnT");
    }
}

TEST_CASE(invalid_domain_code_points)
{
    {
        constexpr auto upper_case_url = "http://example%25.com"sv;
        auto url = URL::Parser::basic_parse(upper_case_url);
        EXPECT(!url.has_value());
    }

    {
        constexpr auto mixed_case_url = "http://thing\u0007y/'"sv;
        auto url = URL::Parser::basic_parse(mixed_case_url);
        EXPECT(!url.has_value());
    }
}
