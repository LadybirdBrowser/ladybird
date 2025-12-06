/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/Argon2.h>

#include <AK/ByteBuffer.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/thread.h>

namespace Crypto::Hash {

Argon2::Argon2(char const* openssl_name)
    : m_kdf(EVP_KDF_fetch(nullptr, openssl_name, nullptr))
{
}

ErrorOr<ByteBuffer> Argon2::derive_key(
    ReadonlyBytes message,
    ReadonlyBytes nonce,
    u32 parallelism,
    u32 memory,
    u32 passes,
    u32 version,
    Optional<ByteBuffer> secret_value,
    Optional<ByteBuffer> associated_data,
    u32 tag_length) const
{

    auto ctx = TRY(OpenSSL_KDF_CTX::wrap(EVP_KDF_CTX_new(m_kdf)));

    OPENSSL_TRY(OSSL_set_max_threads(nullptr, parallelism));

    OSSL_PARAM params[] = {
        OSSL_PARAM_uint32(OSSL_KDF_PARAM_THREADS, &parallelism),
        OSSL_PARAM_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &parallelism),
        OSSL_PARAM_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory),
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_SALT, const_cast<u8*>(nonce.data()), nonce.size()),
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_PASSWORD, const_cast<u8*>(message.data()), message.size()),
        OSSL_PARAM_uint32(OSSL_KDF_PARAM_ARGON2_VERSION, &version),
        OSSL_PARAM_uint32(OSSL_KDF_PARAM_ITER, &passes),
        OSSL_PARAM_uint32(OSSL_KDF_PARAM_SIZE, &tag_length),
        OSSL_PARAM_END,
        OSSL_PARAM_END,
        OSSL_PARAM_END,
    };

    auto insertion_point = 8;

    if (secret_value.has_value()) {
        params[insertion_point++] = OSSL_PARAM_octet_string(OSSL_KDF_PARAM_SECRET, secret_value->data(), secret_value->size());
    }

    if (associated_data.has_value()) {
        params[insertion_point++] = OSSL_PARAM_octet_string(OSSL_KDF_PARAM_ARGON2_AD, associated_data->data(), associated_data->size());
    }

    auto buf = TRY(ByteBuffer::create_uninitialized(tag_length));
    OPENSSL_TRY(EVP_KDF_derive(ctx.ptr(), buf.data(), tag_length, params));

    return buf;
}

}
