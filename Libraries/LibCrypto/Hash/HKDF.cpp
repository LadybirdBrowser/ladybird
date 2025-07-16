/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/HKDF.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

namespace Crypto::Hash {

HKDF::HKDF(HashKind hash_kind)
    : m_kdf(EVP_KDF_fetch(nullptr, "HKDF", nullptr))
    , m_hash_kind(hash_kind)
{
}

ErrorOr<ByteBuffer> HKDF::derive_key(Optional<ReadonlyBytes> maybe_salt, ReadonlyBytes key, ReadonlyBytes info, u32 key_length_bytes)
{
    auto hash_name = TRY(hash_kind_to_openssl_digest_name(m_hash_kind));

    auto ctx = TRY(OpenSSL_KDF_CTX::wrap(EVP_KDF_CTX_new(m_kdf)));

    OSSL_PARAM params[] = {
        OSSL_PARAM_utf8_string(OSSL_KDF_PARAM_DIGEST, const_cast<char*>(hash_name.characters_without_null_termination()), hash_name.length()),
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_KEY, const_cast<u8*>(key.data()), key.size()),
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_INFO, const_cast<u8*>(info.data()), info.size()),
        OSSL_PARAM_END,
        OSSL_PARAM_END,
    };

    if (maybe_salt.has_value()) {
        static constexpr u8 empty_salt[0] {};

        // FIXME: As of openssl 3.5.1, we can no longer pass a null salt pointer. This seems like a mistake; we should
        //        check if this is still the case in the next openssl release. See:
        //        https://github.com/openssl/openssl/pull/27305#discussion_r2198316685
        auto salt = maybe_salt->is_null() ? ReadonlySpan<u8> { empty_salt, 0 } : *maybe_salt;

        params[3] = OSSL_PARAM_octet_string(OSSL_KDF_PARAM_SALT, const_cast<u8*>(salt.data()), salt.size());
    }

    auto buf = TRY(ByteBuffer::create_uninitialized(key_length_bytes));
    OPENSSL_TRY(EVP_KDF_derive(ctx.ptr(), buf.data(), key_length_bytes, params));

    return buf;
}

}
