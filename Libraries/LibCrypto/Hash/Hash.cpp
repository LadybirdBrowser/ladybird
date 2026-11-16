/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/Hash.h>

#include <openssl/evp.h>

namespace Crypto::Hash {

static_assert(sizeof(Digest::data) == EVP_MAX_MD_SIZE);

ErrorOr<char const*> openssl_digest_name(HashKind kind)
{
    switch (kind) {
    case HashKind::SHA1:
        return "SHA1";
    case HashKind::SHA256:
        return "SHA256";
    case HashKind::SHA384:
        return "SHA384";
    case HashKind::SHA512:
        return "SHA512";
    case HashKind::SHA3_256:
        return "SHA3-256";
    case HashKind::SHA3_384:
        return "SHA3-384";
    case HashKind::SHA3_512:
        return "SHA3-512";
    case HashKind::Unknown:
    case HashKind::None:
        break;
    }
    return Error::from_string_literal("Unsupported hash kind");
}

size_t digest_size(HashKind kind)
{
    switch (kind) {
    case HashKind::SHA1:
        return 20;
    case HashKind::SHA256:
    case HashKind::SHA3_256:
        return 32;
    case HashKind::SHA384:
    case HashKind::SHA3_384:
        return 48;
    case HashKind::SHA512:
    case HashKind::SHA3_512:
        return 64;
    case HashKind::Unknown:
    case HashKind::None:
        break;
    }
    VERIFY_NOT_REACHED();
}

Digest digest(HashKind kind, ReadonlyBytes data)
{
    Digest result;
    if (EVP_Q_digest(nullptr, MUST(openssl_digest_name(kind)), nullptr, data.data(), data.size(), result.data.data(), &result.size) != 1)
        VERIFY_NOT_REACHED();
    return result;
}

Hasher::Hasher(HashKind kind)
    : m_context(EVP_MD_CTX_new())
{
    auto* type = EVP_MD_fetch(nullptr, MUST(openssl_digest_name(kind)), nullptr);
    if (!m_context || !type || EVP_DigestInit_ex(m_context, type, nullptr) != 1)
        VERIFY_NOT_REACHED();
    EVP_MD_free(type);
}

Hasher::~Hasher()
{
    EVP_MD_CTX_free(m_context);
}

void Hasher::update(ReadonlyBytes data)
{
    if (EVP_DigestUpdate(m_context, data.data(), data.size()) != 1)
        VERIFY_NOT_REACHED();
}

Digest Hasher::finish()
{
    Digest result;
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(m_context, result.data.data(), &size) != 1)
        VERIFY_NOT_REACHED();
    result.size = size;
    return result;
}

}
