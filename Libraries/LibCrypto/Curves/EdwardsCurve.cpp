/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCrypto/Curves/EdwardsCurve.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>

namespace Crypto::Curves {

ErrorOr<ByteBuffer> EdwardsCurve::generate_private_key()
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_Q_keygen(nullptr, nullptr, m_curve_name)));

    size_t key_size = 0;
    OPENSSL_TRY(EVP_PKEY_get_raw_private_key(key.ptr(), nullptr, &key_size));

    auto buf = TRY(ByteBuffer::create_uninitialized(key_size));
    OPENSSL_TRY(EVP_PKEY_get_raw_private_key(key.ptr(), buf.data(), &key_size));

    return buf;
}

ErrorOr<ByteBuffer> EdwardsCurve::generate_public_key(ReadonlyBytes private_key)
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_private_key_ex(nullptr, m_curve_name, nullptr, private_key.data(), private_key.size())));

    size_t key_size = 0;
    OPENSSL_TRY(EVP_PKEY_get_raw_public_key(key.ptr(), nullptr, &key_size));

    auto buf = TRY(ByteBuffer::create_uninitialized(key_size));
    OPENSSL_TRY(EVP_PKEY_get_raw_public_key(key.ptr(), buf.data(), &key_size));

    return buf;
}

ErrorOr<ByteBuffer> SignatureEdwardsCurve::sign(ReadonlyBytes private_key, ReadonlyBytes message, ReadonlyBytes context)
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_private_key_ex(nullptr, m_curve_name, nullptr, private_key.data(), private_key.size())));

    auto ctx = TRY(OpenSSL_MD_CTX::create());

    OSSL_PARAM params[2] = {
        OSSL_PARAM_END,
        OSSL_PARAM_END
    };

    if (!context.is_null()) {
        params[0] = OSSL_PARAM_octet_string(OSSL_SIGNATURE_PARAM_CONTEXT_STRING, const_cast<u8*>(context.data()), context.size());
    }

    OPENSSL_TRY(EVP_DigestSignInit_ex(ctx.ptr(), nullptr, nullptr, nullptr, nullptr, key.ptr(), params));

    size_t sig_len = 0;
    OPENSSL_TRY(EVP_DigestSign(ctx.ptr(), nullptr, &sig_len, message.data(), message.size()));

    auto sig = TRY(ByteBuffer::create_uninitialized(sig_len));
    OPENSSL_TRY(EVP_DigestSign(ctx.ptr(), sig.data(), &sig_len, message.data(), message.size()));

    return sig;
}

ErrorOr<bool> SignatureEdwardsCurve::verify(ReadonlyBytes public_key, ReadonlyBytes signature, ReadonlyBytes message, ReadonlyBytes context)
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_public_key_ex(nullptr, m_curve_name, nullptr, public_key.data(), public_key.size())));

    auto ctx = TRY(OpenSSL_MD_CTX::create());

    OSSL_PARAM params[2] = {
        OSSL_PARAM_END,
        OSSL_PARAM_END
    };

    if (!context.is_null()) {
        params[0] = OSSL_PARAM_octet_string(OSSL_SIGNATURE_PARAM_CONTEXT_STRING, const_cast<u8*>(context.data()), context.size());
    }

    OPENSSL_TRY(EVP_DigestVerifyInit_ex(ctx.ptr(), nullptr, nullptr, nullptr, nullptr, key.ptr(), params));

    auto res = EVP_DigestVerify(ctx.ptr(), signature.data(), signature.size(), message.data(), message.size());
    if (res == 1)
        return true;
    if (res == 0)
        return false;
    OPENSSL_TRY(res);
    VERIFY_NOT_REACHED();
}

ErrorOr<ByteBuffer> ExchangeEdwardsCurve::compute_coordinate(ReadonlyBytes private_key, ReadonlyBytes public_key)
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_private_key_ex(nullptr, m_curve_name, nullptr, private_key.data(), private_key.size())));
    auto peerkey = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_public_key_ex(nullptr, m_curve_name, nullptr, public_key.data(), public_key.size())));

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new(key.ptr(), nullptr)));

    OPENSSL_TRY(EVP_PKEY_derive_init(ctx.ptr()));
    OPENSSL_TRY(EVP_PKEY_derive_set_peer(ctx.ptr(), peerkey.ptr()));

    size_t key_size = 0;
    OPENSSL_TRY(EVP_PKEY_derive(ctx.ptr(), nullptr, &key_size));

    auto buf = TRY(ByteBuffer::create_uninitialized(key_size));
    OPENSSL_TRY(EVP_PKEY_derive(ctx.ptr(), buf.data(), &key_size));

    return buf;
}

}
