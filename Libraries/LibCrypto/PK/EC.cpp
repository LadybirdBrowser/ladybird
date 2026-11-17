/*
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Memory.h>
#include <AK/ScopeGuard.h>
#include <AK/StringView.h>
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

static ErrorOr<OpenSSL_SharedPKEY> key_from_parameters(int selection, OSSL_PARAM_BLD* builder)
{
    auto* params = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_to_param(builder));
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)));
    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx.ptr()));

    EVP_PKEY* key = nullptr;
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx.ptr(), &key, selection, params));
    return OpenSSL_SharedPKEY { TRY(OpenSSL_PKEY::wrap(key)) };
}

static ErrorOr<ByteBuffer> integer_parameter(EVP_PKEY* key, char const* name, size_t size)
{
    BIGNUM* value = nullptr;
    OPENSSL_TRY(EVP_PKEY_get_bn_param(key, name, &value));
    ScopeGuard const free_value = [&] { BN_clear_free(value); };

    auto bytes = TRY(ByteBuffer::create_uninitialized(size));
    if (BN_bn2binpad(value, bytes.data(), size) < 0)
        return Error::from_string_literal("EC key component is too large");
    return bytes;
}

ErrorOr<ECPublicKey> ECPublicKey::from_octets(ReadonlyBytes data)
{
    if (data.is_empty())
        return Error::from_string_literal("Empty EC point");

    size_t scalar_size = 0;
    if (data[0] == 0x04 && data.size() % 2 == 1)
        scalar_size = (data.size() - 1) / 2;
    else if (data[0] == 0x02 || data[0] == 0x03)
        scalar_size = data.size() - 1;
    else
        return Error::from_string_literal("Unsupported EC point format");

    auto const* group = TRY(curve_name(scalar_size));

    auto* builder = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_builder = [&] { OSSL_PARAM_BLD_free(builder); };
    OPENSSL_TRY(OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, group, 0));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(builder, OSSL_PKEY_PARAM_PUB_KEY, data.data(), data.size()));
    auto key = key_from_parameters(EVP_PKEY_PUBLIC_KEY, builder);
    if (key.is_error())
        return Error::from_string_literal("Invalid EC public key");

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key.value().ptr(), nullptr)));
    if (EVP_PKEY_public_check(ctx.ptr()) != 1)
        return Error::from_string_literal("Invalid EC public key");

    return ECPublicKey { key.release_value(), scalar_size };
}

ErrorOr<ECPublicKey> ECPublicKey::from_coordinates(ReadonlyBytes x, ReadonlyBytes y)
{
    if (x.size() != y.size())
        return Error::from_string_literal("EC coordinates have different sizes");

    auto octets = TRY(ByteBuffer::create_uninitialized(1 + x.size() + y.size()));
    octets[0] = 0x04;
    octets.overwrite(1, x.data(), x.size());
    octets.overwrite(1 + x.size(), y.data(), y.size());
    return from_octets(octets);
}

ErrorOr<ByteBuffer> ECPublicKey::x_bytes() const
{
    return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_EC_PUB_X, m_scalar_size);
}

ErrorOr<ByteBuffer> ECPublicKey::y_bytes() const
{
    return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_EC_PUB_Y, m_scalar_size);
}

ErrorOr<ByteBuffer> ECPublicKey::to_uncompressed() const
{
    auto x = TRY(x_bytes());
    auto y = TRY(y_bytes());

    auto bytes = TRY(ByteBuffer::create_uninitialized(1 + x.size() + y.size()));
    bytes[0] = 0x04;
    bytes.overwrite(1, x.data(), x.size());
    bytes.overwrite(1 + x.size(), y.data(), y.size());
    return bytes;
}

ErrorOr<bool> ecdsa_verify(ECPublicKey const& public_key, ReadonlyBytes digest, ReadonlyBytes r, ReadonlyBytes s)
{
    if (r.size() != public_key.scalar_size() || s.size() != public_key.scalar_size())
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

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, public_key.openssl_key(), nullptr)));
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
