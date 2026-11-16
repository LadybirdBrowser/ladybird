/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Authentication/HMAC.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/evp.h>

namespace Crypto::Authentication {

ErrorOr<Hash::Digest> hmac(Hash::HashKind hash_kind, ReadonlyBytes key, ReadonlyBytes message)
{
    auto const* hash_name = TRY(Hash::openssl_digest_name(hash_kind));

    // NB: OpenSSL rejects a null key pointer, which an empty key may have.
    static constexpr u8 empty_key[1] {};
    auto const* key_data = key.is_empty() ? empty_key : key.data();

    Hash::Digest result;
    OPENSSL_TRY_PTR(EVP_Q_mac(nullptr, "HMAC", nullptr, hash_name, nullptr, key_data, key.size(), message.data(), message.size(), result.data.data(), result.data.size(), &result.size));
    return result;
}

}
