/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibDNS/Resolver.h>

namespace RequestServer {

struct DNSStreamTLSOptions {
    // Root certificates to trust on top of the system's, if any.
    ByteString root_certificates_path;
    // A CURLOPT_SSL_CTX_FUNCTION, called with no CURLOPT_SSL_CTX_DATA.
    void* ssl_context_function { nullptr };
};

// Connects through curl without blocking, including the handshake for DNS over TLS.
ErrorOr<NonnullOwnPtr<DNS::StreamConnection>> connect_dns_stream(Core::SocketAddress const&, ByteString const& server_name, DNS::StreamSecurity, DNSStreamTLSOptions const&);

}
