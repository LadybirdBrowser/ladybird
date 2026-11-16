/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <LibCrypto/OpenSSL.h>

namespace Crypto::PK {

enum class MLKEMSize {
    MLKEM512,
    MLKEM768,
    MLKEM1024,
};

class MLKEMPublicKey {
public:
    MLKEMPublicKey() = default;

    // NB: Any bytes make a key. Web Crypto only runs the encapsulation key check of FIPS 203, section 7.2, when the key
    //     is used, so its result is kept for then.
    static ErrorOr<MLKEMPublicKey> from_bytes(ReadonlyBytes);

    // The parameter set that the key passes the encapsulation key check for, if any.
    Optional<MLKEMSize> size() const { return m_size; }
    ByteBuffer const& public_key() const { return m_public_key; }

    // NB: A key that fails the encapsulation key check has none.
    EVP_PKEY* openssl_key() const { return m_key.ptr(); }

private:
    OpenSSL_SharedPKEY m_key;
    ByteBuffer m_public_key;
    Optional<MLKEMSize> m_size;
};

class MLKEMPrivateKey {
public:
    MLKEMPrivateKey() = default;

    // An empty seed generates a new key.
    static ErrorOr<MLKEMPrivateKey> generate(MLKEMSize, ReadonlyBytes seed);

    // A key that was given in the expandedKey form, which Web Crypto rejects once it has parsed it. It has no seed,
    // public key or OpenSSL key, since OpenSSL would refuse an expanded key that is not valid.
    static MLKEMPrivateKey in_expanded_key_form(MLKEMSize);

    MLKEMSize size() const { return m_size; }
    static constexpr size_t seed_size = 64;

    bool has_seed() const { return m_key.ptr(); }
    // NB: OpenSSL keeps the seed, so that no copy of it is left behind when a key is moved or replaced. It is written
    //     into the caller's buffer, which the caller clears.
    ErrorOr<ReadonlyBytes> seed(Bytes buffer) const;
    ByteBuffer const& public_key() const { return m_public_key; }
    ErrorOr<ByteBuffer> expanded_key() const;

    ErrorOr<ByteBuffer> export_as_der() const;

    EVP_PKEY* openssl_key() const { return m_key.ptr(); }

private:
    MLKEMPrivateKey(OpenSSL_SharedPKEY key, ByteBuffer public_key, MLKEMSize size)
        : m_key(move(key))
        , m_public_key(move(public_key))
        , m_size(size)
    {
    }

    OpenSSL_SharedPKEY m_key;
    ByteBuffer m_public_key;
    MLKEMSize m_size { MLKEMSize::MLKEM512 };
};

struct MLKEMKeyPair {
    MLKEMPublicKey public_key;
    MLKEMPrivateKey private_key;
};

struct MLKEMEncapsulation {
    ByteBuffer shared_key;
    ByteBuffer ciphertext;
};

class MLKEM {
public:
    using KeyPairType = MLKEMKeyPair;

    static ErrorOr<KeyPairType> parse_mlkem_key(MLKEMSize, ReadonlyBytes der);
    static ErrorOr<KeyPairType> generate_key_pair(MLKEMSize, ReadonlyBytes seed = {});

    static ErrorOr<MLKEMEncapsulation> encapsulate(MLKEMSize, MLKEMPublicKey const&);
    static ErrorOr<ByteBuffer> decapsulate(MLKEMSize, MLKEMPrivateKey const&, ReadonlyBytes ciphertext);
};

}
