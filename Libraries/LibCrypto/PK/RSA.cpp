/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Memory.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/PK/RSA.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace Crypto::PK {

// NB: The builder reads each BIGNUM when it builds the parameters, so the BIGNUMs must outlive the builder.
class ParameterBuilder {
    AK_MAKE_NONCOPYABLE(ParameterBuilder);
    AK_MAKE_NONMOVABLE(ParameterBuilder);

public:
    ParameterBuilder() = default;

    ~ParameterBuilder()
    {
        OSSL_PARAM_BLD_free(m_builder);
        for (auto* integer : m_integers)
            BN_clear_free(integer);
    }

    ErrorOr<void> push_integer(char const* name, ReadonlyBytes bytes, bool secret)
    {
        if (!m_builder)
            m_builder = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());

        auto* integer = OPENSSL_TRY_PTR(secret ? BN_secure_new() : BN_new());
        TRY(m_integers.try_append(integer));
        OPENSSL_TRY_PTR(BN_bin2bn(bytes.data(), bytes.size(), integer));
        OPENSSL_TRY(OSSL_PARAM_BLD_push_BN(m_builder, name, integer));
        return {};
    }

    OSSL_PARAM_BLD* ptr() { return m_builder; }

private:
    OSSL_PARAM_BLD* m_builder { nullptr };
    Vector<BIGNUM*, 8> m_integers;
};

static ErrorOr<OpenSSL_SharedPKEY> key_from_parameters(int selection, ParameterBuilder& builder)
{
    auto* params = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_to_param(builder.ptr()));
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)));
    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx.ptr()));

    EVP_PKEY* key = nullptr;
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx.ptr(), &key, selection, params));
    return OpenSSL_SharedPKEY { TRY(OpenSSL_PKEY::wrap(key)) };
}

// https://www.rfc-editor.org/rfc/rfc8017#section-3.1
// [...] the RSA public exponent e is an integer between 3 and n - 1 [...]
// NB: An odd e above 1 is at least 3.
static ErrorOr<bool> public_exponent_is_valid(EVP_PKEY* key)
{
    BIGNUM* n = nullptr;
    BIGNUM* e = nullptr;
    ScopeGuard const free_values = [&] {
        BN_free(n);
        BN_free(e);
    };
    OPENSSL_TRY(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &n));
    OPENSSL_TRY(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &e));
    return BN_is_odd(e) && !BN_is_one(e) && BN_cmp(e, n) < 0;
}

RSAPublicKey::RSAPublicKey(OpenSSL_SharedPKEY key)
    : m_key(move(key))
    , m_length(EVP_PKEY_get_size(m_key.ptr()))
{
}

ErrorOr<RSAPublicKey> RSAPublicKey::from_components(ReadonlyBytes modulus, ReadonlyBytes public_exponent)
{
    ParameterBuilder builder;
    TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_N, modulus, false));
    TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_E, public_exponent, false));
    return RSAPublicKey { TRY(key_from_parameters(EVP_PKEY_PUBLIC_KEY, builder)) };
}

ErrorOr<bool> RSAPublicKey::is_valid() const
{
    if (!TRY(public_exponent_is_valid(m_key.ptr())))
        return false;

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, m_key.ptr(), nullptr)));
    return EVP_PKEY_public_check(ctx.ptr()) == 1;
}

static ErrorOr<bool> verify(RSAPublicKey const& public_key, Hash::HashKind hash_kind, OSSL_PARAM const* params, ReadonlyBytes message, ReadonlyBytes signature)
{
    if (signature.size() != public_key.length())
        return false;

    // NB: Keys are checked in full when they are imported, so this only keeps the rule on e, which is cheap.
    if (!TRY(public_exponent_is_valid(public_key.openssl_key())))
        return false;

    auto* key = public_key.openssl_key();
    auto ctx = TRY(OpenSSL_MD_CTX::create());

    OPENSSL_TRY(EVP_DigestVerifyInit_ex(ctx.ptr(), nullptr, TRY(Hash::openssl_digest_name(hash_kind)), nullptr, nullptr, key, params));

    auto result = EVP_DigestVerify(ctx.ptr(), signature.data(), signature.size(), message.data(), message.size());
    if (result == 1)
        return true;
    if (result == 0)
        return false;
    OPENSSL_TRY(result);
    VERIFY_NOT_REACHED();
}

static OSSL_PARAM const pkcs1_parameters[] {
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, const_cast<char*>(OSSL_PKEY_RSA_PAD_MODE_PKCSV15), sizeof(OSSL_PKEY_RSA_PAD_MODE_PKCSV15) - 1),
    OSSL_PARAM_END,
};

ErrorOr<bool> rsa_pkcs1_verify(RSAPublicKey const& public_key, Hash::HashKind hash_kind, ReadonlyBytes message, ReadonlyBytes signature)
{
    return verify(public_key, hash_kind, pkcs1_parameters, message, signature);
}

}
