/*
 * Copyright (c) 2023, Michiel Visser <opensource@webmichiel.nl>
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/StringView.h>
#include <LibCrypto/ASN1/Constants.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Curves/EllipticCurve.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace {
// Used by ASN1 macros
static String s_error_string;
}

namespace Crypto::Curves {

struct SECPxxxr1CurveParameters {
    char const* name;
};

struct SECPxxxr1Point {
    UnsignedBigInteger x;
    UnsignedBigInteger y;
    size_t size;

    static ErrorOr<ByteBuffer> scalar_to_bytes(UnsignedBigInteger const& a, size_t size)
    {
        auto a_bytes = TRY(ByteBuffer::create_uninitialized(a.byte_length()));
        auto a_size = a.export_data(a_bytes.span());
        VERIFY(a_size >= size);

        for (size_t i = 0; i < a_size - size; i++) {
            if (a_bytes[i] != 0) {
                return Error::from_string_literal("Scalar is too large for the given size");
            }
        }

        return a_bytes.slice(a_size - size, size);
    }

    static ErrorOr<SECPxxxr1Point> from_uncompressed(ReadonlyBytes data)
    {
        if (data.size() < 1 || data[0] != 0x04)
            return Error::from_string_literal("Invalid length or not an uncompressed SECPxxxr1 point");

        auto half_size = (data.size() - 1) / 2;
        return SECPxxxr1Point {
            UnsignedBigInteger::import_data(data.slice(1, half_size)),
            UnsignedBigInteger::import_data(data.slice(1 + half_size, half_size)),
            half_size,
        };
    }

    ErrorOr<ByteBuffer> x_bytes() const
    {
        return scalar_to_bytes(x, size);
    }

    ErrorOr<ByteBuffer> y_bytes() const
    {
        return scalar_to_bytes(y, size);
    }

    ErrorOr<ByteBuffer> to_uncompressed() const
    {
        auto x = TRY(x_bytes());
        auto y = TRY(y_bytes());

        auto bytes = TRY(ByteBuffer::create_uninitialized(1 + (size * 2)));
        bytes[0] = 0x04; // uncompressed
        bytes.overwrite(1, x.data(), size);
        bytes.overwrite(1 + size, y.data(), size);
        return bytes;
    }
};

struct SECPxxxr1Signature {
    UnsignedBigInteger r;
    UnsignedBigInteger s;
    size_t size;

    static ErrorOr<SECPxxxr1Signature> from_asn(Span<int const> curve_oid, ReadonlyBytes signature, Vector<StringView> current_scope)
    {
        ASN1::Decoder decoder(signature);
        ENTER_TYPED_SCOPE(Sequence, "SECPxxxr1Signature");
        READ_OBJECT(Integer, UnsignedBigInteger, r_big_int);
        READ_OBJECT(Integer, UnsignedBigInteger, s_big_int);

        size_t scalar_size;
        if (curve_oid == ASN1::secp256r1_oid) {
            scalar_size = ceil_div(256, 8);
        } else if (curve_oid == ASN1::secp384r1_oid) {
            scalar_size = ceil_div(384, 8);
        } else if (curve_oid == ASN1::secp521r1_oid) {
            scalar_size = ceil_div(521, 8);
        } else {
            return Error::from_string_literal("Unknown SECPxxxr1 curve");
        }

        if (r_big_int.byte_length() < scalar_size || s_big_int.byte_length() < scalar_size)
            return Error::from_string_literal("Invalid SECPxxxr1 signature");

        return SECPxxxr1Signature { r_big_int, s_big_int, scalar_size };
    }

    ErrorOr<ByteBuffer> r_bytes() const
    {
        return SECPxxxr1Point::scalar_to_bytes(r, size);
    }

    ErrorOr<ByteBuffer> s_bytes() const
    {
        return SECPxxxr1Point::scalar_to_bytes(s, size);
    }

    ErrorOr<ByteBuffer> to_asn()
    {
        ASN1::Encoder encoder;
        TRY(encoder.write_constructed(ASN1::Class::Universal, ASN1::Kind::Sequence, [&]() -> ErrorOr<void> {
            TRY(encoder.write(r));
            TRY(encoder.write(s));
            return {};
        }));

        return encoder.finish();
    }
};

template<size_t bit_size, SECPxxxr1CurveParameters const& CURVE_PARAMETERS>
class SECPxxxr1 : public EllipticCurve {
    static constexpr size_t KEY_BIT_SIZE = bit_size;
    static constexpr size_t KEY_BYTE_SIZE = ceil_div(KEY_BIT_SIZE, 8ull);
    static constexpr size_t POINT_BYTE_SIZE = 1 + 2 * KEY_BYTE_SIZE;

public:
    size_t key_size() override { return POINT_BYTE_SIZE; }

    ErrorOr<ByteBuffer> generate_private_key() override
    {
        auto key = TRY(generate_private_key_scalar());

        auto buffer = TRY(ByteBuffer::create_uninitialized(KEY_BYTE_SIZE));
        auto buffer_bytes = buffer.bytes();
        auto size = key.export_data(buffer_bytes);
        return buffer.slice(0, size);
    }

    ErrorOr<UnsignedBigInteger> generate_private_key_scalar()
    {
        auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", CURVE_PARAMETERS.name)));

        auto priv_bn = TRY(OpenSSL_BN::create());
        auto* priv_bn_ptr = priv_bn.ptr();
        OPENSSL_TRY(EVP_PKEY_get_bn_param(key.ptr(), OSSL_PKEY_PARAM_PRIV_KEY, &priv_bn_ptr));

        return TRY(openssl_bignum_to_unsigned_big_integer(priv_bn));
    }

    ErrorOr<ByteBuffer> generate_public_key(ReadonlyBytes a) override
    {
        auto a_int = UnsignedBigInteger::import_data(a);
        auto point = TRY(generate_public_key_point(a_int));
        return point.to_uncompressed();
    }

    ErrorOr<SECPxxxr1Point> generate_public_key_point(UnsignedBigInteger scalar)
    {
        auto* group = EC_GROUP_new_by_curve_name(EC_curve_nist2nid(CURVE_PARAMETERS.name));
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
            KEY_BYTE_SIZE,
        };
    }

    ErrorOr<ByteBuffer> compute_coordinate(ReadonlyBytes scalar_bytes, ReadonlyBytes point_bytes) override
    {
        auto scalar = UnsignedBigInteger::import_data(scalar_bytes);
        auto point = TRY(SECPxxxr1Point::from_uncompressed(point_bytes));
        auto result = TRY(compute_coordinate_point(scalar, { point.x, point.y, KEY_BYTE_SIZE }));
        return result.to_uncompressed();
    }

    ErrorOr<SECPxxxr1Point> compute_coordinate_point(UnsignedBigInteger scalar, SECPxxxr1Point point)
    {
        auto* group = EC_GROUP_new_by_curve_name(EC_curve_nist2nid(CURVE_PARAMETERS.name));
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
            KEY_BYTE_SIZE,
        };
    }

    ErrorOr<ByteBuffer> derive_premaster_key(ReadonlyBytes shared_point_bytes) override
    {
        auto shared_point = TRY(SECPxxxr1Point::from_uncompressed(shared_point_bytes));
        auto premaster_key_point = TRY(derive_premaster_key_point(shared_point));
        return premaster_key_point.to_uncompressed();
    }

    ErrorOr<SECPxxxr1Point> derive_premaster_key_point(SECPxxxr1Point shared_point)
    {
        return shared_point;
    }

    ErrorOr<bool> verify_point(ReadonlyBytes hash, SECPxxxr1Point pubkey, SECPxxxr1Signature signature)
    {
        auto ctx_import = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)));

        OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx_import.ptr()));

        auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
        ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

        OPENSSL_TRY(OSSL_PARAM_BLD_push_utf8_string(params_bld, OSSL_PKEY_PARAM_GROUP_NAME, CURVE_PARAMETERS.name, strlen(CURVE_PARAMETERS.name)));

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

    ErrorOr<bool> verify(ReadonlyBytes hash, ReadonlyBytes pubkey, SECPxxxr1Signature signature)
    {
        auto pubkey_point = TRY(SECPxxxr1Point::from_uncompressed(pubkey));
        return verify_point(hash, pubkey_point, signature);
    }

    ErrorOr<SECPxxxr1Signature> sign_scalar(ReadonlyBytes hash, UnsignedBigInteger private_key)
    {
        auto ctx_import = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)));

        OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx_import.ptr()));

        auto d = TRY(unsigned_big_integer_to_openssl_bignum(private_key));

        auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
        ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

        OPENSSL_TRY(OSSL_PARAM_BLD_push_utf8_string(params_bld, OSSL_PKEY_PARAM_GROUP_NAME, CURVE_PARAMETERS.name, strlen(CURVE_PARAMETERS.name)));
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
            KEY_BYTE_SIZE,
        };
    }

    ErrorOr<SECPxxxr1Signature> sign(ReadonlyBytes hash, ReadonlyBytes private_key_bytes)
    {
        auto signature = TRY(sign_scalar(hash, UnsignedBigInteger::import_data(private_key_bytes.data(), private_key_bytes.size())));
        return signature;
    }
};

// SECP256r1 curve
static constexpr SECPxxxr1CurveParameters SECP256r1_CURVE_PARAMETERS {
    .name = "P-256",
};
using SECP256r1 = SECPxxxr1<256, SECP256r1_CURVE_PARAMETERS>;

// SECP384r1 curve
static constexpr SECPxxxr1CurveParameters SECP384r1_CURVE_PARAMETERS {
    .name = "P-384",
};
using SECP384r1 = SECPxxxr1<384, SECP384r1_CURVE_PARAMETERS>;

// SECP521r1 curve
static constexpr SECPxxxr1CurveParameters SECP521r1_CURVE_PARAMETERS {
    .name = "P-521",
};
using SECP521r1 = SECPxxxr1<521, SECP521r1_CURVE_PARAMETERS>;

}
