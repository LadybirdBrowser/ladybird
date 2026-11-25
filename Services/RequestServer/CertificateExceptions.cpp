/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/StringBuilder.h>
#include <RequestServer/CertificateExceptions.h>

#if defined(AK_OS_WINDOWS)
// Included before the OpenSSL headers so the winsock2 struct timeval is renamed and can't collide with AK's.
#    include <AK/Windows.h>
#endif

#include <openssl/evp.h>
#include <openssl/x509.h>

namespace RequestServer {

// The errors each certificate is trusted in spite of, by fingerprint.
struct TrustedCertificate {
    Requests::CertificateError errors { Requests::CertificateError::None };
    ByteString error_details;
};
using TrustedCertificates = HashMap<ByteString, TrustedCertificate>;

static HashMap<ByteString, TrustedCertificates>& certificate_exceptions()
{
    static HashMap<ByteString, TrustedCertificates> exceptions;
    return exceptions;
}

static ByteString exception_key(ByteString const& host, u16 port)
{
    return ByteString::formatted("{}:{}", host, port);
}

ByteString certificate_fingerprint(X509* certificate)
{
    u8 digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    if (X509_digest(certificate, EVP_sha256(), digest, &digest_length) != 1)
        return {};

    StringBuilder builder;
    for (unsigned int i = 0; i < digest_length; ++i)
        builder.appendff("{:02x}", digest[i]);
    return builder.to_byte_string();
}

void add_certificate_exception(ByteString const& host, u16 port, ByteString fingerprint, Requests::CertificateError errors, ByteString error_details)
{
    if (fingerprint.is_empty())
        return;
    // The latest review covers all current errors and replaces earlier approvals.
    certificate_exceptions().ensure(exception_key(host, port)).set(move(fingerprint), { errors, move(error_details) });
}

bool has_certificate_exception(ByteString const& host, u16 port, ByteString const& fingerprint, Requests::CertificateError errors, StringView error_details)
{
    if (fingerprint.is_empty())
        return false;
    auto exceptions = certificate_exceptions().get(exception_key(host, port));
    if (!exceptions.has_value())
        return false;
    // A certificate that has since begun to fail for another reason goes back to the user.
    auto trusted = exceptions->get(fingerprint);
    if (!trusted.has_value() || has_any_flag(errors, ~trusted->errors))
        return false;
    // Descriptions distinguish errors sharing the Other flag.
    return !has_flag(errors, Requests::CertificateError::Other) || error_details == trusted->error_details;
}

bool has_any_certificate_exception(ByteString const& host, u16 port)
{
    return certificate_exceptions().contains(exception_key(host, port));
}

}
