/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <LibCore/Environment.h>
#include <LibHTTP/Proxy.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibURL/URL.h>

namespace HTTP {

Optional<Proxy> Proxy::parse(StringView string)
{
    string = string.trim_whitespace();
    if (string.is_empty())
        return {};

    Proxy proxy;

    if (auto scheme_end = string.find("://"sv); scheme_end.has_value()) {
        auto scheme = string.substring_view(0, *scheme_end);
        if (scheme.equals_ignoring_ascii_case("http"sv))
            proxy.type = Type::HTTP;
        else if (scheme.equals_ignoring_ascii_case("https"sv))
            proxy.type = Type::HTTPS;
        else if (scheme.equals_ignoring_ascii_case("socks4"sv))
            proxy.type = Type::SOCKS4;
        else if (scheme.equals_ignoring_ascii_case("socks4a"sv))
            proxy.type = Type::SOCKS4A;
        else if (scheme.equals_ignoring_ascii_case("socks5"sv))
            proxy.type = Type::SOCKS5;
        else if (scheme.equals_ignoring_ascii_case("socks5h"sv))
            proxy.type = Type::SOCKS5Hostname;
        else
            return {};
        string = string.substring_view(*scheme_end + 3);
    }

    if (auto path_start = string.find('/'); path_start.has_value())
        string = string.substring_view(0, *path_start);

    if (auto userinfo_end = string.find_last('@'); userinfo_end.has_value()) {
        proxy.userinfo = string.substring_view(0, *userinfo_end);
        string = string.substring_view(*userinfo_end + 1);
    }

    Optional<StringView> port;
    if (string.starts_with('[')) {
        auto host_end = string.find(']');
        if (!host_end.has_value())
            return {};
        proxy.host = string.substring_view(1, *host_end - 1).to_byte_string().to_lowercase();
        auto rest = string.substring_view(*host_end + 1);
        if (!rest.is_empty()) {
            if (!rest.starts_with(':'))
                return {};
            port = rest.substring_view(1);
        }
    } else if (auto port_start = string.find_last(':'); port_start.has_value()) {
        // An IPv6 address has to be bracketed to tell its colons from the port separator.
        auto host = string.substring_view(0, *port_start);
        if (host.contains(':'))
            return {};
        proxy.host = host.to_byte_string().to_lowercase();
        port = string.substring_view(*port_start + 1);
    } else {
        proxy.host = string.to_byte_string().to_lowercase();
    }

    if (proxy.host.is_empty())
        return {};

    // Match libcurl's default proxy ports.
    if (port.has_value()) {
        auto number = port->to_number<u16>();
        if (!number.has_value() || *number == 0)
            return {};
        proxy.port = *number;
    } else {
        proxy.port = proxy.type == Type::HTTPS ? 443 : 1080;
    }

    return proxy;
}

bool Proxy::resolves_hostnames() const
{
    switch (type) {
    case Type::HTTP:
    case Type::HTTPS:
    case Type::SOCKS4A:
    case Type::SOCKS5Hostname:
        return true;
    case Type::SOCKS4:
    case Type::SOCKS5:
        return false;
    }
    VERIFY_NOT_REACHED();
}

static StringView scheme_for_type(Proxy::Type type)
{
    switch (type) {
    case Proxy::Type::HTTP:
        return "http"sv;
    case Proxy::Type::HTTPS:
        return "https"sv;
    case Proxy::Type::SOCKS4:
        return "socks4"sv;
    case Proxy::Type::SOCKS4A:
        return "socks4a"sv;
    case Proxy::Type::SOCKS5:
        return "socks5"sv;
    case Proxy::Type::SOCKS5Hostname:
        return "socks5h"sv;
    }
    VERIFY_NOT_REACHED();
}

ByteString Proxy::to_curl_url() const
{
    StringBuilder builder;
    builder.appendff("{}://", scheme_for_type(type));
    if (!userinfo.is_empty())
        builder.appendff("{}@", userinfo);
    if (host.contains(':'))
        builder.appendff("[{}]", host);
    else
        builder.append(host);
    builder.appendff(":{}", port);
    return builder.to_byte_string();
}

ProxyConfiguration ProxyConfiguration::from_environment()
{
    return from_environment([](StringView name) { return Core::Environment::get(name); });
}

ProxyConfiguration ProxyConfiguration::from_environment(EnvironmentLookup const& lookup)
{
    auto first_set = [&](std::initializer_list<StringView> names) -> Optional<StringView> {
        for (auto name : names) {
            if (auto value = lookup(name); value.has_value() && !value->trim_whitespace().is_empty())
                return value;
        }
        return {};
    };

    auto proxy_from = [&](std::initializer_list<StringView> names) -> Optional<Proxy> {
        if (auto value = first_set(names); value.has_value()) {
            if (auto proxy = Proxy::parse(*value); proxy.has_value())
                return proxy;
            dbgln("Ignoring invalid proxy '{}'", *value);
        }
        return {};
    };

    // Ignore HTTP_PROXY: CGI can populate it from a request's Proxy header.
    auto all_proxy = proxy_from({ "all_proxy"sv, "ALL_PROXY"sv });
    auto http_proxy = proxy_from({ "http_proxy"sv });
    auto https_proxy = proxy_from({ "https_proxy"sv, "HTTPS_PROXY"sv });

    Vector<ByteString> no_proxy;
    if (auto value = first_set({ "no_proxy"sv, "NO_PROXY"sv }); value.has_value()) {
        for (auto entry : value->split_view_if([](char c) { return c == ',' || is_ascii_space(c); }))
            no_proxy.append(entry.to_byte_string().to_lowercase());
    }

    return ProxyConfiguration {
        http_proxy.has_value() ? move(http_proxy) : all_proxy,
        https_proxy.has_value() ? move(https_proxy) : all_proxy,
        move(no_proxy),
    };
}

Optional<Proxy const&> ProxyConfiguration::proxy_for(URL::URL const& url) const
{
    Optional<Proxy> const* proxy = nullptr;
    if (url.scheme().is_one_of("http"sv, "ws"sv))
        proxy = &m_http_proxy;
    else if (url.scheme().is_one_of("https"sv, "wss"sv))
        proxy = &m_https_proxy;

    if (!proxy || !proxy->has_value() || !url.host().has_value())
        return {};
    if (is_excluded(url.serialized_host()))
        return {};
    return **proxy;
}

static bool address_matches(IPv4Address address, StringView entry)
{
    auto network = entry;
    u32 prefix_length = 32;
    if (auto slash = entry.find('/'); slash.has_value()) {
        network = entry.substring_view(0, *slash);
        auto length = entry.substring_view(*slash + 1).to_number<u32>();
        if (!length.has_value() || *length > 32)
            return false;
        prefix_length = *length;
    }

    auto network_address = IPv4Address::from_string(network);
    if (!network_address.has_value())
        return false;

    auto to_u32 = [](IPv4Address a) { return (static_cast<u32>(a[0]) << 24) | (a[1] << 16) | (a[2] << 8) | a[3]; };
    u32 mask = prefix_length == 0 ? 0 : ~0u << (32 - prefix_length);
    return (to_u32(address) & mask) == (to_u32(*network_address) & mask);
}

static bool address_matches(IPv6Address address, StringView entry)
{
    auto network = entry;
    u32 prefix_length = 128;
    if (auto slash = entry.find('/'); slash.has_value()) {
        network = entry.substring_view(0, *slash);
        auto length = entry.substring_view(*slash + 1).to_number<u32>();
        if (!length.has_value() || *length > 128)
            return false;
        prefix_length = *length;
    }
    if (network.starts_with('[') && network.ends_with(']'))
        network = network.substring_view(1, network.length() - 2);

    auto network_address = IPv6Address::from_string(network);
    if (!network_address.has_value())
        return false;

    for (size_t i = 0; i < 8 && prefix_length > 0; ++i) {
        u32 bits = min(prefix_length, 16u);
        u16 mask = static_cast<u16>(0xffff << (16 - bits));
        if ((address[i] & mask) != ((*network_address)[i] & mask))
            return false;
        prefix_length -= bits;
    }
    return true;
}

// Matches the host the way libcurl matches NO_PROXY: "*" excludes every host, an address or CIDR range excludes the
// addresses in it, and a name excludes itself and every name below it.
bool ProxyConfiguration::is_excluded(StringView host) const
{
    if (host.starts_with('[') && host.ends_with(']'))
        host = host.substring_view(1, host.length() - 2);
    if (host.ends_with('.'))
        host = host.substring_view(0, host.length() - 1);

    auto ipv4_address = IPv4Address::from_string(host);
    auto ipv6_address = ipv4_address.has_value() ? Optional<IPv6Address> {} : IPv6Address::from_string(host);

    for (auto const& entry : m_no_proxy) {
        if (entry == "*"sv)
            return true;

        if (ipv4_address.has_value()) {
            if (address_matches(*ipv4_address, entry))
                return true;
            continue;
        }
        if (ipv6_address.has_value()) {
            if (address_matches(*ipv6_address, entry))
                return true;
            continue;
        }

        StringView name = entry;
        if (name.starts_with('.'))
            name = name.substring_view(1);
        if (name.ends_with('.'))
            name = name.substring_view(0, name.length() - 1);
        if (name.is_empty())
            continue;

        if (host.equals_ignoring_ascii_case(name))
            return true;
        if (host.length() > name.length() && host[host.length() - name.length() - 1] == '.' && host.ends_with(name, CaseSensitivity::CaseInsensitive))
            return true;
    }

    return false;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, HTTP::Proxy const& proxy)
{
    TRY(encoder.encode(proxy.type));
    TRY(encoder.encode(proxy.host));
    TRY(encoder.encode(proxy.port));
    TRY(encoder.encode(proxy.userinfo));
    return {};
}

template<>
ErrorOr<HTTP::Proxy> decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<HTTP::Proxy::Type>());
    if (to_underlying(type) > to_underlying(HTTP::Proxy::Type::SOCKS5Hostname))
        return Error::from_string_literal("Invalid proxy type");

    auto host = TRY(decoder.decode<ByteString>());
    auto port = TRY(decoder.decode<u16>());
    auto userinfo = TRY(decoder.decode<ByteString>());
    return HTTP::Proxy { type, move(host), port, move(userinfo) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, HTTP::ProxyConfiguration const& configuration)
{
    TRY(encoder.encode(configuration.http_proxy()));
    TRY(encoder.encode(configuration.https_proxy()));
    TRY(encoder.encode(configuration.no_proxy()));
    return {};
}

template<>
ErrorOr<HTTP::ProxyConfiguration> decode(Decoder& decoder)
{
    auto http_proxy = TRY(decoder.decode<Optional<HTTP::Proxy>>());
    auto https_proxy = TRY(decoder.decode<Optional<HTTP::Proxy>>());
    auto no_proxy = TRY(decoder.decode<Vector<ByteString>>());
    return HTTP::ProxyConfiguration { move(http_proxy), move(https_proxy), move(no_proxy) };
}

}
