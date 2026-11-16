/*
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Memory.h>
#include <AK/ScopeGuard.h>
#include <AK/StringView.h>
#include <LibCrypto/ASN1/DER.h>
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

ErrorOr<ECPrivateKey> ECPrivateKey::generate(size_t scalar_size)
{
    auto key = OpenSSL_SharedPKEY { TRY(OpenSSL_PKEY::wrap(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", TRY(curve_name(scalar_size))))) };
    ECPrivateKey private_key { move(key), scalar_size, {}, {} };
    private_key.m_public_key = TRY(private_key.derive_public_key());
    return private_key;
}

ErrorOr<ECPrivateKey> ECPrivateKey::from_scalar(ReadonlyBytes d_bytes, Optional<ASN1::ObjectIdentifier> parameters, Optional<ECPublicKey> public_key)
{
    auto const* group_name = TRY(curve_name(d_bytes.size()));

    auto* group = OPENSSL_TRY_PTR(EC_GROUP_new_by_curve_name(EC_curve_nist2nid(group_name)));
    ScopeGuard const free_group = [&] { EC_GROUP_free(group); };

    auto* d = OPENSSL_TRY_PTR(BN_secure_new());
    ScopeGuard const free_d = [&] { BN_clear_free(d); };
    OPENSSL_TRY_PTR(BN_bin2bn(d_bytes.data(), d_bytes.size(), d));

    // NB: SEC 1 requires d to be in [1, n − 1].
    if (BN_is_zero(d) || BN_cmp(d, EC_GROUP_get0_order(group)) >= 0)
        return Error::from_string_literal("EC private key is out of range");

    auto* point = OPENSSL_TRY_PTR(EC_POINT_new(group));
    ScopeGuard const free_point = [&] { EC_POINT_free(point); };
    OPENSSL_TRY(EC_POINT_mul(group, point, d, nullptr, nullptr, nullptr));

    auto public_octets = TRY(ByteBuffer::create_uninitialized(1 + 2 * d_bytes.size()));
    if (EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED, public_octets.data(), public_octets.size(), nullptr) != public_octets.size())
        return Error::from_string_literal("Failed to encode EC public key");

    if (public_key.has_value() && TRY(public_key->to_uncompressed()) != public_octets)
        return Error::from_string_literal("EC public key does not match the private key");

    auto* builder = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_builder = [&] { OSSL_PARAM_BLD_free(builder); };
    OPENSSL_TRY(OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, group_name, 0));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_PRIV_KEY, d));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(builder, OSSL_PKEY_PARAM_PUB_KEY, public_octets.data(), public_octets.size()));
    auto key = TRY(key_from_parameters(EVP_PKEY_KEYPAIR, builder));

    return ECPrivateKey { move(key), d_bytes.size(), move(parameters), move(public_key) };
}

// https://www.rfc-editor.org/rfc/rfc5915#section-3
static ErrorOr<ByteBuffer> encode_private_key(ReadonlyBytes d, Optional<ASN1::ObjectIdentifier> const& parameters, Optional<ReadonlyBytes> public_point)
{
    ASN1::Encoder encoder;
    TRY(encoder.write_constructed(ASN1::Class::Universal, ASN1::Kind::Sequence, [&]() -> ErrorOr<void> {
        TRY(encoder.write(1u)); // version
        TRY(encoder.write<ReadonlyBytes>(d));

        if (parameters.has_value()) {
            TRY(encoder.write_constructed(ASN1::Class::Context, static_cast<ASN1::Kind>(0), [&]() -> ErrorOr<void> {
                TRY(encoder.write<ASN1::ObjectIdentifier>(*parameters, {}, ASN1::Kind::ObjectIdentifier));
                return {};
            }));
        }

        if (public_point.has_value()) {
            TRY(encoder.write_constructed(ASN1::Class::Context, static_cast<ASN1::Kind>(1), [&]() -> ErrorOr<void> {
                TRY(encoder.write(ASN1::BitStringView(*public_point, 0)));
                return {};
            }));
        }

        return {};
    }));

    return encoder.finish();
}

ErrorOr<ECPrivateKey> ECPrivateKey::from_private_key_info(EVP_PKEY* key, ASN1::ObjectIdentifier const& curve, ReadonlyBytes encoded)
{
    auto scalar_size = (EVP_PKEY_get_bits(key) + 7) / 8;
    auto d = TRY(integer_parameter(key, OSSL_PKEY_PARAM_PRIV_KEY, scalar_size));
    ScopeGuard const clear_d = [&] { secure_zero(d.data(), d.size()); };
    auto public_key = TRY(TRY(from_scalar(d, {}, {})).derive_public_key());

    auto uncompressed = TRY(public_key.to_uncompressed());
    auto compressed = TRY(ByteBuffer::copy(uncompressed.bytes().trim(1 + scalar_size)));
    compressed[0] = 0x02 | (uncompressed.bytes().last() & 1);

    // NB: OpenSSL accepts BER, and a privateKey of any width. The parameters and the public key are optional, and the
    //     public key may be compressed, so the encoding must be the DER encoding of the key in one of those forms.
    for (auto with_parameters : { false, true }) {
        for (auto public_point : { Optional<ReadonlyBytes> {}, Optional<ReadonlyBytes> { uncompressed }, Optional<ReadonlyBytes> { compressed } }) {
            Optional<ASN1::ObjectIdentifier> parameters;
            if (with_parameters)
                parameters = curve;
            auto candidate_encoding = TRY(encode_private_key(d, parameters, public_point));
            ScopeGuard const clear_candidate_encoding = [&] { secure_zero(candidate_encoding.data(), candidate_encoding.size()); };
            if (candidate_encoding != encoded)
                continue;

            Optional<ECPublicKey> maybe_public_key;
            if (public_point.has_value())
                maybe_public_key = public_key;
            return from_scalar(d, move(parameters), move(maybe_public_key));
        }
    }
    return Error::from_string_literal("Invalid ECPrivateKey");
}

ErrorOr<ByteBuffer> ECPrivateKey::d_bytes() const
{
    return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_PRIV_KEY, m_scalar_size);
}

ErrorOr<ECPublicKey> ECPrivateKey::derive_public_key() const
{
    auto x = TRY(integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_EC_PUB_X, m_scalar_size));
    auto y = TRY(integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_EC_PUB_Y, m_scalar_size));
    return ECPublicKey::from_coordinates(x, y);
}

ErrorOr<ByteBuffer> ECPrivateKey::export_as_der() const
{
    auto d = TRY(d_bytes());
    ScopeGuard const clear_d = [&] { secure_zero(d.data(), d.size()); };
    Optional<ByteBuffer> public_point;
    if (m_public_key.has_value())
        public_point = TRY(m_public_key->to_uncompressed());
    return encode_private_key(d, m_parameters, public_point.map([](auto const& point) { return point.bytes(); }));
}

ErrorOr<ECDSASignature> ecdsa_sign(ECPrivateKey const& private_key, ReadonlyBytes digest)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, private_key.openssl_key(), nullptr)));
    OPENSSL_TRY(EVP_PKEY_sign_init(ctx.ptr()));

    size_t signature_size = 0;
    OPENSSL_TRY(EVP_PKEY_sign(ctx.ptr(), nullptr, &signature_size, digest.data(), digest.size()));

    auto signature = TRY(ByteBuffer::create_uninitialized(signature_size));
    OPENSSL_TRY(EVP_PKEY_sign(ctx.ptr(), signature.data(), &signature_size, digest.data(), digest.size()));

    auto const* signature_data = signature.data();
    auto* signature_object = OPENSSL_TRY_PTR(d2i_ECDSA_SIG(nullptr, &signature_data, signature_size));
    ScopeGuard const free_signature_object = [&] { ECDSA_SIG_free(signature_object); };

    auto size = private_key.scalar_size();
    ECDSASignature result {
        TRY(ByteBuffer::create_uninitialized(size)),
        TRY(ByteBuffer::create_uninitialized(size)),
    };
    if (BN_bn2binpad(ECDSA_SIG_get0_r(signature_object), result.r.data(), size) < 0 || BN_bn2binpad(ECDSA_SIG_get0_s(signature_object), result.s.data(), size) < 0)
        return Error::from_string_literal("ECDSA signature is too large");
    return result;
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

ErrorOr<ByteBuffer> ecdh_derive(ECPrivateKey const& private_key, ECPublicKey const& peer)
{
    if (private_key.scalar_size() != peer.scalar_size())
        return Error::from_string_literal("EC keys are on different curves");

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, private_key.openssl_key(), nullptr)));
    OPENSSL_TRY(EVP_PKEY_derive_init(ctx.ptr()));
    OPENSSL_TRY(EVP_PKEY_derive_set_peer_ex(ctx.ptr(), peer.openssl_key(), 1));

    size_t secret_size = 0;
    OPENSSL_TRY(EVP_PKEY_derive(ctx.ptr(), nullptr, &secret_size));

    auto secret = TRY(ByteBuffer::create_uninitialized(secret_size));
    OPENSSL_TRY(EVP_PKEY_derive(ctx.ptr(), secret.data(), &secret_size));
    secret.resize(secret_size);
    return secret;
}

}
