/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/PBKDF2.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

namespace Crypto::Hash {

PBKDF2::PBKDF2(HashKind hash_kind)
    : m_kdf(EVP_KDF_fetch(nullptr, "PBKDF2", nullptr))
    , m_hash_kind(hash_kind)
{
}

ErrorOr<ByteBuffer> PBKDF2::derive_key(ReadonlyBytes password, ReadonlyBytes salt, u32 iterations, u32 key_length_bytes)
{
    auto hash_name = TRY(hash_kind_to_openssl_digest_name(m_hash_kind));

    auto ctx = TRY(OpenSSL_KDF_CTX::wrap(EVP_KDF_CTX_new(m_kdf)));

    OSSL_PARAM params[] = {
        OSSL_PARAM_utf8_string(OSSL_KDF_PARAM_DIGEST, const_cast<char*>(hash_name.characters_without_null_termination()), hash_name.length()),
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_PASSWORD, const_cast<u8*>(password.data()), password.size()),
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_SALT, const_cast<u8*>(salt.data()), salt.size()),
        OSSL_PARAM_uint32(OSSL_KDF_PARAM_ITER, &iterations),
        OSSL_PARAM_END,
    };

    auto buf = TRY(ByteBuffer::create_uninitialized(key_length_bytes));
    OPENSSL_TRY(EVP_KDF_derive(ctx.ptr(), buf.data(), key_length_bytes, params));

    return buf;
}

}
