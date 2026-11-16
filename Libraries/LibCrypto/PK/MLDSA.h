/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <LibCrypto/OpenSSL.h>

namespace Crypto::PK {

enum MLDSASize {
    MLDSA44,
    MLDSA65,
    MLDSA87,
};

class MLDSAPublicKey {
public:
    MLDSAPublicKey() = default;

    static ErrorOr<MLDSAPublicKey> from_bytes(ReadonlyBytes);

    MLDSASize size() const { return m_size; }
    ByteBuffer const& public_key() const { return m_public_key; }

    EVP_PKEY* openssl_key() const { return m_key.ptr(); }

private:
    MLDSAPublicKey(OpenSSL_SharedPKEY key, ByteBuffer public_key, MLDSASize size)
        : m_key(move(key))
        , m_public_key(move(public_key))
        , m_size(size)
    {
    }

    OpenSSL_SharedPKEY m_key;
    ByteBuffer m_public_key;
    MLDSASize m_size { MLDSA44 };
};

class MLDSAPrivateKey {
public:
    MLDSAPrivateKey() = default;

    // An empty seed generates a new key.
    static ErrorOr<MLDSAPrivateKey> generate(MLDSASize, ReadonlyBytes seed);

    // A key that was given in the expandedKey form, which Web Crypto rejects once it has parsed it. It has no seed,
    // public key or OpenSSL key, since OpenSSL would refuse an expanded key that is not valid.
    static MLDSAPrivateKey in_expanded_key_form(MLDSASize);

    MLDSASize size() const { return m_size; }
    static constexpr size_t seed_size = 32;

    bool has_seed() const { return m_key.ptr(); }
    // NB: OpenSSL keeps the seed, so that no copy of it is left behind when a key is moved or replaced. It is written
    //     into the caller's buffer, which the caller clears.
    ErrorOr<ReadonlyBytes> seed(Bytes buffer) const;
    ByteBuffer const& public_key() const { return m_public_key; }
    ErrorOr<ByteBuffer> expanded_key() const;

    ErrorOr<ByteBuffer> export_as_der() const;

    EVP_PKEY* openssl_key() const { return m_key.ptr(); }

private:
    MLDSAPrivateKey(OpenSSL_SharedPKEY key, ByteBuffer public_key, MLDSASize size)
        : m_key(move(key))
        , m_public_key(move(public_key))
        , m_size(size)
    {
    }

    OpenSSL_SharedPKEY m_key;
    ByteBuffer m_public_key;
    MLDSASize m_size { MLDSA44 };
};

struct MLDSAKeyPair {
    MLDSAPublicKey public_key;
    MLDSAPrivateKey private_key;
};

class MLDSA {
public:
    using KeyPairType = MLDSAKeyPair;

    static ErrorOr<KeyPairType> parse_mldsa_key(MLDSASize, ReadonlyBytes der);
    static ErrorOr<KeyPairType> generate_key_pair(MLDSASize, ReadonlyBytes seed = {});
};

ErrorOr<ByteBuffer> mldsa_sign(MLDSASize, MLDSAPrivateKey const&, ReadonlyBytes context, ReadonlyBytes message);
ErrorOr<bool> mldsa_verify(MLDSASize, MLDSAPublicKey const&, ReadonlyBytes context, ReadonlyBytes message, ReadonlyBytes signature);

}
