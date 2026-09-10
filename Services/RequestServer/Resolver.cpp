/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTLS/TLSv12.h>
#include <RequestServer/Resolver.h>

namespace RequestServer {

static ByteBuffer g_root_certificate_bundle;

ByteBuffer const& root_certificate_bundle()
{
    return g_root_certificate_bundle;
}

void set_root_certificate_bundle(ByteBuffer root_certificate_bundle)
{
    g_root_certificate_bundle = move(root_certificate_bundle);
}

DNSInfo& DNSInfo::the()
{
    static DNSInfo g_dns_info;
    return g_dns_info;
}

NonnullRefPtr<Resolver> Resolver::default_resolver()
{
    static WeakPtr<Resolver> g_resolver {};

    if (auto resolver = g_resolver.strong_ref())
        return *resolver;

    auto resolver = adopt_ref(*new Resolver([] -> ErrorOr<DNS::Resolver::SocketResult> {
        auto& dns_info = DNSInfo::the();

        if (!dns_info.server_address.has_value()) {
            if (!dns_info.server_hostname.has_value())
                return Error::from_string_literal("No DNS server configured");

            auto resolved = TRY(default_resolver()->dns.lookup(*dns_info.server_hostname)->await());
            if (!resolved->has_cached_addresses())
                return Error::from_string_literal("Failed to resolve DNS server hostname");

            auto address = resolved->cached_addresses().first().visit([&](auto& addr) -> Core::SocketAddress { return { addr, dns_info.port }; });
            dns_info.server_address = address;
        }

        if (dns_info.use_dns_over_tls) {
            TLS::Options options;

            if (auto const& bundle = g_root_certificate_bundle; !bundle.is_empty())
                options.root_certificates_blob = bundle.bytes();

            return DNS::Resolver::SocketResult {
                MaybeOwned<Core::Socket>(TRY(TLS::TLSv12::connect(*dns_info.server_address, *dns_info.server_hostname, move(options)))),
                DNS::Resolver::ConnectionMode::TCP,
            };
        }

        return DNS::Resolver::SocketResult {
            MaybeOwned<Core::Socket>(TRY(Core::BufferedUDPSocket::create(TRY(Core::UDPSocket::connect(*dns_info.server_address))))),
            DNS::Resolver::ConnectionMode::UDP,
        };
    }));

    g_resolver = resolver;
    return resolver;
}

Resolver::Resolver(Function<ErrorOr<DNS::Resolver::SocketResult>()> create_socket)
    : dns(move(create_socket))
{
}

}
