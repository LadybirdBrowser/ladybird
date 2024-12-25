/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Debug.h>
#include <AK/Random.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/ASN1/PEM.h>
#include <LibCrypto/Certificate/Certificate.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/PK/RSA.h>
#include <LibCrypto/SecureRandom.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>

namespace Crypto::PK {

ErrorOr<RSA::KeyPairType> RSA::parse_rsa_key(ReadonlyBytes der, bool is_private, Vector<StringView> current_scope)
{
    KeyPairType keypair;

    ASN1::Decoder decoder(der);

    if (is_private) {
        // RSAPrivateKey ::= SEQUENCE {
        //      version             Version,
        //      modulus             INTEGER,
        //      publicExponent      INTEGER,
        //      privateExponent     INTEGER,
        //      prime1              INTEGER,
        //      prime2              INTEGER,
        //      exponent1           INTEGER,
        //      exponent2           INTEGER,
        //      coefficient         INTEGER,
        //      otherPrimeInfos     OtherPrimeInfos OPTIONAL
        // }

        ENTER_TYPED_SCOPE(Sequence, "RSAPrivateKey"sv);

        PUSH_SCOPE("version");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, version);
        POP_SCOPE();
        if (version != 0) {
            ERROR_WITH_SCOPE(TRY(String::formatted("Invalid version value at {}", current_scope)));
        }

        PUSH_SCOPE("modulus");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, modulus);
        POP_SCOPE();

        PUSH_SCOPE("publicExponent");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, public_exponent);
        POP_SCOPE();

        PUSH_SCOPE("privateExponent");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, private_exponent);
        POP_SCOPE();

        PUSH_SCOPE("prime1");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, prime1);
        POP_SCOPE();

        PUSH_SCOPE("prime2");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, prime2);
        POP_SCOPE();

        PUSH_SCOPE("exponent1");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, exponent1);
        POP_SCOPE();

        PUSH_SCOPE("exponent2");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, exponent2);
        POP_SCOPE();

        PUSH_SCOPE("coefficient");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, coefficient);
        POP_SCOPE();

        keypair.private_key = {
            modulus,
            private_exponent,
            public_exponent,
            prime1,
            prime2,
            exponent1,
            exponent2,
            coefficient,
        };
        keypair.public_key = { modulus, public_exponent };

        EXIT_SCOPE();
        return keypair;
    } else {
        // RSAPublicKey ::= SEQUENCE {
        //      modulus         INTEGER,
        //      publicExponent  INTEGER
        // }

        ENTER_TYPED_SCOPE(Sequence, "RSAPublicKey"sv);

        PUSH_SCOPE("modulus");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, modulus);
        POP_SCOPE();

        PUSH_SCOPE("publicExponent");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, public_exponent);
        POP_SCOPE();

        keypair.public_key = { move(modulus), move(public_exponent) };

        EXIT_SCOPE();
        return keypair;
    }
}

ErrorOr<RSA::KeyPairType> RSA::generate_key_pair(size_t bits, IntegerType e)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)));

    OPENSSL_TRY(EVP_PKEY_keygen_init(ctx.ptr()));

    auto e_bn = TRY(unsigned_big_integer_to_openssl_bignum(e));

    auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

    OPENSSL_TRY(OSSL_PARAM_BLD_push_size_t(params_bld, OSSL_PKEY_PARAM_RSA_BITS, bits));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_BN(params_bld, OSSL_PKEY_PARAM_RSA_E, e_bn.ptr()));

    auto* params = OSSL_PARAM_BLD_to_param(params_bld);
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    OPENSSL_TRY(EVP_PKEY_CTX_set_params(ctx.ptr(), params));

    auto key = TRY(OpenSSL_PKEY::create());
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_generate(ctx.ptr(), &key_ptr));

#define OPENSSL_GET_KEY_PARAM(param, openssl_name)                                \
    auto param##_bn = TRY(OpenSSL_BN::create());                                  \
    auto* param##_bn_ptr = param##_bn.ptr();                                      \
    OPENSSL_TRY(EVP_PKEY_get_bn_param(key.ptr(), openssl_name, &param##_bn_ptr)); \
    auto param = TRY(openssl_bignum_to_unsigned_big_integer(param##_bn));

    OPENSSL_GET_KEY_PARAM(n, OSSL_PKEY_PARAM_RSA_N);
    OPENSSL_GET_KEY_PARAM(d, OSSL_PKEY_PARAM_RSA_D);
    OPENSSL_GET_KEY_PARAM(p, OSSL_PKEY_PARAM_RSA_FACTOR1);
    OPENSSL_GET_KEY_PARAM(q, OSSL_PKEY_PARAM_RSA_FACTOR2);
    OPENSSL_GET_KEY_PARAM(dp, OSSL_PKEY_PARAM_RSA_EXPONENT1);
    OPENSSL_GET_KEY_PARAM(dq, OSSL_PKEY_PARAM_RSA_EXPONENT2);
    OPENSSL_GET_KEY_PARAM(qinv, OSSL_PKEY_PARAM_RSA_COEFFICIENT1);

#undef OPENSSL_GET_KEY_PARAM

    RSAKeyPair<PublicKeyType, PrivateKeyType> keys {
        { n, e },
        { n, d, e, p, q, dp, dq, qinv }
    };
    return keys;
}

ErrorOr<ByteBuffer> RSA::encrypt(ReadonlyBytes in, Bytes& out)
{
    dbgln_if(CRYPTO_DEBUG, "in size: {}", in.size());
    auto in_integer = UnsignedBigInteger::import_data(in.data(), in.size());
    if (in_integer >= m_public_key.modulus())
        return Error::from_string_literal("Data too large for key");

    auto exp = NumberTheory::ModularPower(in_integer, m_public_key.public_exponent(), m_public_key.modulus());

    auto out = TRY(ByteBuffer::create_uninitialized(exp.byte_length()));
    auto size = exp.export_data(out);
    auto outsize = out.size();
    VERIFY(size == outsize);
    return out;
}

ErrorOr<ByteBuffer> RSA::decrypt(ReadonlyBytes in)
{
    auto in_integer = UnsignedBigInteger::import_data(in.data(), in.size());

    UnsignedBigInteger m;
    if (m_private_key.prime1().is_zero() || m_private_key.prime2().is_zero()) {
        m = NumberTheory::ModularPower(in_integer, m_private_key.private_exponent(), m_private_key.modulus());
    } else {
        auto m1 = NumberTheory::ModularPower(in_integer, m_private_key.exponent1(), m_private_key.prime1());
        auto m2 = NumberTheory::ModularPower(in_integer, m_private_key.exponent2(), m_private_key.prime2());
        while (m1 < m2)
            m1 = m1.plus(m_private_key.prime1());

        auto h = NumberTheory::Mod(m1.minus(m2).multiplied_by(m_private_key.coefficient()), m_private_key.prime1());
        m = m2.plus(h.multiplied_by(m_private_key.prime2()));
    }

    auto out = TRY(ByteBuffer::create_uninitialized(m.byte_length()));
    auto size = m.export_data(out);
    auto align = m_private_key.length();
    auto aligned_size = (size + align - 1) / align * align;

    for (auto i = size; i < aligned_size; ++i)
        out[out.size() - i - 1] = 0; // zero the non-aligned values
    return out.slice(out.size() - aligned_size, aligned_size);
}

ErrorOr<ByteBuffer> RSA::sign(ReadonlyBytes in)
{
    auto in_integer = UnsignedBigInteger::import_data(in.data(), in.size());
    auto exp = NumberTheory::ModularPower(in_integer, m_private_key.private_exponent(), m_private_key.modulus());

    auto out = TRY(ByteBuffer::create_uninitialized(exp.byte_length()));
    auto size = exp.export_data(out);
    return out.slice(out.size() - size, size);
}

ErrorOr<ByteBuffer> RSA::verify(ReadonlyBytes in)
{
    auto in_integer = UnsignedBigInteger::import_data(in.data(), in.size());
    auto exp = NumberTheory::ModularPower(in_integer, m_public_key.public_exponent(), m_public_key.modulus());

    auto out = TRY(ByteBuffer::create_uninitialized(exp.byte_length()));
    auto size = exp.export_data(out);
    return out.slice(out.size() - size, size);
}

void RSA::import_private_key(ReadonlyBytes bytes, bool pem)
{
    ByteBuffer decoded_bytes;
    if (pem) {
        auto decoded = decode_pem(bytes);
        if (decoded.type == PEMType::RSAPrivateKey) {
            decoded_bytes = decoded.data;
        } else if (decoded.type == PEMType::PrivateKey) {
            ASN1::Decoder decoder(decoded.data);
            auto maybe_key = Certificate::parse_private_key_info(decoder, {});
            if (maybe_key.is_error()) {
                dbgln("Failed to parse private key info: {}", maybe_key.error());
                VERIFY_NOT_REACHED();
            }

            m_private_key = maybe_key.release_value().rsa;
            return;
        } else {
            dbgln("Expected a PEM encoded private key");
            VERIFY_NOT_REACHED();
        }
    }

    auto maybe_key = parse_rsa_key(decoded_bytes, true, {});
    if (maybe_key.is_error()) {
        dbgln("Failed to parse RSA private key: {}", maybe_key.error());
        VERIFY_NOT_REACHED();
    }
    m_private_key = maybe_key.release_value().private_key;
}

void RSA::import_public_key(ReadonlyBytes bytes, bool pem)
{
    ByteBuffer decoded_bytes;
    if (pem) {
        auto decoded = decode_pem(bytes);
        if (decoded.type == PEMType::RSAPublicKey) {
            decoded_bytes = decoded.data;
        } else if (decoded.type == PEMType::PublicKey) {
            ASN1::Decoder decoder(decoded.data);
            auto maybe_key = Certificate::parse_subject_public_key_info(decoder, {});
            if (maybe_key.is_error()) {
                dbgln("Failed to parse subject public key info: {}", maybe_key.error());
                VERIFY_NOT_REACHED();
            }

            m_public_key = maybe_key.release_value().rsa;
            return;
        } else {
            dbgln("Expected a PEM encoded public key");
            VERIFY_NOT_REACHED();
        }
    }

    auto maybe_key = parse_rsa_key(decoded_bytes, false, {});
    if (maybe_key.is_error()) {
        dbgln("Failed to parse RSA public key: {}", maybe_key.error());
        VERIFY_NOT_REACHED();
    }
    m_public_key = maybe_key.release_value().public_key;
}

ErrorOr<ByteBuffer> RSA_PKCS1_EME::encrypt(ReadonlyBytes in)
{
    auto mod_len = (m_public_key.modulus().trimmed_length() * sizeof(u32) * 8 + 7) / 8;
    dbgln_if(CRYPTO_DEBUG, "key size: {}", mod_len);
    if (in.size() > mod_len - 11)
        return Error::from_string_literal("Message too long");

    auto out = TRY(ByteBuffer::create_uninitialized(mod_len));

    auto ps_length = mod_len - in.size() - 3;
    Vector<u8, 8096> ps;
    ps.resize(ps_length);

    fill_with_secure_random(ps);
    // since fill_with_random can create zeros (shocking!)
    // we have to go through and un-zero the zeros
    for (size_t i = 0; i < ps_length; ++i) {
        while (!ps[i])
            ps[i] = get_random<u8>();
    }

    u8 paddings[] { 0x00, 0x02 };

    out.overwrite(0, paddings, 2);
    out.overwrite(2, ps.data(), ps_length);
    out.overwrite(2 + ps_length, paddings, 1);
    out.overwrite(3 + ps_length, in.data(), in.size());
    out.trim(3 + ps_length + in.size(), true); // should be a single block

    dbgln_if(CRYPTO_DEBUG, "padded output size: {} buffer size: {}", 3 + ps_length + in.size(), out.size());

    return TRY(RSA::encrypt(out));
}

ErrorOr<ByteBuffer> RSA_PKCS1_EME::decrypt(ReadonlyBytes in)
{
    auto mod_len = (m_public_key.modulus().trimmed_length() * sizeof(u32) * 8 + 7) / 8;
    if (in.size() != mod_len)
        return Error::from_string_literal("Invalid input size");

    auto out = TRY(RSA::decrypt(in));

    if (out.size() < RSA::output_size())
        return Error::from_string_literal("Not enough data after decryption");

    if (out[0] != 0x00 || out[1] != 0x02)
        return Error::from_string_literal("Invalid padding");

    size_t offset = 2;
    while (offset < out.size() && out[offset])
        ++offset;

    if (offset == out.size())
        return Error::from_string_literal("Garbage data, no zero to split padding");

    ++offset;

    if (offset - 3 < 8)
        return Error::from_string_literal("PS too small");

    return out.slice(offset, out.size() - offset);;
}

ErrorOr<ByteBuffer> RSA_PKCS1_EME::sign(ReadonlyBytes)
{
    return Error::from_string_literal("FIXME: RSA_PKCS_EME::sign");
}

ErrorOr<ByteBuffer> RSA_PKCS1_EME::verify(ReadonlyBytes)
{
    return Error::from_string_literal("FIXME: RSA_PKCS_EME::verify");
}
}
