/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Span.h>
#include <LibCrypto/OpenSSLForward.h>
#include <LibRequests/CertificateError.h>

namespace RequestServer {

struct SystemTrustVerdict {
    Requests::CertificateError errors { Requests::CertificateError::None };
    // System descriptions of errors without a dedicated CertificateError flag.
    ByteString details;
};

// Query the system's issuer trust and rejection reasons without changing trust settings.
// The caller checks host names.
SystemTrustVerdict system_trust_verdict(X509* leaf, ReadonlySpan<X509*> intermediates);

}
