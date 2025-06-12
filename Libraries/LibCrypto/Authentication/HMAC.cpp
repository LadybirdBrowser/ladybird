/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Authentication/HMAC.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>

namespace Crypto::Authentication {

HMAC::HMAC(Hash::HashKind hash_kind, ReadonlyBytes key)
    : m_hash_kind(hash_kind)
    , m_key(key)
    , m_mac(EVP_MAC_fetch(nullptr, "HMAC", nullptr))
{
    reset();
}

HMAC::~HMAC()
{
    EVP_MAC_free(m_mac);
    EVP_MAC_CTX_free(m_ctx);
}

size_t HMAC::digest_size() const
{
    return EVP_MAC_CTX_get_mac_size(m_ctx);
}

void HMAC::update(u8 const* message, size_t length)
{
    if (EVP_MAC_update(m_ctx, message, length) != 1) {
        VERIFY_NOT_REACHED();
    }
}

ByteBuffer HMAC::digest()
{
    auto buf = MUST(ByteBuffer::create_uninitialized(digest_size()));

    auto size = digest_size();
    if (EVP_MAC_final(m_ctx, buf.data(), &size, size) != 1) {
        VERIFY_NOT_REACHED();
    }

    return MUST(buf.slice(0, size));
}

void HMAC::reset()
{
    EVP_MAC_CTX_free(m_ctx);
    m_ctx = EVP_MAC_CTX_new(m_mac);

    auto hash_name = MUST(hash_kind_to_openssl_digest_name(m_hash_kind));

    OSSL_PARAM params[] = {
        OSSL_PARAM_utf8_string(OSSL_MAC_PARAM_DIGEST, const_cast<char*>(hash_name.characters_without_null_termination()), hash_name.length()),
        OSSL_PARAM_END
    };

    if (EVP_MAC_init(m_ctx, m_key.data(), m_key.size(), params) != 1) {
        VERIFY_NOT_REACHED();
    }
}

}
