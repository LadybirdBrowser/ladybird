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

    static ErrorOr<KeyPairType> generate_key_pair(MLDSASize, ByteBuffer seed = {});
};

}
