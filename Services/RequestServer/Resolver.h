/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Utf16String.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>
#include <LibDNS/Resolver.h>
#include <LibHTTP/Proxy.h>

namespace RequestServer {

struct DNSInfo {
    static DNSInfo& the();

    Optional<Core::SocketAddress> server_address;
    Optional<ByteString> server_hostname;
    u16 port { 0 };
    bool use_dns_over_tls { true };
    bool validate_dnssec_locally { false };

    // Only configured DNS gives us the full address pool to distribute requests over.
    bool uses_configured_dns_server() const { return server_address.has_value() || server_hostname.has_value(); }

private:
    DNSInfo() = default;
};

struct Resolver
    : public RefCounted<Resolver>
    , public Weakable<Resolver> {
    static NonnullRefPtr<Resolver> default_resolver();

    // Isolate private DNS cache history; discard it when the last private client leaves.
    static NonnullRefPtr<Resolver> private_resolver();

    static void reset_connections();

    DNS::Resolver dns;

private:
    static NonnullRefPtr<Resolver> create();

    explicit Resolver(Function<ErrorOr<Optional<DNS::Resolver::SocketResult>>()> create_socket);
};

ByteString const& default_certificate_path();
void set_default_certificate_path(ByteString);

HTTP::ProxyConfiguration const& proxy_configuration();
void set_proxy_configuration(HTTP::ProxyConfiguration);

// The top-level site this RequestServer serves, when there is one per site.
Optional<Utf16String> const& process_top_level_site();
void set_process_top_level_site(Optional<Utf16String>);

// Sites have RequestServers of their own, and this one has no site.
void set_process_sites_have_their_own();

// Cache only this server's site's responses. A server with no site caches all sites unless they have
// their own RequestServers; other sites' responses are only in transit to their site's server.
bool process_keeps_responses_under(Utf16String const& top_level_site);

}
