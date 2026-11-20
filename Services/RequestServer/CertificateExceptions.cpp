/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/StringBuilder.h>
#include <RequestServer/CertificateExceptions.h>

#if defined(AK_OS_WINDOWS)
// Included before the OpenSSL headers so the winsock2 struct timeval is renamed and can't collide with AK's.
#    include <AK/Windows.h>
#endif

#include <openssl/evp.h>
#include <openssl/x509.h>

namespace RequestServer {

static HashMap<ByteString, HashTable<ByteString>>& certificate_exceptions()
{
    static HashMap<ByteString, HashTable<ByteString>> exceptions;
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

void add_certificate_exception(ByteString const& host, u16 port, ByteString fingerprint)
{
    if (fingerprint.is_empty())
        return;
    certificate_exceptions().ensure(exception_key(host, port)).set(move(fingerprint));
}

bool has_certificate_exception(ByteString const& host, u16 port, ByteString const& fingerprint)
{
    if (fingerprint.is_empty())
        return false;
    auto exceptions = certificate_exceptions().get(exception_key(host, port));
    return exceptions.has_value() && exceptions->contains(fingerprint);
}

bool has_any_certificate_exception(ByteString const& host, u16 port)
{
    return certificate_exceptions().contains(exception_key(host, port));
}

}
