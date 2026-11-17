/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/PK/RSA.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace Crypto::PK {

// https://www.rfc-editor.org/rfc/rfc8017#section-3.1
// [...] the RSA public exponent e is an integer between 3 and n - 1 [...]
// NB: An odd e above 1 is at least 3.
static bool public_exponent_is_valid(BIGNUM const* n, BIGNUM const* e)
{
    return BN_is_odd(e) && !BN_is_one(e) && BN_cmp(e, n) < 0;
}

static OSSL_PARAM const pkcs1_parameters[] {
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, const_cast<char*>(OSSL_PKEY_RSA_PAD_MODE_PKCSV15), sizeof(OSSL_PKEY_RSA_PAD_MODE_PKCSV15) - 1),
    OSSL_PARAM_END,
};

ErrorOr<bool> rsa_pkcs1_verify(ReadonlyBytes modulus, ReadonlyBytes public_exponent, Hash::HashKind hash_kind, ReadonlyBytes message, ReadonlyBytes signature)
{
    // NB: The builder reads each BIGNUM when it builds the parameters, so the BIGNUMs must outlive it.
    BIGNUM* n = nullptr;
    BIGNUM* e = nullptr;
    OSSL_PARAM_BLD* builder = nullptr;
    OSSL_PARAM* params = nullptr;
    ScopeGuard const free_parameters = [&] {
        OSSL_PARAM_free(params);
        OSSL_PARAM_BLD_free(builder);
        BN_free(n);
        BN_free(e);
    };
    n = OPENSSL_TRY_PTR(BN_bin2bn(modulus.data(), modulus.size(), nullptr));
    e = OPENSSL_TRY_PTR(BN_bin2bn(public_exponent.data(), public_exponent.size(), nullptr));
    builder = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    OPENSSL_TRY(OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, n));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, e));
    params = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_to_param(builder));

    auto key_ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)));
    OPENSSL_TRY(EVP_PKEY_fromdata_init(key_ctx.ptr()));
    EVP_PKEY* key_pointer = nullptr;
    OPENSSL_TRY(EVP_PKEY_fromdata(key_ctx.ptr(), &key_pointer, EVP_PKEY_PUBLIC_KEY, params));
    auto key = TRY(OpenSSL_PKEY::wrap(key_pointer));

    if (!public_exponent_is_valid(n, e))
        return false;
    auto check_ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key.ptr(), nullptr)));
    if (EVP_PKEY_public_check(check_ctx.ptr()) != 1)
        return false;
    if (signature.size() != static_cast<size_t>(EVP_PKEY_get_size(key.ptr())))
        return false;

    auto ctx = TRY(OpenSSL_MD_CTX::create());
    OPENSSL_TRY(EVP_DigestVerifyInit_ex(ctx.ptr(), nullptr, TRY(Hash::openssl_digest_name(hash_kind)), nullptr, nullptr, key.ptr(), pkcs1_parameters));
    auto result = EVP_DigestVerify(ctx.ptr(), signature.data(), signature.size(), message.data(), message.size());
    if (result == 1)
        return true;
    if (result == 0)
        return false;
    OPENSSL_TRY(result);
    VERIFY_NOT_REACHED();
}

}
