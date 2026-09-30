/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibTLS/TLSv12.h>
#include <RequestServer/Resolver.h>

namespace RequestServer {

static ByteString g_default_certificate_path;

ByteString const& default_certificate_path()
{
    return g_default_certificate_path;
}

void set_default_certificate_path(ByteString default_certificate_path)
{
    g_default_certificate_path = move(default_certificate_path);
}

static HTTP::ProxyConfiguration g_proxy_configuration;

HTTP::ProxyConfiguration const& proxy_configuration()
{
    return g_proxy_configuration;
}

void set_proxy_configuration(HTTP::ProxyConfiguration proxy_configuration)
{
    g_proxy_configuration = move(proxy_configuration);
}

DNSInfo& DNSInfo::the()
{
    static DNSInfo g_dns_info;
    return g_dns_info;
}

// System resolver to resolve the resolvers themselves.
static DNS::Resolver& system_resolver()
{
    static NeverDestroyed<DNS::Resolver> resolver { [] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
        return OptionalNone {};
    } };
    return *resolver;
}

static WeakPtr<Resolver> g_default_resolver;
static WeakPtr<Resolver> g_private_resolver;

NonnullRefPtr<Resolver> Resolver::default_resolver()
{
    if (auto resolver = g_default_resolver.strong_ref())
        return *resolver;

    auto resolver = create();
    g_default_resolver = resolver;
    return resolver;
}

NonnullRefPtr<Resolver> Resolver::private_resolver()
{
    if (auto resolver = g_private_resolver.strong_ref())
        return *resolver;

    auto resolver = create();
    g_private_resolver = resolver;
    return resolver;
}

void Resolver::reset_connections()
{
    for (auto* weak_resolver : { &g_default_resolver, &g_private_resolver }) {
        if (auto resolver = weak_resolver->strong_ref())
            resolver->dns.reset_connection();
    }
}

NonnullRefPtr<Resolver> Resolver::create()
{
    return adopt_ref(*new Resolver([] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
        auto& dns_info = DNSInfo::the();

        if (!dns_info.server_address.has_value()) {
            if (!dns_info.server_hostname.has_value())
                return OptionalNone {};

            auto resolved = TRY(system_resolver().lookup(*dns_info.server_hostname)->await());
            if (!resolved->has_cached_addresses())
                return Error::from_string_literal("Failed to resolve DNS server hostname");

            auto address = resolved->cached_addresses().first().visit([&](auto& addr) -> Core::SocketAddress { return { addr, dns_info.port }; });
            dns_info.server_address = address;
        }

        if (dns_info.use_dns_over_tls) {
            TLS::Options options;

            if (!g_default_certificate_path.is_empty())
                options.root_certificates_path = g_default_certificate_path;

            return DNS::Resolver::SocketResult {
                MaybeOwned<Core::Socket>(TRY(TLS::TLSv12::connect(*dns_info.server_address, *dns_info.server_hostname, move(options)))),
                DNS::Resolver::ConnectionMode::TCP,
            };
        }

        return DNS::Resolver::SocketResult {
            MaybeOwned<Core::Socket>(TRY(Core::UDPSocket::connect(*dns_info.server_address))),
            DNS::Resolver::ConnectionMode::UDP,
            [address = *dns_info.server_address] -> ErrorOr<NonnullOwnPtr<Core::Socket>> {
                return TRY(Core::TCPSocket::connect(address));
            },
        };
    }));
}

Resolver::Resolver(Function<ErrorOr<Optional<DNS::Resolver::SocketResult>>()> create_socket)
    : dns(move(create_socket))
{
}

}
