/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/NumericLimits.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Certificate/Certificate.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/PK/RSA.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>

namespace Crypto::PK {

void RSAPrivateKey::clear_sensitive_data()
{
    m_private_exponent.clear_sensitive_data();
    m_prime_1.clear_sensitive_data();
    m_prime_2.clear_sensitive_data();
    m_exponent_1.clear_sensitive_data();
    m_exponent_2.clear_sensitive_data();
    m_coefficient.clear_sensitive_data();
}

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

        ENTER_TYPED_SCOPE(Sequence, "RSAPrivateKey");

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

        ENTER_TYPED_SCOPE(Sequence, "RSAPublicKey");

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

ErrorOr<void> RSA::validate_key_generation_parameters(size_t bits)
{
    // OpenSSL refuses to generate keys with shorter moduli.
    static constexpr size_t minimum_modulus_length = 512;

    if (bits < minimum_modulus_length)
        return Error::from_string_literal("RSA modulus length must be at least 512 bits");

    if (bits > OPENSSL_RSA_MAX_MODULUS_BITS)
        return Error::from_string_literal("RSA modulus length must be at most 16384 bits");

    return {};
}

ErrorOr<RSA::KeyPairType> RSA::generate_key_pair(size_t bits, UnsignedBigInteger e)
{
    TRY(validate_key_generation_parameters(bits));

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

    KeyPairType keys {
        { n, e },
        { n, d, e, p, q, dp, dq, qinv }
    };
    return keys;
}

#define OPENSSL_SET_KEY_PARAM_NOT_ZERO(param, openssl_name, value)                       \
    auto param##_bn = TRY(unsigned_big_integer_to_openssl_bignum(value));                \
    if (!value.is_zero()) {                                                              \
        OPENSSL_TRY(OSSL_PARAM_BLD_push_BN(params_bld, openssl_name, param##_bn.ptr())); \
    }

static ErrorOr<OpenSSL_PKEY> public_key_to_openssl_pkey(RSAPublicKey const& public_key)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)));

    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx.ptr()));

    auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

    OPENSSL_SET_KEY_PARAM_NOT_ZERO(n, OSSL_PKEY_PARAM_RSA_N, public_key.modulus());
    OPENSSL_SET_KEY_PARAM_NOT_ZERO(e, OSSL_PKEY_PARAM_RSA_E, public_key.public_exponent());

    auto* params = OSSL_PARAM_BLD_to_param(params_bld);
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto key = TRY(OpenSSL_PKEY::create());
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx.ptr(), &key_ptr, EVP_PKEY_PUBLIC_KEY, params));
    return key;
}

static ErrorOr<OpenSSL_PKEY> private_key_to_openssl_pkey(RSAPrivateKey const& private_key)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)));

    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx.ptr()));

    auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

    OPENSSL_SET_KEY_PARAM_NOT_ZERO(n, OSSL_PKEY_PARAM_RSA_N, private_key.modulus());
    OPENSSL_SET_KEY_PARAM_NOT_ZERO(e, OSSL_PKEY_PARAM_RSA_E, private_key.public_exponent());
    OPENSSL_SET_KEY_PARAM_NOT_ZERO(d, OSSL_PKEY_PARAM_RSA_D, private_key.private_exponent());
    OPENSSL_SET_KEY_PARAM_NOT_ZERO(p, OSSL_PKEY_PARAM_RSA_FACTOR1, private_key.prime1());
    OPENSSL_SET_KEY_PARAM_NOT_ZERO(q, OSSL_PKEY_PARAM_RSA_FACTOR2, private_key.prime2());
    OPENSSL_SET_KEY_PARAM_NOT_ZERO(dp, OSSL_PKEY_PARAM_RSA_EXPONENT1, private_key.exponent1());
    OPENSSL_SET_KEY_PARAM_NOT_ZERO(dq, OSSL_PKEY_PARAM_RSA_EXPONENT2, private_key.exponent2());
    OPENSSL_SET_KEY_PARAM_NOT_ZERO(qinv, OSSL_PKEY_PARAM_RSA_COEFFICIENT1, private_key.coefficient());

    auto* params = OSSL_PARAM_BLD_to_param(params_bld);
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto key = TRY(OpenSSL_PKEY::create());
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx.ptr(), &key_ptr, EVP_PKEY_KEYPAIR, params));
    return key;
}

#undef OPENSSL_SET_KEY_PARAM_NOT_ZERO

// https://www.rfc-editor.org/rfc/rfc3447.html#section-3.1
ErrorOr<bool> RSAPublicKey::is_valid() const
{
    // In a valid RSA public key, the RSA modulus n is a product of u
    // distinct odd primes r_i, i = 1, 2, ..., u, where u >= 2, and the RSA
    // public exponent e is an integer between 3 and n - 1 satisfying GCD(e,
    // \lambda(n)) = 1, where \lambda(n) = LCM(r_1 - 1, ..., r_u - 1).

    if (!m_public_exponent.is_odd())
        return false;

    if (m_public_exponent < 3 || m_public_exponent >= m_modulus)
        return false;

    return true;
}

// https://www.rfc-editor.org/rfc/rfc3447.html#section-3.2
ErrorOr<bool> RSAPrivateKey::is_valid() const
{
    if (!m_public_exponent.is_odd())
        return false;

    if (m_public_exponent < 3 || m_public_exponent >= m_modulus)
        return false;

    if (!m_prime_1.is_zero() && !m_prime_2.is_zero() && !m_exponent_1.is_zero() && !m_exponent_2.is_zero() && !m_coefficient.is_zero()) {
        // In a valid RSA private key with the second representation, the two
        // factors p and q are the first two prime factors of the RSA modulus n
        // (i.e., r_1 and r_2), the CRT exponents dP and dQ are positive
        // integers less than p and q respectively satisfying
        //   e * dP == 1 (mod (p-1))
        //   e * dQ == 1 (mod (q-1)) ,
        // and the CRT coefficient qInv is a positive integer less than p
        // satisfying
        //   q * qInv == 1 (mod p).
        // If u > 2, the representation will include one or more triplets (r_i,
        // d_i, t_i), i = 3, ..., u.  The factors r_i are the additional prime
        // factors of the RSA modulus n.  Each CRT exponent d_i (i = 3, ..., u)
        // satisfies
        //   e * d_i == 1 (mod (r_i - 1)).
        // Each CRT coefficient t_i (i = 3, ..., u) is a positive integer less
        // than r_i satisfying
        //   R_i * t_i == 1 (mod r_i) ,
        // where R_i = r_1 * r_2 * ... * r_(i-1).

        if (m_exponent_1 >= m_prime_1 || m_exponent_2 >= m_prime_2 || m_coefficient >= m_prime_1)
            return false;

        if (m_prime_1.multiplied_by(m_prime_2) != m_modulus)
            return false;

        auto tmp_bn = TRY(OpenSSL_BN::create());

        auto e = TRY(unsigned_big_integer_to_openssl_bignum(m_public_exponent)),
             p = TRY(unsigned_big_integer_to_openssl_bignum(m_prime_1)),
             q = TRY(unsigned_big_integer_to_openssl_bignum(m_prime_2));

        auto dp = TRY(unsigned_big_integer_to_openssl_bignum(m_exponent_1)),
             dq = TRY(unsigned_big_integer_to_openssl_bignum(m_exponent_2));

        auto* bn_ctx = OPENSSL_TRY_PTR(BN_CTX_new());
        ScopeGuard const free_bn_ctx = [&] { BN_CTX_free(bn_ctx); };

        auto p1 = TRY(OpenSSL_BN::create());
        OPENSSL_TRY(BN_sub(p1.ptr(), p.ptr(), BN_value_one()));

        OPENSSL_TRY(BN_mod_mul(tmp_bn.ptr(), e.ptr(), dp.ptr(), p1.ptr(), bn_ctx));
        if (!BN_is_one(tmp_bn.ptr()))
            return false;

        auto q1 = TRY(OpenSSL_BN::create());
        OPENSSL_TRY(BN_sub(q1.ptr(), q.ptr(), BN_value_one()));

        OPENSSL_TRY(BN_mod_mul(tmp_bn.ptr(), e.ptr(), dq.ptr(), q1.ptr(), bn_ctx));
        if (!BN_is_one(tmp_bn.ptr()))
            return false;

        auto q_inv = TRY(unsigned_big_integer_to_openssl_bignum(m_coefficient));
        OPENSSL_TRY(BN_mod_mul(tmp_bn.ptr(), q.ptr(), q_inv.ptr(), p.ptr(), bn_ctx));
        if (!BN_is_one(tmp_bn.ptr()))
            return false;

        if (!m_private_exponent.is_zero()) {
            if (m_private_exponent >= m_modulus)
                return false;

            auto lambda = TRY(m_prime_1.minus(1)).lcm(TRY(m_prime_2.minus(1)));
            auto lambda_bn = TRY(unsigned_big_integer_to_openssl_bignum(lambda));

            auto d = TRY(unsigned_big_integer_to_openssl_bignum(m_private_exponent));

            OPENSSL_TRY(BN_mod_mul(tmp_bn.ptr(), d.ptr(), e.ptr(), lambda_bn.ptr(), bn_ctx));
            if (!BN_is_one(tmp_bn.ptr()))
                return false;
        }

        return true;
    }

    if (!m_modulus.is_zero() && !m_private_exponent.is_zero()) {
        // In a valid RSA private key with the first representation, the RSA
        // modulus n is the same as in the corresponding RSA public key and is
        // the product of u distinct odd primes r_i, i = 1, 2, ..., u, where u
        // >= 2.  The RSA private exponent d is a positive integer less than n
        // satisfying
        //   e * d == 1 (mod \lambda(n)),
        // where e is the corresponding RSA public exponent and \lambda(n) is
        // defined as in Section 3.1.

        if (m_private_exponent >= m_modulus)
            return false;

        return true;
    }

    return false;
}

static ErrorOr<Array<OSSL_PARAM, 5>> oaep_parameters(Hash::HashKind hash_kind, ReadonlyBytes const& label)
{
    auto* hash_name = const_cast<char*>(TRY(Hash::openssl_digest_name(hash_kind)));
    Array<OSSL_PARAM, 5> params {
        OSSL_PARAM_construct_utf8_string(OSSL_ASYM_CIPHER_PARAM_PAD_MODE, const_cast<char*>(OSSL_PKEY_RSA_PAD_MODE_OAEP), 0),
        OSSL_PARAM_construct_utf8_string(OSSL_ASYM_CIPHER_PARAM_OAEP_DIGEST, hash_name, 0),
        OSSL_PARAM_construct_utf8_string(OSSL_ASYM_CIPHER_PARAM_MGF1_DIGEST, hash_name, 0),
        OSSL_PARAM_construct_end(),
        OSSL_PARAM_construct_end(),
    };
    // NB: OpenSSL copies the label.
    if (!label.is_empty())
        params[3] = OSSL_PARAM_construct_octet_string(OSSL_ASYM_CIPHER_PARAM_OAEP_LABEL, const_cast<u8*>(label.data()), label.size());
    return params;
}

ErrorOr<ByteBuffer> rsa_oaep_encrypt(RSAPublicKey const& public_key, Hash::HashKind hash_kind, ReadonlyBytes label, ReadonlyBytes plaintext)
{
    auto key = TRY(public_key_to_openssl_pkey(public_key));
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key.ptr(), nullptr)));

    auto params = TRY(oaep_parameters(hash_kind, label));
    OPENSSL_TRY(EVP_PKEY_encrypt_init_ex(ctx.ptr(), params.data()));

    size_t out_size = 0;
    OPENSSL_TRY(EVP_PKEY_encrypt(ctx.ptr(), nullptr, &out_size, plaintext.data(), plaintext.size()));

    auto out = TRY(ByteBuffer::create_uninitialized(out_size));
    OPENSSL_TRY(EVP_PKEY_encrypt(ctx.ptr(), out.data(), &out_size, plaintext.data(), plaintext.size()));
    return out.slice(0, out_size);
}

ErrorOr<ByteBuffer> rsa_oaep_decrypt(RSAPrivateKey const& private_key, Hash::HashKind hash_kind, ReadonlyBytes label, ReadonlyBytes ciphertext)
{
    if (ciphertext.size() != private_key.length())
        return Error::from_string_literal("RSA ciphertext has an invalid length");

    auto key = TRY(private_key_to_openssl_pkey(private_key));
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key.ptr(), nullptr)));

    auto params = TRY(oaep_parameters(hash_kind, label));
    OPENSSL_TRY(EVP_PKEY_decrypt_init_ex(ctx.ptr(), params.data()));

    size_t out_size = 0;
    OPENSSL_TRY(EVP_PKEY_decrypt(ctx.ptr(), nullptr, &out_size, ciphertext.data(), ciphertext.size()));

    auto out = TRY(ByteBuffer::create_uninitialized(out_size));
    OPENSSL_TRY(EVP_PKEY_decrypt(ctx.ptr(), out.data(), &out_size, ciphertext.data(), ciphertext.size()));
    return out.slice(0, out_size);
}

static ErrorOr<ByteBuffer> sign(RSAPrivateKey const& private_key, Hash::HashKind hash_kind, OSSL_PARAM const* params, ReadonlyBytes message)
{
    auto key = TRY(private_key_to_openssl_pkey(private_key));
    auto ctx = TRY(OpenSSL_MD_CTX::create());

    OPENSSL_TRY(EVP_DigestSignInit_ex(ctx.ptr(), nullptr, TRY(Hash::openssl_digest_name(hash_kind)), nullptr, nullptr, key.ptr(), params));

    size_t signature_size = 0;
    OPENSSL_TRY(EVP_DigestSign(ctx.ptr(), nullptr, &signature_size, message.data(), message.size()));

    auto signature = TRY(ByteBuffer::create_uninitialized(signature_size));
    OPENSSL_TRY(EVP_DigestSign(ctx.ptr(), signature.data(), &signature_size, message.data(), message.size()));
    return signature.slice(0, signature_size);
}

static ErrorOr<bool> verify(RSAPublicKey const& public_key, Hash::HashKind hash_kind, OSSL_PARAM const* params, ReadonlyBytes message, ReadonlyBytes signature)
{
    if (!TRY(public_key.is_valid()))
        return false;

    if (signature.size() != public_key.length())
        return false;

    auto key = TRY(public_key_to_openssl_pkey(public_key));
    auto ctx = TRY(OpenSSL_MD_CTX::create());

    OPENSSL_TRY(EVP_DigestVerifyInit_ex(ctx.ptr(), nullptr, TRY(Hash::openssl_digest_name(hash_kind)), nullptr, nullptr, key.ptr(), params));

    auto result = EVP_DigestVerify(ctx.ptr(), signature.data(), signature.size(), message.data(), message.size());
    if (result == 1)
        return true;
    if (result == 0)
        return false;
    OPENSSL_TRY(result);
    VERIFY_NOT_REACHED();
}

// NB: OpenSSL takes the salt length as an int and gives negative values special meanings, such as salt length
//     auto-detection, so a length that does not fit in an int must never reach it. No RSA key is large enough
//     for such a salt.
static bool salt_length_fits_in_int(u32 salt_length)
{
    return salt_length <= static_cast<u32>(NumericLimits<int>::max());
}

static ErrorOr<Array<OSSL_PARAM, 4>> pss_parameters(Hash::HashKind hash_kind, int& salt_length)
{
    return Array<OSSL_PARAM, 4> {
        OSSL_PARAM_construct_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, const_cast<char*>(OSSL_PKEY_RSA_PAD_MODE_PSS), 0),
        OSSL_PARAM_construct_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_DIGEST, const_cast<char*>(TRY(Hash::openssl_digest_name(hash_kind))), 0),
        OSSL_PARAM_construct_int(OSSL_SIGNATURE_PARAM_PSS_SALTLEN, &salt_length),
        OSSL_PARAM_construct_end(),
    };
}

ErrorOr<ByteBuffer> rsa_pss_sign(RSAPrivateKey const& private_key, Hash::HashKind hash_kind, u32 salt_length, ReadonlyBytes message)
{
    if (!salt_length_fits_in_int(salt_length))
        return Error::from_string_literal("Salt length is too large");

    auto salt_length_int = static_cast<int>(salt_length);
    auto params = TRY(pss_parameters(hash_kind, salt_length_int));
    return sign(private_key, hash_kind, params.data(), message);
}

ErrorOr<bool> rsa_pss_verify(RSAPublicKey const& public_key, Hash::HashKind hash_kind, u32 salt_length, ReadonlyBytes message, ReadonlyBytes signature)
{
    if (!salt_length_fits_in_int(salt_length))
        return false;

    auto salt_length_int = static_cast<int>(salt_length);
    auto params = TRY(pss_parameters(hash_kind, salt_length_int));
    return verify(public_key, hash_kind, params.data(), message, signature);
}

static OSSL_PARAM const pkcs1_parameters[] {
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, const_cast<char*>(OSSL_PKEY_RSA_PAD_MODE_PKCSV15), sizeof(OSSL_PKEY_RSA_PAD_MODE_PKCSV15) - 1),
    OSSL_PARAM_END,
};

ErrorOr<ByteBuffer> rsa_pkcs1_sign(RSAPrivateKey const& private_key, Hash::HashKind hash_kind, ReadonlyBytes message)
{
    return sign(private_key, hash_kind, pkcs1_parameters, message);
}

ErrorOr<bool> rsa_pkcs1_verify(RSAPublicKey const& public_key, Hash::HashKind hash_kind, ReadonlyBytes message, ReadonlyBytes signature)
{
    return verify(public_key, hash_kind, pkcs1_parameters, message, signature);
}

}
