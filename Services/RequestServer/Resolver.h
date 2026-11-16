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
    Vector<ByteString> loopback_domains;

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

    // From now on, every resolver sends the names that it looks up with the system resolver to the process that
    // launched RequestServer: RequestServer may not read the files the system resolver needs, like /etc/resolv.conf.
    static void resolve_system_names_remotely();

    DNS::Resolver dns;

private:
    static NonnullRefPtr<Resolver> create();

    explicit Resolver(Function<ErrorOr<Optional<DNS::Resolver::SocketResult>>()> create_socket);
};

// curl resolves the host of a proxy given by name itself, with the system resolver, unless the handle's CURLOPT_RESOLVE
// list pins it. But RequestServer may not read the files the system resolver needs. So this looks the host up with the
// system resolver the way RequestServer does, for curl_resolve_entry_for_proxy() to pin, and calls back once it has.
// There's nothing to look up for a proxy given as an address, or for no proxy.
void resolve_proxy_host(Optional<HTTP::Proxy> const&, Function<void(ErrorOr<void>)>);

// The CURLOPT_RESOLVE entry that pins the host of a proxy given by name to what resolve_proxy_host() found for it.
Optional<ByteString> curl_resolve_entry_for_proxy(Optional<HTTP::Proxy> const&);

ByteString const& default_certificate_path();
void set_default_certificate_path(ByteString);

HTTP::ProxyConfiguration const& proxy_configuration();
void set_proxy_configuration(HTTP::ProxyConfiguration);

// The top-level site this RequestServer serves, when there is one per site.
Optional<Utf16String> const& process_top_level_site();
void set_process_top_level_site(Optional<Utf16String>);

// Cache only this server's site's responses. Other sites' responses are only in transit to their site's server. A
// server with no site has a disk cache only if sites have no servers of their own, so it caches all sites.
bool process_keeps_responses_under(Utf16String const& top_level_site);

}
