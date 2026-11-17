/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibCrypto/OpenSSL.h>

// NB: RSA key components are big-endian unsigned integers.
namespace Crypto::PK {

class RSAPublicKey {
public:
    RSAPublicKey() = default;

    static ErrorOr<RSAPublicKey> from_components(ReadonlyBytes modulus, ReadonlyBytes public_exponent);

    size_t length() const { return m_length; }

    ErrorOr<bool> is_valid() const;

    EVP_PKEY* openssl_key() const { return m_key.ptr(); }

private:
    RSAPublicKey(OpenSSL_SharedPKEY key);

    OpenSSL_SharedPKEY m_key;
    size_t m_length { 0 };
};

ErrorOr<bool> rsa_pkcs1_verify(RSAPublicKey const&, Hash::HashKind, ReadonlyBytes message, ReadonlyBytes signature);

}
