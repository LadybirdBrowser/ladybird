/*
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/ScopeGuard.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/PK/EC.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace Crypto::PK {

static ErrorOr<char const*> curve_name(size_t scalar_size)
{
    switch (scalar_size) {
    case 32:
        return "P-256";
    case 48:
        return "P-384";
    case 66:
        return "P-521";
    default:
        return Error::from_string_literal("Unsupported EC curve");
    }
}

static ErrorOr<OpenSSL_PKEY> public_key_from_coordinates(ReadonlyBytes x, ReadonlyBytes y)
{
    if (x.size() != y.size())
        return Error::from_string_literal("EC coordinates have different sizes");
    auto const* group = TRY(curve_name(x.size()));
    auto octets = TRY(ByteBuffer::create_uninitialized(1 + x.size() + y.size()));
    octets[0] = 0x04;
    octets.overwrite(1, x.data(), x.size());
    octets.overwrite(1 + x.size(), y.data(), y.size());

    auto* builder = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_builder = [&] { OSSL_PARAM_BLD_free(builder); };
    OPENSSL_TRY(OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, group, 0));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(builder, OSSL_PKEY_PARAM_PUB_KEY, octets.data(), octets.size()));
    auto* params = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_to_param(builder));
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)));
    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx.ptr()));
    EVP_PKEY* key_pointer = nullptr;
    if (EVP_PKEY_fromdata(ctx.ptr(), &key_pointer, EVP_PKEY_PUBLIC_KEY, params) != 1)
        return Error::from_string_literal("Invalid EC public key");
    auto key = TRY(OpenSSL_PKEY::wrap(key_pointer));

    auto check_ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key.ptr(), nullptr)));
    if (EVP_PKEY_public_check(check_ctx.ptr()) != 1)
        return Error::from_string_literal("Invalid EC public key");
    return key;
}

ErrorOr<bool> ecdsa_verify(ReadonlyBytes x, ReadonlyBytes y, ReadonlyBytes digest, ReadonlyBytes r, ReadonlyBytes s)
{
    auto key = TRY(public_key_from_coordinates(x, y));
    if (r.size() != x.size() || s.size() != x.size())
        return false;

    auto* signature_object = OPENSSL_TRY_PTR(ECDSA_SIG_new());
    ScopeGuard const free_signature_object = [&] { ECDSA_SIG_free(signature_object); };
    auto* r_value = OPENSSL_TRY_PTR(BN_bin2bn(r.data(), r.size(), nullptr));
    auto* s_value = BN_bin2bn(s.data(), s.size(), nullptr);
    if (!s_value || ECDSA_SIG_set0(signature_object, r_value, s_value) != 1) {
        BN_free(r_value);
        BN_free(s_value);
        return Error::from_string_literal("Failed to build the ECDSA signature");
    }

    u8* signature = nullptr;
    ScopeGuard const free_signature = [&] { OPENSSL_free(signature); };
    auto signature_size = i2d_ECDSA_SIG(signature_object, &signature);
    if (signature_size <= 0)
        return Error::from_string_literal("Failed to encode the ECDSA signature");

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key.ptr(), nullptr)));
    OPENSSL_TRY(EVP_PKEY_verify_init(ctx.ptr()));
    auto result = EVP_PKEY_verify(ctx.ptr(), signature, signature_size, digest.data(), digest.size());
    if (result == 1)
        return true;
    if (result == 0)
        return false;
    OPENSSL_TRY(result);
    VERIFY_NOT_REACHED();
}

}
