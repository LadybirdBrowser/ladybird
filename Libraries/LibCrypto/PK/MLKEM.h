/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>

namespace Crypto::PK {

enum class MLKEMSize {
    MLKEM512,
    MLKEM768,
    MLKEM1024,
};

class MLKEMPublicKey {
public:
    explicit MLKEMPublicKey(ByteBuffer m_public_key)
        : m_public_key(move(m_public_key))
    {
    }

    MLKEMPublicKey() = default;

    ByteBuffer public_key() const { return m_public_key; }

private:
    ByteBuffer m_public_key;
};

class MLKEMPrivateKey {
public:
    MLKEMPrivateKey(ByteBuffer seed, ByteBuffer public_key, ByteBuffer private_key)
        : m_seed(move(seed))
        , m_public_key(move(public_key))
        , m_private_key(move(private_key))
    {
    }

    MLKEMPrivateKey() = default;

private:
    ByteBuffer m_seed;
    ByteBuffer m_public_key;
    ByteBuffer m_private_key;
};

template<typename PubKey, typename PrivKey>
struct MLKEMKeyPair {
    PubKey public_key;
    PrivKey private_key;
};

struct MLKEMEncapsulation {
    ByteBuffer shared_key;
    ByteBuffer ciphertext;
};

class MLKEM {
    using PublicKeyType = MLKEMPublicKey;
    using PrivateKeyType = MLKEMPrivateKey;

public:
    using KeyPairType = MLKEMKeyPair<PublicKeyType, PrivateKeyType>;

    static ErrorOr<MLKEMEncapsulation> encapsulate(MLKEMSize size, MLKEMPublicKey const& key);
    static ErrorOr<KeyPairType> generate_key_pair(MLKEMSize size, ByteBuffer seed = {});
};

};
