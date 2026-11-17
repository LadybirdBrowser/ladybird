/*
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibCrypto/OpenSSL.h>

// NB: Each key's curve is the NIST prime curve whose coordinates have the key's scalar size: 32 bytes for P-256, 48 for
//     P-384 and 66 for P-521.
namespace Crypto::PK {

class ECPublicKey {
public:
    ECPublicKey() = default;

    static ErrorOr<ECPublicKey> from_octets(ReadonlyBytes);
    static ErrorOr<ECPublicKey> from_coordinates(ReadonlyBytes x, ReadonlyBytes y);

    size_t scalar_size() const { return m_scalar_size; }

    ErrorOr<ByteBuffer> x_bytes() const;
    ErrorOr<ByteBuffer> y_bytes() const;
    ErrorOr<ByteBuffer> to_uncompressed() const;

    EVP_PKEY* openssl_key() const { return m_key.ptr(); }

private:
    ECPublicKey(OpenSSL_SharedPKEY key, size_t scalar_size)
        : m_key(move(key))
        , m_scalar_size(scalar_size)
    {
    }

    OpenSSL_SharedPKEY m_key;
    size_t m_scalar_size { 0 };
};

ErrorOr<bool> ecdsa_verify(ECPublicKey const&, ReadonlyBytes digest, ReadonlyBytes r, ReadonlyBytes s);

}
