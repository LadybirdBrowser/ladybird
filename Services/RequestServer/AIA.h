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
#include <openssl/types.h>

typedef void CURL;

namespace RequestServer {

// Per-request state shared with (per-connection, possibly pooled and longer-lived) SSL_CTX during verification. Ref-
// counted, so a connection outliving its originating request can't leave the verify callback with a dangling pointer.
class AIACollector : public RefCounted<AIACollector> {
public:
    Vector<ByteString> pending_urls;      // caIssuers URLs collected during verification, awaiting fetch.
    HashTable<ByteString> attempted_urls; // URLs already fetched for this request, so they're not re-collected.
};

// During verification, any cert whose issuer we can't find locally or in the fetched-intermediate cache has its
// caIssuers URLs appended to collector.pending_urls (skipping ones already attempted or recently known dead).
void install_aia_verification_hook(CURL* easy_handle, AIACollector& collector);

// Parse a fetched AIA response body and, if it yields a certificate, add it to the process-wide intermediate cache
// that's consulted during verification. Returns true if a certificate was added.
bool add_fetched_aia_intermediate(ReadonlyBytes body);

// Record that fetching the given caIssuers URL failed — so it's not retried.
void mark_aia_url_failed(ByteString url);

// The following two parsing primitives are exposed for unit testing.
Vector<ByteString> ca_issuers_urls(X509* certificate);
Vector<X509*> parse_certificates(ReadonlyBytes body);

}
