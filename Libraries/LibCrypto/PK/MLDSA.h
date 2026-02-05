/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibCrypto/PK/PK.h>

namespace Crypto::PK {

enum MLDSASize {
    MLDSA44,
    MLDSA65,
    MLDSA87,
};

class MLDSAPublicKey {
public:
    MLDSAPublicKey(ByteBuffer m_public_key)
        : m_public_key(move(m_public_key))
    {
    }

    MLDSAPublicKey() = default;

    ByteBuffer const& public_key() const { return m_public_key; }

private:
    ByteBuffer m_public_key;
};

class MLDSAPrivateKey {
public:
    MLDSAPrivateKey(ByteBuffer seed, ByteBuffer public_key, ByteBuffer private_key)
        : m_seed(move(seed))
        , m_public_key(move(public_key))
        , m_private_key(move(private_key))
    {
    }

    MLDSAPrivateKey() = default;

    ByteBuffer const& seed() const { return m_seed; }
    ByteBuffer const& public_key() const { return m_public_key; }
    ByteBuffer const& private_key() const { return m_private_key; }

    ErrorOr<ByteBuffer> export_as_der() const;

private:
    ByteBuffer m_seed;
    ByteBuffer m_public_key;
    ByteBuffer m_private_key;
};

template<typename PubKey, typename PrivKey>
struct MLDSAKeyPair {
    PubKey public_key;
    PrivKey private_key;
};

class MLDSA : public PKSystem<MLDSAPrivateKey, MLDSAPublicKey> {
public:
    using KeyPairType = MLDSAKeyPair<PublicKeyType, PrivateKeyType>;

    static ErrorOr<KeyPairType> parse_mldsa_key(MLDSASize, ReadonlyBytes der, Vector<StringView> current_scope);
    static ErrorOr<KeyPairType> generate_key_pair(MLDSASize, ByteBuffer seed = {});

    MLDSA(MLDSASize size, PrivateKeyType const& priv_key, ByteBuffer context)
        : m_size(size)
        , m_context(move(context))
    {
        m_private_key = priv_key;
        m_public_key = { priv_key.public_key() };
    }

    MLDSA(MLDSASize size, PublicKeyType const& pub_key, ByteBuffer context)
        : m_size(size)
        , m_context(move(context))
    {
        m_public_key = pub_key;
    }

    ErrorOr<ByteBuffer> sign(ReadonlyBytes message) override;

    ErrorOr<ByteBuffer> encrypt(ReadonlyBytes) override { return Error::from_string_literal("Operation not supported"); }
    ErrorOr<ByteBuffer> decrypt(ReadonlyBytes) override { return Error::from_string_literal("Operation not supported"); }
    ErrorOr<bool> verify(ReadonlyBytes, ReadonlyBytes) override;
    ByteString class_name() const override { return "ML-DSA"; }

private:
    MLDSASize m_size;
    ByteBuffer m_context;
};

}
