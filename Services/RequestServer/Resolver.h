/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>
#include <LibDNS/Resolver.h>

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

    DNS::Resolver dns;

private:
    explicit Resolver(Function<ErrorOr<DNS::Resolver::SocketResult>()> create_socket);
};

ByteBuffer const& root_certificate_bundle();
void set_root_certificate_bundle(ByteBuffer);

}
