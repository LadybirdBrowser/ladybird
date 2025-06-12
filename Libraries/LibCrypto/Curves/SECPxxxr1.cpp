/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Curves/SECPxxxr1.h>

#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace Crypto::Curves {

ErrorOr<UnsignedBigInteger> SECPxxxr1::generate_private_key()
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", m_curve_name)));

    auto priv_bn = TRY(OpenSSL_BN::create());
    auto* priv_bn_ptr = priv_bn.ptr();
    OPENSSL_TRY(EVP_PKEY_get_bn_param(key.ptr(), OSSL_PKEY_PARAM_PRIV_KEY, &priv_bn_ptr));

    return TRY(openssl_bignum_to_unsigned_big_integer(priv_bn));
}

ErrorOr<SECPxxxr1Point> SECPxxxr1::generate_public_key(UnsignedBigInteger scalar)
{
    auto* group = EC_GROUP_new_by_curve_name(EC_curve_nist2nid(m_curve_name));
    ScopeGuard const free_group = [&] { EC_GROUP_free(group); };

    auto scalar_int = TRY(unsigned_big_integer_to_openssl_bignum(scalar));

    auto* r = EC_POINT_new(group);
    ScopeGuard const free_r = [&] { EC_POINT_free(r); };

    OPENSSL_TRY(EC_POINT_mul(group, r, scalar_int.ptr(), nullptr, nullptr, nullptr));

    auto x = TRY(OpenSSL_BN::create());
    auto y = TRY(OpenSSL_BN::create());

    OPENSSL_TRY(EC_POINT_get_affine_coordinates(group, r, x.ptr(), y.ptr(), nullptr));

    return SECPxxxr1Point {
        TRY(openssl_bignum_to_unsigned_big_integer(x)),
        TRY(openssl_bignum_to_unsigned_big_integer(y)),
        m_scalar_size,
    };
}

ErrorOr<SECPxxxr1Point> SECPxxxr1::compute_coordinate(UnsignedBigInteger scalar, SECPxxxr1Point point)
{
    auto* group = EC_GROUP_new_by_curve_name(EC_curve_nist2nid(m_curve_name));
    ScopeGuard const free_group = [&] { EC_GROUP_free(group); };

    auto scalar_int = TRY(unsigned_big_integer_to_openssl_bignum(scalar));

    auto qx = TRY(unsigned_big_integer_to_openssl_bignum(point.x));
    auto qy = TRY(unsigned_big_integer_to_openssl_bignum(point.y));

    auto* q = EC_POINT_new(group);
    ScopeGuard const free_q = [&] { EC_POINT_free(q); };

    OPENSSL_TRY(EC_POINT_set_affine_coordinates(group, q, qx.ptr(), qy.ptr(), nullptr));

    auto* r = EC_POINT_new(group);
    ScopeGuard const free_r = [&] { EC_POINT_free(r); };

    OPENSSL_TRY(EC_POINT_mul(group, r, nullptr, q, scalar_int.ptr(), nullptr));

    auto rx = TRY(OpenSSL_BN::create());
    auto ry = TRY(OpenSSL_BN::create());

    OPENSSL_TRY(EC_POINT_get_affine_coordinates(group, r, rx.ptr(), ry.ptr(), nullptr));

    return SECPxxxr1Point {
        TRY(openssl_bignum_to_unsigned_big_integer(rx)),
        TRY(openssl_bignum_to_unsigned_big_integer(ry)),
        m_scalar_size,
    };
}

ErrorOr<bool> SECPxxxr1::verify(ReadonlyBytes hash, SECPxxxr1Point pubkey, SECPxxxr1Signature signature)
{
    auto ctx_import = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)));

    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx_import.ptr()));

    auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

    OPENSSL_TRY(OSSL_PARAM_BLD_push_utf8_string(params_bld, OSSL_PKEY_PARAM_GROUP_NAME, m_curve_name, strlen(m_curve_name)));

    auto pubkey_bytes = TRY(pubkey.to_uncompressed());
    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_PUB_KEY, pubkey_bytes.data(), pubkey_bytes.size()));

    auto* params = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_to_param(params_bld));
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new()));
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx_import.ptr(), &key_ptr, EVP_PKEY_PUBLIC_KEY, params));

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key.ptr(), nullptr)));

    OPENSSL_TRY(EVP_PKEY_verify_init(ctx.ptr()));

    auto* sig_obj = OPENSSL_TRY_PTR(ECDSA_SIG_new());
    ScopeGuard const free_sig_obj = [&] { ECDSA_SIG_free(sig_obj); };

    auto r = TRY(unsigned_big_integer_to_openssl_bignum(signature.r));
    auto s = TRY(unsigned_big_integer_to_openssl_bignum(signature.s));

    // Let sig_obj own a copy of r and s
    OPENSSL_TRY(ECDSA_SIG_set0(sig_obj, BN_dup(r.ptr()), BN_dup(s.ptr())));

    u8* sig = nullptr;
    ScopeGuard const free_sig = [&] { OPENSSL_free(sig); };

    auto sig_len = TRY([&] -> ErrorOr<int> {
        auto ret = i2d_ECDSA_SIG(sig_obj, &sig);
        if (ret <= 0) {
            OPENSSL_TRY(ret);
            VERIFY_NOT_REACHED();
        }
        return ret;
    }());

    auto ret = EVP_PKEY_verify(ctx.ptr(), sig, sig_len, hash.data(), hash.size());
    if (ret == 1)
        return true;
    if (ret == 0)
        return false;
    OPENSSL_TRY(ret);
    VERIFY_NOT_REACHED();
}

ErrorOr<SECPxxxr1Signature> SECPxxxr1::sign(ReadonlyBytes hash, UnsignedBigInteger private_key)
{
    auto ctx_import = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)));

    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx_import.ptr()));

    auto d = TRY(unsigned_big_integer_to_openssl_bignum(private_key));

    auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

    OPENSSL_TRY(OSSL_PARAM_BLD_push_utf8_string(params_bld, OSSL_PKEY_PARAM_GROUP_NAME, m_curve_name, strlen(m_curve_name)));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_BN(params_bld, OSSL_PKEY_PARAM_PRIV_KEY, d.ptr()));

    auto* params = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_to_param(params_bld));
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new()));
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx_import.ptr(), &key_ptr, EVP_PKEY_KEYPAIR, params));

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key.ptr(), nullptr)));

    OPENSSL_TRY(EVP_PKEY_sign_init(ctx.ptr()));

    size_t sig_len = 0;
    OPENSSL_TRY(EVP_PKEY_sign(ctx.ptr(), nullptr, &sig_len, hash.data(), hash.size()));

    auto sig = TRY(ByteBuffer::create_uninitialized(sig_len));
    OPENSSL_TRY(EVP_PKEY_sign(ctx.ptr(), sig.data(), &sig_len, hash.data(), hash.size()));

    auto const* sig_data = sig.data();
    auto* sig_obj = OPENSSL_TRY_PTR(d2i_ECDSA_SIG(nullptr, &sig_data, sig.size()));
    ScopeGuard const free_sig_obj = [&] { ECDSA_SIG_free(sig_obj); };

    // Duplicate r and s so that sig_obj can own them
    auto r = TRY(OpenSSL_BN::wrap(BN_dup(OPENSSL_TRY_PTR(ECDSA_SIG_get0_r(sig_obj)))));
    auto s = TRY(OpenSSL_BN::wrap(BN_dup(OPENSSL_TRY_PTR(ECDSA_SIG_get0_s(sig_obj)))));

    return SECPxxxr1Signature {
        TRY(openssl_bignum_to_unsigned_big_integer(r)),
        TRY(openssl_bignum_to_unsigned_big_integer(s)),
        m_scalar_size,
    };
}

}
