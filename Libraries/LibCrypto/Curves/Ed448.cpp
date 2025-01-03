/*
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCrypto/Curves/Ed448.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>

namespace Crypto::Curves {

ErrorOr<ByteBuffer> Ed448::generate_private_key()
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED448")));

    size_t key_size = EVP_PKEY_get_size(key.ptr());
    auto buf = TRY(ByteBuffer::create_uninitialized(key_size));

    OPENSSL_TRY(EVP_PKEY_get_raw_private_key(key.ptr(), buf.data(), &key_size));

    return buf.slice(0, key_size);
}

ErrorOr<ByteBuffer> Ed448::generate_public_key(ReadonlyBytes private_key)
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED448, nullptr, private_key.data(), private_key.size())));

    size_t key_size = EVP_PKEY_get_size(key.ptr());
    auto buf = TRY(ByteBuffer::create_uninitialized(key_size));

    OPENSSL_TRY(EVP_PKEY_get_raw_public_key(key.ptr(), buf.data(), &key_size));

    return buf.slice(0, key_size);
}

ErrorOr<ByteBuffer> Ed448::sign(ReadonlyBytes private_key, ReadonlyBytes message, ReadonlyBytes context)
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_private_key_ex(nullptr, "ED448", nullptr, private_key.data(), private_key.size())));

    auto ctx = TRY(OpenSSL_MD_CTX::create());

    OSSL_PARAM params[] = {
        OSSL_PARAM_octet_string(OSSL_SIGNATURE_PARAM_CONTEXT_STRING, const_cast<u8*>(context.data()), context.size()),
        OSSL_PARAM_END
    };

    OPENSSL_TRY(EVP_DigestSignInit_ex(ctx.ptr(), nullptr, nullptr, nullptr, nullptr, key.ptr(), params));

    size_t sig_len = signature_size();
    auto sig = TRY(ByteBuffer::create_uninitialized(sig_len));

    OPENSSL_TRY(EVP_DigestSign(ctx.ptr(), sig.data(), &sig_len, message.data(), message.size()));

    return sig.slice(0, sig_len);
}

ErrorOr<bool> Ed448::verify(ReadonlyBytes public_key, ReadonlyBytes signature, ReadonlyBytes message, ReadonlyBytes context)
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_public_key_ex(nullptr, "ED448", nullptr, public_key.data(), public_key.size())));

    auto ctx = TRY(OpenSSL_MD_CTX::create());

    OSSL_PARAM params[] = {
        OSSL_PARAM_octet_string(OSSL_SIGNATURE_PARAM_CONTEXT_STRING, const_cast<u8*>(context.data()), context.size()),
        OSSL_PARAM_END
    };

    OPENSSL_TRY(EVP_DigestVerifyInit_ex(ctx.ptr(), nullptr, nullptr, nullptr, nullptr, key.ptr(), params));

    auto res = EVP_DigestVerify(ctx.ptr(), signature.data(), signature.size(), message.data(), message.size());
    if (res == 1)
        return true;
    if (res == 0)
        return false;
    OPENSSL_TRY(res);
    VERIFY_NOT_REACHED();
}
}
