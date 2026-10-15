/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashTable.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibCrypto/OpenSSL.h>
#include <LibTLS/OpenSSLForward.h>
#include <RequestServer/IsPrivate.h>

namespace RequestServer {

// Per-request AIA state: the caIssuers URLs still to fetch, and the intermediates fetched so far. Ref-counted, since
// one fetch can serve several requests.
class AIACollector : public RefCounted<AIACollector> {
public:
    explicit AIACollector(IsPrivate is_private)
        : is_private(is_private)
    {
    }

    IsPrivate const is_private;           // Which of the process-wide caches this request may use and add to.
    Vector<ByteString> pending_urls;      // caIssuers URLs collected from the probed chain, awaiting fetch.
    HashTable<ByteString> attempted_urls; // URLs already fetched for this request, so they're not re-collected.

    // Only intermediates used in verified chains enter the shared cache, preventing unrelated-chain poisoning.
    Vector<Crypto::OpenSSL_X509> fetched_intermediates;
};

// Verifies the chain a server presented, leaf first, against the trust store at the path plus the fetched and cached
// intermediates. A chain that fails for want of an issuer leaves that certificate's caIssuers URLs in
// collector.pending_urls for the request to fetch (skipping ones already attempted or recently known dead), and one
// that verifies promotes the fetched intermediates it used to the shared cache. Returns whether the chain verified.
bool verify_chain_for_aia(ReadonlySpan<X509*> chain, AIACollector& collector, ByteString const& trust_store_path);

// The fetched and cached intermediates as PEM, for libcurl to see next to the trust store's roots. A TLS backend may
// take anything in that bundle for a trust anchor, so a request hands them over only once verify_chain_for_aia() has
// shown its chain reaches a real root through them.
ByteString aia_intermediates_pem(AIACollector const& collector);

// Returns true if the body yielded a certificate.
bool add_fetched_aia_intermediates(ReadonlyBytes body, ReadonlySpan<NonnullRefPtr<AIACollector>> collectors);

// Record that fetching the given caIssuers URL failed — so it's not retried.
void mark_aia_url_failed(IsPrivate, ByteString url);

void clear_aia_state(IsPrivate);

// The following two parsing primitives are exposed for unit testing.
Vector<ByteString> ca_issuers_urls(X509* certificate);
Vector<X509*> parse_certificates(ReadonlyBytes body);

}
