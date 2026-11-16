/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibCrypto/OpenSSL.h>

// NB: RSA key components are big-endian unsigned integers.
namespace Crypto::PK {

class RSAPublicKey {
public:
    RSAPublicKey() = default;

    static ErrorOr<RSAPublicKey> from_components(ReadonlyBytes modulus, ReadonlyBytes public_exponent);

    size_t length() const { return m_length; }
    size_t modulus_bits() const;

    ErrorOr<ByteBuffer> modulus() const;
    ErrorOr<ByteBuffer> public_exponent() const;

    ErrorOr<bool> is_valid() const;

    ErrorOr<ByteBuffer> export_as_der() const;

    EVP_PKEY* openssl_key() const { return m_key.ptr(); }

private:
    RSAPublicKey(OpenSSL_SharedPKEY key);

    OpenSSL_SharedPKEY m_key;
    size_t m_length { 0 };
};

struct RSACRTComponents {
    ReadonlyBytes prime1;
    ReadonlyBytes prime2;
    ReadonlyBytes exponent1;
    ReadonlyBytes exponent2;
    ReadonlyBytes coefficient;
};

class RSAPrivateKey {
public:
    // NB: The order matches libcrypto_rust's RsaJwkInteger.
    enum class Component : u8 {
        Modulus,
        PublicExponent,
        PrivateExponent,
        Prime1,
        Prime2,
        Exponent1,
        Exponent2,
        Coefficient,
    };

    RSAPrivateKey() = default;

    static ErrorOr<RSAPrivateKey> from_components(ReadonlyBytes modulus, ReadonlyBytes public_exponent, ReadonlyBytes private_exponent, Optional<RSACRTComponents>);

    size_t length() const { return m_length; }
    size_t modulus_bits() const;
    bool has_crt_components() const { return m_has_crt_components; }

    // A CRT component of a key without them is empty.
    ErrorOr<ByteBuffer> component(Component) const;

    ErrorOr<RSAPublicKey> public_key() const;

    ErrorOr<bool> is_valid() const;

    ErrorOr<ByteBuffer> export_as_der() const;

    EVP_PKEY* openssl_key() const { return m_key.ptr(); }

private:
    friend class RSA;

    RSAPrivateKey(OpenSSL_SharedPKEY key, bool has_crt_components);

    OpenSSL_SharedPKEY m_key;
    size_t m_length { 0 };
    bool m_has_crt_components { false };
};

struct RSAKeyPair {
    RSAPublicKey public_key;
    RSAPrivateKey private_key;
};

class RSA {
public:
    using KeyPairType = RSAKeyPair;

    static ErrorOr<KeyPairType> parse_rsa_key(ReadonlyBytes der, bool is_private, Vector<StringView> current_scope);
    static ErrorOr<void> validate_key_generation_parameters(size_t bits);
    static ErrorOr<KeyPairType> generate_key_pair(size_t bits, ReadonlyBytes public_exponent);
    static ErrorOr<KeyPairType> generate_key_pair(size_t bits);
};

ErrorOr<ByteBuffer> rsa_oaep_encrypt(RSAPublicKey const&, Hash::HashKind, ReadonlyBytes label, ReadonlyBytes plaintext);
ErrorOr<ByteBuffer> rsa_oaep_decrypt(RSAPrivateKey const&, Hash::HashKind, ReadonlyBytes label, ReadonlyBytes ciphertext);

ErrorOr<ByteBuffer> rsa_pss_sign(RSAPrivateKey const&, Hash::HashKind, u32 salt_length, ReadonlyBytes message);
ErrorOr<bool> rsa_pss_verify(RSAPublicKey const&, Hash::HashKind, u32 salt_length, ReadonlyBytes message, ReadonlyBytes signature);

ErrorOr<ByteBuffer> rsa_pkcs1_sign(RSAPrivateKey const&, Hash::HashKind, ReadonlyBytes message);
ErrorOr<bool> rsa_pkcs1_verify(RSAPublicKey const&, Hash::HashKind, ReadonlyBytes message, ReadonlyBytes signature);

}
