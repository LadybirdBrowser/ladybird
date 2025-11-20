/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

    auto resolver = adopt_ref(*new Resolver([] -> NonnullRefPtr<Core::Promise<DNS::Resolver::SocketResult>> {
        auto promise = Core::Promise<DNS::Resolver::SocketResult>::construct();
        auto& dns_info = DNSInfo::the();

        auto make_resolver = [] -> ErrorOr<DNS::Resolver::SocketResult> {
            auto& dns_info = DNSInfo::the();

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
                MaybeOwned<Core::Socket>(TRY(Core::BufferedUDPSocket::create(TRY(Core::UDPSocket::connect(*dns_info.server_address))))),
                DNS::Resolver::ConnectionMode::UDP,
            };
        };

        if (!dns_info.server_address.has_value()) {
            if (!dns_info.server_hostname.has_value()) {
                promise->reject(Error::from_string_literal("No DNS server configured"));
                return promise;
            }

            auto resolved_promise = default_resolver()->dns.lookup(*dns_info.server_hostname);
            resolved_promise->when_resolved([promise, make_resolver = move(make_resolver)](NonnullRefPtr<DNS::LookupResult const> const& resolved) -> ErrorOr<void> {
                if (!resolved->has_cached_addresses())
                    return Error::from_string_literal("Failed to resolve DNS server hostname");

                auto& dns_info = DNSInfo::the();
                auto address = resolved->cached_addresses().first().visit([&](auto& addr) -> Core::SocketAddress { return { addr, dns_info.port }; });
                dns_info.server_address = address;
                promise->resolve(TRY(make_resolver()));
                return {};
            });

            resolved_promise->when_rejected([promise](Error const& error) {
                promise->reject(Error::copy(error));
            });

            promise->add_child(move(resolved_promise));
        } else {
            auto result = make_resolver();
            if (!result.is_error()) {
                promise->resolve(result.release_value());
            } else {
                promise->reject(result.release_error());
            }
        }

        return promise;
    }));

    g_resolver = resolver;
    return resolver;
}

Resolver::Resolver(DNS::Resolver::CreateSocketFunction create_socket)
    : dns(move(create_socket))
{
}

}
