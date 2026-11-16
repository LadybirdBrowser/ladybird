/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/PK/PK.h>

namespace Crypto::PK {

class RSAPublicKey {
public:
    RSAPublicKey(UnsignedBigInteger n, UnsignedBigInteger e)
        : m_modulus(move(n))
        , m_public_exponent(move(e))
        , m_length(m_modulus.byte_length())
    {
    }

    RSAPublicKey()
        : m_modulus(0)
        , m_public_exponent(0)
    {
    }

    UnsignedBigInteger const& modulus() const { return m_modulus; }
    UnsignedBigInteger const& public_exponent() const { return m_public_exponent; }
    size_t length() const { return m_length; }

    ErrorOr<bool> is_valid() const;

    ErrorOr<ByteBuffer> export_as_der() const
    {
        ASN1::Encoder encoder;
        TRY(encoder.write_constructed(ASN1::Class::Universal, ASN1::Kind::Sequence, [&]() -> ErrorOr<void> {
            TRY(encoder.write(m_modulus));
            TRY(encoder.write(m_public_exponent));
            return {};
        }));

        return encoder.finish();
    }

private:
    UnsignedBigInteger m_modulus;
    UnsignedBigInteger m_public_exponent;
    size_t m_length { 0 };
};

class RSAPrivateKey {
public:
    RSAPrivateKey(UnsignedBigInteger n, UnsignedBigInteger d, UnsignedBigInteger e)
        : m_modulus(move(n))
        , m_private_exponent(move(d))
        , m_public_exponent(move(e))
        , m_length(m_modulus.byte_length())
    {
    }

    RSAPrivateKey(UnsignedBigInteger n, UnsignedBigInteger d, UnsignedBigInteger e, UnsignedBigInteger p, UnsignedBigInteger q, UnsignedBigInteger dp, UnsignedBigInteger dq, UnsignedBigInteger qinv)
        : m_modulus(move(n))
        , m_private_exponent(move(d))
        , m_public_exponent(move(e))
        , m_prime_1(move(p))
        , m_prime_2(move(q))
        , m_exponent_1(move(dp))
        , m_exponent_2(move(dq))
        , m_coefficient(move(qinv))
        , m_length(m_modulus.byte_length())
    {
    }

    RSAPrivateKey() = default;

    UnsignedBigInteger const& modulus() const { return m_modulus; }
    void clear_sensitive_data();

    UnsignedBigInteger const& private_exponent() const { return m_private_exponent; }
    UnsignedBigInteger const& public_exponent() const { return m_public_exponent; }
    UnsignedBigInteger const& prime1() const { return m_prime_1; }
    UnsignedBigInteger const& prime2() const { return m_prime_2; }
    UnsignedBigInteger const& exponent1() const { return m_exponent_1; }
    UnsignedBigInteger const& exponent2() const { return m_exponent_2; }
    UnsignedBigInteger const& coefficient() const { return m_coefficient; }
    size_t length() const { return m_length; }

    ErrorOr<bool> is_valid() const;

    ErrorOr<ByteBuffer> export_as_der() const
    {
        if (m_prime_1.is_zero() || m_prime_2.is_zero()) {
            return Error::from_string_literal("Cannot export private key without prime factors");
        }

        ASN1::Encoder encoder;
        TRY(encoder.write_constructed(ASN1::Class::Universal, ASN1::Kind::Sequence, [&]() -> ErrorOr<void> {
            TRY(encoder.write(0x00u)); // version
            TRY(encoder.write(m_modulus));
            TRY(encoder.write(m_public_exponent));
            TRY(encoder.write(m_private_exponent));
            TRY(encoder.write(m_prime_1));
            TRY(encoder.write(m_prime_2));
            TRY(encoder.write(m_exponent_1));
            TRY(encoder.write(m_exponent_2));
            TRY(encoder.write(m_coefficient));
            return {};
        }));

        return encoder.finish();
    }

private:
    UnsignedBigInteger m_modulus;
    UnsignedBigInteger m_private_exponent;
    UnsignedBigInteger m_public_exponent;
    UnsignedBigInteger m_prime_1;
    UnsignedBigInteger m_prime_2;
    UnsignedBigInteger m_exponent_1;  // d mod (p-1)
    UnsignedBigInteger m_exponent_2;  // d mod (q-1)
    UnsignedBigInteger m_coefficient; // q^-1 mod p
    size_t m_length { 0 };
};

template<typename PubKey, typename PrivKey>
struct RSAKeyPair {
    PubKey public_key;
    PrivKey private_key;
};

class RSA {
public:
    using KeyPairType = RSAKeyPair<RSAPublicKey, RSAPrivateKey>;

    static ErrorOr<KeyPairType> parse_rsa_key(ReadonlyBytes der, bool is_private, Vector<StringView> current_scope);
    static ErrorOr<void> validate_key_generation_parameters(size_t bits);
    static ErrorOr<KeyPairType> generate_key_pair(size_t bits, UnsignedBigInteger e = 65537);
};

ErrorOr<ByteBuffer> rsa_oaep_encrypt(RSAPublicKey const&, Hash::HashKind, ReadonlyBytes label, ReadonlyBytes plaintext);
ErrorOr<ByteBuffer> rsa_oaep_decrypt(RSAPrivateKey const&, Hash::HashKind, ReadonlyBytes label, ReadonlyBytes ciphertext);

ErrorOr<ByteBuffer> rsa_pss_sign(RSAPrivateKey const&, Hash::HashKind, u32 salt_length, ReadonlyBytes message);
ErrorOr<bool> rsa_pss_verify(RSAPublicKey const&, Hash::HashKind, u32 salt_length, ReadonlyBytes message, ReadonlyBytes signature);

ErrorOr<ByteBuffer> rsa_pkcs1_sign(RSAPrivateKey const&, Hash::HashKind, ReadonlyBytes message);
ErrorOr<bool> rsa_pkcs1_verify(RSAPublicKey const&, Hash::HashKind, ReadonlyBytes message, ReadonlyBytes signature);

}
