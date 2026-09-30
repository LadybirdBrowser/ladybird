/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/HashMap.h>
#include <LibHTTP/Proxy.h>
#include <LibURL/Parser.h>

static HTTP::ProxyConfiguration configuration_from(HashMap<StringView, StringView> const& environment)
{
    return HTTP::ProxyConfiguration::from_environment([&](StringView name) { return environment.get(name); });
}

static URL::URL url(StringView string)
{
    return URL::Parser::basic_parse(string).release_value();
}

TEST_CASE(parse_proxy)
{
    auto proxy = HTTP::Proxy::parse("proxy.example:3128"sv);
    EXPECT(proxy.has_value());
    EXPECT_EQ(proxy->type, HTTP::Proxy::Type::HTTP);
    EXPECT_EQ(proxy->host, "proxy.example"sv);
    EXPECT_EQ(proxy->port, 3128);
    EXPECT_EQ(proxy->to_curl_url(), "http://proxy.example:3128"sv);

    proxy = HTTP::Proxy::parse("SOCKS5H://user:pass@Proxy.Example/"sv);
    EXPECT(proxy.has_value());
    EXPECT_EQ(proxy->type, HTTP::Proxy::Type::SOCKS5Hostname);
    EXPECT_EQ(proxy->host, "proxy.example"sv);
    EXPECT_EQ(proxy->port, 1080);
    EXPECT_EQ(proxy->userinfo, "user:pass"sv);
    EXPECT_EQ(proxy->to_curl_url(), "socks5h://user:pass@proxy.example:1080"sv);

    proxy = HTTP::Proxy::parse("https://[::1]"sv);
    EXPECT(proxy.has_value());
    EXPECT_EQ(proxy->host, "::1"sv);
    EXPECT_EQ(proxy->port, 443);
    EXPECT_EQ(proxy->to_curl_url(), "https://[::1]:443"sv);

    EXPECT(!HTTP::Proxy::parse(""sv).has_value());
    EXPECT(!HTTP::Proxy::parse("ftp://proxy.example"sv).has_value());
    EXPECT(!HTTP::Proxy::parse("proxy.example:0"sv).has_value());
    EXPECT(!HTTP::Proxy::parse("proxy.example:99999"sv).has_value());
    EXPECT(!HTTP::Proxy::parse("http://:8080"sv).has_value());
    EXPECT(!HTTP::Proxy::parse("http://[::1:8080"sv).has_value());
    EXPECT(!HTTP::Proxy::parse("http://2001:db8::1:8080"sv).has_value());
}

TEST_CASE(proxy_hostname_resolution)
{
    EXPECT(HTTP::Proxy::parse("http://proxy"sv)->resolves_hostnames());
    EXPECT(HTTP::Proxy::parse("https://proxy"sv)->resolves_hostnames());
    EXPECT(HTTP::Proxy::parse("socks4a://proxy"sv)->resolves_hostnames());
    EXPECT(HTTP::Proxy::parse("socks5h://proxy"sv)->resolves_hostnames());
    EXPECT(!HTTP::Proxy::parse("socks4://proxy"sv)->resolves_hostnames());
    EXPECT(!HTTP::Proxy::parse("socks5://proxy"sv)->resolves_hostnames());
}

TEST_CASE(proxy_environment_precedence)
{
    auto configuration = configuration_from({ { "all_proxy"sv, "socks5://all"sv }, { "https_proxy"sv, "http://secure"sv } });
    EXPECT_EQ(configuration.proxy_for(url("http://example.com"sv))->host, "all"sv);
    EXPECT_EQ(configuration.proxy_for(url("ws://example.com"sv))->host, "all"sv);
    EXPECT_EQ(configuration.proxy_for(url("https://example.com"sv))->host, "secure"sv);
    EXPECT_EQ(configuration.proxy_for(url("wss://example.com"sv))->host, "secure"sv);
    EXPECT(!configuration.proxy_for(url("file:///etc/passwd"sv)).has_value());

    configuration = configuration_from({ { "HTTP_PROXY"sv, "http://upper"sv }, { "HTTPS_PROXY"sv, "http://upper"sv } });
    EXPECT(!configuration.proxy_for(url("http://example.com"sv)).has_value());
    EXPECT_EQ(configuration.proxy_for(url("https://example.com"sv))->host, "upper"sv);

    configuration = configuration_from({ { "http_proxy"sv, "ftp://invalid"sv } });
    EXPECT(configuration.is_empty());
}

TEST_CASE(no_proxy_matching)
{
    auto configuration = configuration_from({
        { "all_proxy"sv, "http://proxy"sv },
        { "no_proxy"sv, "localhost, .internal.example corp.example.,10.0.0.0/8 ::1 192.168.1.1"sv },
    });

    auto is_proxied = [&](StringView string) { return configuration.proxy_for(url(string)).has_value(); };

    EXPECT(!is_proxied("http://localhost/"sv));
    EXPECT(!is_proxied("http://LOCALHOST./"sv));
    EXPECT(is_proxied("http://notlocalhost/"sv));
    EXPECT(!is_proxied("http://internal.example/"sv));
    EXPECT(!is_proxied("http://a.internal.example/"sv));
    EXPECT(!is_proxied("https://b.corp.example/"sv));
    EXPECT(is_proxied("https://xcorp.example/"sv));
    EXPECT(!is_proxied("http://10.1.2.3/"sv));
    EXPECT(is_proxied("http://11.1.2.3/"sv));
    EXPECT(!is_proxied("http://[::1]/"sv));
    EXPECT(is_proxied("http://[::2]/"sv));
    EXPECT(!is_proxied("http://192.168.1.1/"sv));
    EXPECT(is_proxied("http://192.168.1.2/"sv));

    configuration = configuration_from({ { "all_proxy"sv, "http://proxy"sv }, { "NO_PROXY"sv, "*"sv } });
    EXPECT(!is_proxied("https://example.com/"sv));
}
