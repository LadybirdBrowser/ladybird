/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Types.h>
#include <LibCrypto/OpenSSLForward.h>
#include <LibRequests/CertificateError.h>

namespace RequestServer {

// The hex-encoded SHA-256 digest of a certificate's DER encoding.
ByteString certificate_fingerprint(X509* certificate);

// The UI process grants exceptions for user-trusted certificates that fail verification.
// Each is limited to one host and port and to the errors the user reviewed, and lasts for this process's lifetime.
void add_certificate_exception(ByteString const& host, u16 port, ByteString fingerprint, Requests::CertificateError, ByteString error_details = {});
bool has_certificate_exception(ByteString const& host, u16 port, ByteString const& fingerprint, Requests::CertificateError, StringView error_details = {});
bool has_any_certificate_exception(ByteString const& host, u16 port);

}
