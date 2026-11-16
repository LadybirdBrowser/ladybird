/*
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibCrypto/ASN1/DER.h>
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

// https://www.rfc-editor.org/rfc/rfc5915#section-3
class ECPrivateKey {
public:
    ECPrivateKey() = default;

    static ErrorOr<ECPrivateKey> generate(size_t scalar_size);
    static ErrorOr<ECPrivateKey> from_scalar(ReadonlyBytes d, Optional<ASN1::ObjectIdentifier> parameters, Optional<ECPublicKey> public_key);

    // The ECPrivateKey inside a PKCS#8 PrivateKeyInfo, whose key OpenSSL has decoded, and whose AlgorithmIdentifier names the curve.
    static ErrorOr<ECPrivateKey> from_private_key_info(EVP_PKEY*, ASN1::ObjectIdentifier const& curve, ReadonlyBytes encoded);

    size_t scalar_size() const { return m_scalar_size; }

    ErrorOr<ByteBuffer> d_bytes() const;
    ErrorOr<ECPublicKey> derive_public_key() const;

    Optional<ASN1::ObjectIdentifier> const& parameters() const { return m_parameters; }
    Optional<ECPublicKey> const& public_key() const { return m_public_key; }

    ErrorOr<ByteBuffer> export_as_der() const;

    EVP_PKEY* openssl_key() const { return m_key.ptr(); }

private:
    ECPrivateKey(OpenSSL_SharedPKEY key, size_t scalar_size, Optional<ASN1::ObjectIdentifier> parameters, Optional<ECPublicKey> public_key)
        : m_key(move(key))
        , m_scalar_size(scalar_size)
        , m_parameters(move(parameters))
        , m_public_key(move(public_key))
    {
    }

    OpenSSL_SharedPKEY m_key;
    size_t m_scalar_size { 0 };
    Optional<ASN1::ObjectIdentifier> m_parameters;
    Optional<ECPublicKey> m_public_key;
};

struct ECDSASignature {
    ByteBuffer r;
    ByteBuffer s;
};

ErrorOr<ECDSASignature> ecdsa_sign(ECPrivateKey const&, ReadonlyBytes digest);
ErrorOr<bool> ecdsa_verify(ECPublicKey const&, ReadonlyBytes digest, ReadonlyBytes r, ReadonlyBytes s);

ErrorOr<ByteBuffer> ecdh_derive(ECPrivateKey const&, ECPublicKey const& peer);

}
