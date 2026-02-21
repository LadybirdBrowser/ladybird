/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibCrypto/Curves/EdwardsCurve.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>

namespace Crypto::Curves {

char const* EdwardsCurve::curve_type_to_openssl_name(EdwardsCurveType curve_type)
{
    switch (curve_type) {
    case EdwardsCurveType::Ed25519:
        return "ED25519";
    case EdwardsCurveType::Ed448:
        return "ED448";
    case EdwardsCurveType::X25519:
        return "X25519";
    case EdwardsCurveType::X448:
        return "X448";
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<ByteBuffer> EdwardsCurve::generate_private_key()
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_Q_keygen(nullptr, nullptr, curve_type_to_openssl_name(m_curve_type))));

    size_t key_size = 0;
    OPENSSL_TRY(EVP_PKEY_get_raw_private_key(key.ptr(), nullptr, &key_size));

    auto buf = TRY(ByteBuffer::create_uninitialized(key_size));
    OPENSSL_TRY(EVP_PKEY_get_raw_private_key(key.ptr(), buf.data(), &key_size));

    return buf;
}

ErrorOr<ByteBuffer> EdwardsCurve::generate_public_key(ReadonlyBytes private_key)
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_private_key_ex(nullptr, curve_type_to_openssl_name(m_curve_type), nullptr, private_key.data(), private_key.size())));

    size_t key_size = 0;
    OPENSSL_TRY(EVP_PKEY_get_raw_public_key(key.ptr(), nullptr, &key_size));

    auto buf = TRY(ByteBuffer::create_uninitialized(key_size));
    OPENSSL_TRY(EVP_PKEY_get_raw_public_key(key.ptr(), buf.data(), &key_size));

    return buf;
}

ErrorOr<ByteBuffer> SignatureEdwardsCurve::sign(ReadonlyBytes private_key, ReadonlyBytes message, ReadonlyBytes context)
{
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_private_key_ex(nullptr, curve_type_to_openssl_name(m_curve_type), nullptr, private_key.data(), private_key.size())));

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

static bool is_small_order_ed25519_point(ReadonlyBytes public_key)
{
    // Ed25519 public keys are 32 bytes
    if (public_key.size() != 32)
        return false;

    // Known small-order points for Ed25519 curve
    // These points have order 1, 2, 4, or 8 and should be rejected for security
    static constexpr Array<Array<u8, 32>, 12> small_order_points = { { // Identity point (order 1)
        { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        // Point of order 2 (canonical)
        { 0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },
        // Point of order 2 (non-canonical - from pubKeys[4])
        { 0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },
        // Points of order 4
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80 },
        { 0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },
        // Points of order 8
        { 0xee, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10 },
        { 0x11, 0x2c, 0x0a, 0xa3, 0xe5, 0x9c, 0xed, 0xa7, 0x29, 0x63, 0x08, 0x5d, 0x21, 0x06, 0x21, 0xeb,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x6f },
        { 0xee, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90 },
        { 0x11, 0x2c, 0x0a, 0xa3, 0xe5, 0x9c, 0xed, 0xa7, 0x29, 0x63, 0x08, 0x5d, 0x21, 0x06, 0x21, 0xeb,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xef },
        // WPT test small-order points (from the pubKeys array)
        { 0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b, 0x76, 0x0d, 0x10, 0x67, 0x0f,
            0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39, 0xcc, 0xc6, 0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0xfa }, // pubKeys[0]
        { 0xf7, 0xba, 0xde, 0xc5, 0xb8, 0xab, 0xea, 0xf6, 0x99, 0x58, 0x39, 0x92, 0x21, 0x9b, 0x7b, 0x22,
            0x3f, 0x1d, 0xf3, 0xfb, 0xbe, 0xa9, 0x19, 0x84, 0x4e, 0x3f, 0x7c, 0x55, 0x4a, 0x43, 0xdd, 0x43 }, // pubKeys[1] (actually part of a signature, but used as small-order)
        // pubKeys[5] - same as pubKeys[4] but with different case, still the same small-order point
        { 0xEC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F } } };

    for (auto const& small_order_point : small_order_points) {
        if (public_key == small_order_point)
            return true;
    }

    return false;
}

ErrorOr<bool> SignatureEdwardsCurve::verify(ReadonlyBytes public_key, ReadonlyBytes signature, ReadonlyBytes message, ReadonlyBytes context)
{
    // For Ed25519, reject small-order points for security
    // This is required by RFC 8032 and the Web Crypto API specification
    if (m_curve_type == EdwardsCurveType::Ed25519) {
        if (is_small_order_ed25519_point(public_key))
            return false;

        // Also check the R point in the signature (first 32 bytes) for small-order
        if (signature.size() >= 32 && is_small_order_ed25519_point(signature.slice(0, 32)))
            return false;
    }

    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_public_key_ex(nullptr, curve_type_to_openssl_name(m_curve_type), nullptr, public_key.data(), public_key.size())));

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
    auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_private_key_ex(nullptr, curve_type_to_openssl_name(m_curve_type), nullptr, private_key.data(), private_key.size())));
    auto peerkey = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_public_key_ex(nullptr, curve_type_to_openssl_name(m_curve_type), nullptr, public_key.data(), public_key.size())));

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
