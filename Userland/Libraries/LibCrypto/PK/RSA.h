/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibCrypto/NumberTheory/ModularFunctions.h>
#include <LibCrypto/PK/PK.h>

namespace Crypto::PK {

template<typename Integer = UnsignedBigInteger>
class RSAPublicKey {
public:
    RSAPublicKey(Integer n, Integer e)
        : m_modulus(move(n))
        , m_public_exponent(move(e))
        , m_length(m_modulus.trimmed_length() * sizeof(u32))
    {
    }

    RSAPublicKey()
        : m_modulus(0)
        , m_public_exponent(0)
    {
    }

    Integer const& modulus() const { return m_modulus; }
    Integer const& public_exponent() const { return m_public_exponent; }
    size_t length() const { return m_length; }
    void set_length(size_t length) { m_length = length; }

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

    void set(Integer n, Integer e)
    {
        m_modulus = move(n);
        m_public_exponent = move(e);
        m_length = (m_modulus.trimmed_length() * sizeof(u32));
    }

private:
    Integer m_modulus;
    Integer m_public_exponent;
    size_t m_length { 0 };
};

template<typename Integer = UnsignedBigInteger>
class RSAPrivateKey {
public:
    RSAPrivateKey(Integer n, Integer d, Integer e, Integer p, Integer q)
        : m_modulus(move(n))
        , m_private_exponent(move(d))
        , m_public_exponent(move(e))
        , m_prime_1(move(p))
        , m_prime_2(move(q))
        , m_exponent_1(NumberTheory::Mod(m_private_exponent, m_prime_1.minus(1)))
        , m_exponent_2(NumberTheory::Mod(m_private_exponent, m_prime_2.minus(1)))
        , m_coefficient(NumberTheory::ModularInverse(m_prime_2, m_prime_1))
        , m_length(m_modulus.trimmed_length() * sizeof(u32))
    {
    }

    RSAPrivateKey(Integer n, Integer d, Integer e, Integer p, Integer q, Integer dp, Integer dq, Integer qinv)
        : m_modulus(move(n))
        , m_private_exponent(move(d))
        , m_public_exponent(move(e))
        , m_prime_1(move(p))
        , m_prime_2(move(q))
        , m_exponent_1(move(dp))
        , m_exponent_2(move(dq))
        , m_coefficient(move(qinv))
        , m_length(m_modulus.trimmed_length() * sizeof(u32))
    {
    }

    RSAPrivateKey() = default;

    static RSAPrivateKey from_crt(Integer n, Integer e, Integer p, Integer q, Integer dp, Integer dq, Integer qinv)
    {
        auto lambda = NumberTheory::LCM(p.minus(1), q.minus(1));
        auto d = NumberTheory::ModularInverse(e, lambda);

        return { n, d, e, p, q, dp, dq, qinv };
    }

    Integer const& modulus() const { return m_modulus; }
    Integer const& private_exponent() const { return m_private_exponent; }
    Integer const& public_exponent() const { return m_public_exponent; }
    Integer const& prime1() const { return m_prime_1; }
    Integer const& prime2() const { return m_prime_2; }
    Integer const& exponent1() const { return m_exponent_1; }
    Integer const& exponent2() const { return m_exponent_2; }
    Integer const& coefficient() const { return m_coefficient; }
    size_t length() const { return m_length; }

    ErrorOr<ByteBuffer> export_as_der() const
    {
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
    Integer m_modulus;
    Integer m_private_exponent;
    Integer m_public_exponent;
    Integer m_prime_1;
    Integer m_prime_2;
    Integer m_exponent_1;  // d mod (p-1)
    Integer m_exponent_2;  // d mod (q-1)
    Integer m_coefficient; // q^-1 mod p
    size_t m_length { 0 };
};

template<typename PubKey, typename PrivKey>
struct RSAKeyPair {
    PubKey public_key;
    PrivKey private_key;
};

using IntegerType = UnsignedBigInteger;
class RSA : public PKSystem<RSAPrivateKey<IntegerType>, RSAPublicKey<IntegerType>> {
public:
    using KeyPairType = RSAKeyPair<PublicKeyType, PrivateKeyType>;

    static KeyPairType parse_rsa_key(ReadonlyBytes der);
    static KeyPairType generate_key_pair(size_t bits = 256, IntegerType e = 65537)
    {
        IntegerType p;
        IntegerType q;
        IntegerType lambda;

        do {
            p = NumberTheory::random_big_prime(bits / 2);
            q = NumberTheory::random_big_prime(bits / 2);
            lambda = NumberTheory::LCM(p.minus(1), q.minus(1));
            dbgln("checking combination p={}, q={}, lambda={}", p, q, lambda.length());
        } while (!(NumberTheory::GCD(e, lambda) == 1));

        auto n = p.multiplied_by(q);

        auto d = NumberTheory::ModularInverse(e, lambda);
        dbgln("Your keys are Pub(n={}, e={}) and Priv(n={}, d={}, p={}, q={})", n, e, n, d, p, q);
        RSAKeyPair<PublicKeyType, PrivateKeyType> keys {
            { n, e },
            { n, d, e, p, q }
        };
        return keys;
    }

    RSA(IntegerType n, IntegerType d, IntegerType e)
    {
        m_public_key.set(n, e);
        m_private_key = { n, d, e, 0, 0, 0, 0, 0 };
    }

    RSA(PublicKeyType& pubkey, PrivateKeyType& privkey)
        : PKSystem<RSAPrivateKey<IntegerType>, RSAPublicKey<IntegerType>>(pubkey, privkey)
    {
    }

    RSA(ByteBuffer const& publicKeyPEM, ByteBuffer const& privateKeyPEM)
    {
        import_public_key(publicKeyPEM);
        import_private_key(privateKeyPEM);
    }

    RSA(StringView privKeyPEM)
    {
        import_private_key(privKeyPEM.bytes());
        m_public_key.set(m_private_key.modulus(), m_private_key.public_exponent());
    }

    // create our own keys
    RSA()
    {
        auto pair = generate_key_pair();
        m_public_key = pair.public_key;
        m_private_key = pair.private_key;
    }

    virtual void encrypt(ReadonlyBytes in, Bytes& out) override;
    virtual void decrypt(ReadonlyBytes in, Bytes& out) override;

    virtual void sign(ReadonlyBytes in, Bytes& out) override;
    virtual void verify(ReadonlyBytes in, Bytes& out) override;

    virtual ByteString class_name() const override
    {
        return "RSA";
    }

    virtual size_t output_size() const override
    {
        return m_public_key.length();
    }

    void import_public_key(ReadonlyBytes, bool pem = true);
    void import_private_key(ReadonlyBytes, bool pem = true);

    PrivateKeyType const& private_key() const { return m_private_key; }
    PublicKeyType const& public_key() const { return m_public_key; }
};

class RSA_PKCS1_EME : public RSA {
public:
    // forward all constructions to RSA
    template<typename... Args>
    RSA_PKCS1_EME(Args... args)
        : RSA(args...)
    {
    }

    ~RSA_PKCS1_EME() = default;

    virtual void encrypt(ReadonlyBytes in, Bytes& out) override;
    virtual void decrypt(ReadonlyBytes in, Bytes& out) override;

    virtual void sign(ReadonlyBytes, Bytes&) override;
    virtual void verify(ReadonlyBytes, Bytes&) override;

    virtual ByteString class_name() const override
    {
        return "RSA_PKCS1-EME";
    }

    virtual size_t output_size() const override
    {
        return m_public_key.length();
    }
};
}
