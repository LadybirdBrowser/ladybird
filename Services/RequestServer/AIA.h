/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/OpenSSLForward.h>
#include <LibRequests/CertificateError.h>
#include <RequestServer/IsPrivate.h>

extern "C" {
typedef struct ssl_ctx_st SSL_CTX;
}

namespace RequestServer {

// Per-request state shared with (per-connection, possibly pooled and longer-lived) SSL_CTX during verification. Ref-
// counted, so a connection outliving its originating request can't leave the verify callback with a dangling pointer.
class AIACollector : public RefCounted<AIACollector> {
public:
    explicit AIACollector(IsPrivate is_private)
        : is_private(is_private)
    {
    }

    IsPrivate const is_private;           // Which of the process-wide caches this request may use and add to.
    Vector<ByteString> pending_urls;      // caIssuers URLs collected during verification, awaiting fetch.
    HashTable<ByteString> attempted_urls; // URLs already fetched for this request, so they're not re-collected.

    // Only intermediates used in verified chains enter the shared cache, preventing unrelated-chain poisoning.
    Vector<Crypto::OpenSSL_X509> fetched_intermediates;

    // Certificate exceptions must match the request's host and port.
    ByteString host;
    u16 port { 0 };

    // Verification checks the host name instead of curl when an exception may allow a certificate for another name.
    bool checks_host_name { false };

    // The server's certificate, if the last verification refused it, and why.
    struct RejectedCertificate {
        ByteString fingerprint;
        Requests::CertificateError errors { Requests::CertificateError::None };
        // Descriptions of errors without a dedicated CertificateError flag.
        ByteString details;
    };
    Optional<RejectedCertificate> rejected_certificate;

    // Retain the chain to explain a macOS system verification failure after the handshake.
    struct ChainForSystemVerifier {
        RejectedCertificate rejection;
        Vector<Crypto::OpenSSL_X509> certificates; // The leaf first.
    };
    Optional<ChainForSystemVerifier> chain_for_system_verifier;
};

// Accept certificates with user exceptions; otherwise record the rejected certificate and its errors in the collector.
//
// During verification, any cert whose issuer we can't find locally or in the fetched-intermediate cache has its
// caIssuers URLs appended to collector.pending_urls (skipping ones already attempted or recently known dead).
void apply_aia_verification(SSL_CTX* ssl_context, AIACollector& collector);

// Call after curl rejects the certificate to record any deferred system verification errors.
void explain_system_verifier_refusal(AIACollector&);

// Returns true if the body yielded a certificate.
bool add_fetched_aia_intermediates(ReadonlyBytes body, ReadonlySpan<NonnullRefPtr<AIACollector>> collectors);

// Record that fetching the given caIssuers URL failed — so it's not retried.
void mark_aia_url_failed(IsPrivate, ByteString url);

void clear_aia_state(IsPrivate);

// The following two parsing primitives are exposed for unit testing.
Vector<ByteString> ca_issuers_urls(X509* certificate);
Vector<X509*> parse_certificates(ReadonlyBytes body);

}
