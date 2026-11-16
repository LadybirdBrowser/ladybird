/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Memory.h>
#include <AK/ScopeGuard.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/PK/MLDSA.h>
#include <LibCrypto/PK/ModuleLatticePrivateKey.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>

namespace Crypto::PK {

static constexpr MLDSASize sizes[] { MLDSA44, MLDSA65, MLDSA87 };

static char const* openssl_name(MLDSASize size)
{
    switch (size) {
    case MLDSA44:
        return "ML-DSA-44";
    case MLDSA65:
        return "ML-DSA-65";
    case MLDSA87:
        return "ML-DSA-87";
    }
    VERIFY_NOT_REACHED();
}

static size_t public_key_size(MLDSASize size)
{
    switch (size) {
    case MLDSA44:
        return 1312;
    case MLDSA65:
        return 1952;
    case MLDSA87:
        return 2592;
    }
    VERIFY_NOT_REACHED();
}

static size_t expanded_key_size(MLDSASize size)
{
    switch (size) {
    case MLDSA44:
        return 2560;
    case MLDSA65:
        return 4032;
    case MLDSA87:
        return 4896;
    }
    VERIFY_NOT_REACHED();
}

static ErrorOr<MLDSASize> size_for_length(ReadonlyBytes bytes, size_t (*size_of)(MLDSASize))
{
    for (auto size : sizes) {
        if (size_of(size) == bytes.size())
            return size;
    }
    return Error::from_string_literal("Invalid ML-DSA key length");
}

static ErrorOr<OpenSSL_SharedPKEY> key_from_octets(MLDSASize size, int selection, char const* name, ReadonlyBytes octets)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, openssl_name(size), nullptr)));
    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx.ptr()));

    OSSL_PARAM params[] {
        OSSL_PARAM_octet_string(name, const_cast<u8*>(octets.data()), octets.size()),
        OSSL_PARAM_END,
    };

    EVP_PKEY* key = nullptr;
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx.ptr(), &key, selection, params));
    return OpenSSL_SharedPKEY { TRY(OpenSSL_PKEY::wrap(key)) };
}

ErrorOr<MLDSAPublicKey> MLDSAPublicKey::from_bytes(ReadonlyBytes bytes)
{
    auto size = TRY(size_for_length(bytes, public_key_size));
    auto key = TRY(key_from_octets(size, EVP_PKEY_PUBLIC_KEY, OSSL_PKEY_PARAM_PUB_KEY, bytes));
    return MLDSAPublicKey { move(key), TRY(ByteBuffer::copy(bytes)), size };
}

ErrorOr<MLDSAPrivateKey> MLDSAPrivateKey::generate(MLDSASize size, ReadonlyBytes seed)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, openssl_name(size), nullptr)));
    OPENSSL_TRY(EVP_PKEY_keygen_init(ctx.ptr()));

    if (!seed.is_empty()) {
        OSSL_PARAM params[] {
            OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ML_DSA_SEED, const_cast<u8*>(seed.data()), seed.size()),
            OSSL_PARAM_END,
        };
        OPENSSL_TRY(EVP_PKEY_CTX_set_params(ctx.ptr(), params));
    }

    EVP_PKEY* generated = nullptr;
    OPENSSL_TRY(EVP_PKEY_generate(ctx.ptr(), &generated));
    auto key = OpenSSL_SharedPKEY { TRY(OpenSSL_PKEY::wrap(generated)) };

    auto public_key = TRY(get_byte_buffer_param_from_key(key.ptr(), OSSL_PKEY_PARAM_PUB_KEY));
    return MLDSAPrivateKey { move(key), move(public_key), size };
}

MLDSAPrivateKey MLDSAPrivateKey::in_expanded_key_form(MLDSASize size)
{
    MLDSAPrivateKey private_key;
    private_key.m_size = size;
    return private_key;
}

ErrorOr<ReadonlyBytes> MLDSAPrivateKey::seed(Bytes buffer) const
{
    if (!m_key.ptr())
        return ReadonlyBytes {};
    size_t size = 0;
    OPENSSL_TRY(EVP_PKEY_get_octet_string_param(m_key.ptr(), OSSL_PKEY_PARAM_ML_DSA_SEED, buffer.data(), buffer.size(), &size));
    return buffer.trim(size);
}

ErrorOr<ByteBuffer> MLDSAPrivateKey::expanded_key() const
{
    if (!m_key.ptr())
        return ByteBuffer {};
    return get_byte_buffer_param_from_key(m_key.ptr(), OSSL_PKEY_PARAM_PRIV_KEY);
}

ErrorOr<ByteBuffer> MLDSAPrivateKey::export_as_der() const
{
    // NB: The key is exported in the seed-only form, which a key without its seed cannot be.
    Array<u8, seed_size> buffer;
    ScopeGuard const clear_buffer = [&] { secure_zero(buffer.data(), buffer.size()); };
    auto seed = TRY(this->seed(buffer));
    if (seed.is_empty())
        return Error::from_string_literal("Cannot export an ML-DSA private key without its seed");

    ASN1::Encoder encoder;
    TRY(encoder.write<ReadonlyBytes>(seed, ASN1::Class::Context, static_cast<ASN1::Kind>(0)));

    return encoder.finish();
}

ErrorOr<MLDSA::KeyPairType> MLDSA::parse_mldsa_key(MLDSASize size, ReadonlyBytes der)
{
    auto encoded_key = TRY(parse_module_lattice_private_key(der, 32, expanded_key_size(size)));

    if (encoded_key.seed.is_empty())
        return KeyPairType { {}, MLDSAPrivateKey::in_expanded_key_form(size) };

    auto key_pair = TRY(generate_key_pair(size, encoded_key.seed));
    if (!encoded_key.expanded_key.is_empty()) {
        auto expanded_key = TRY(key_pair.private_key.expanded_key());
        ScopeGuard const clear_expanded_key = [&] { secure_zero(expanded_key.data(), expanded_key.size()); };
        if (expanded_key != encoded_key.expanded_key)
            return Error::from_string_literal("The expanded key does not match the seed");
    }
    return key_pair;
}

ErrorOr<MLDSA::KeyPairType> MLDSA::generate_key_pair(MLDSASize size, ReadonlyBytes seed)
{
    auto private_key = TRY(MLDSAPrivateKey::generate(size, seed));
    auto public_key = TRY(MLDSAPublicKey::from_bytes(private_key.public_key()));
    return KeyPairType { move(public_key), move(private_key) };
}

static Array<OSSL_PARAM, 2> context_parameters(ReadonlyBytes context)
{
    Array<OSSL_PARAM, 2> params { OSSL_PARAM_construct_end(), OSSL_PARAM_construct_end() };
    if (!context.is_empty())
        params[0] = OSSL_PARAM_construct_octet_string(OSSL_SIGNATURE_PARAM_CONTEXT_STRING, const_cast<u8*>(context.data()), context.size());
    return params;
}

ErrorOr<ByteBuffer> mldsa_sign(MLDSASize size, MLDSAPrivateKey const& private_key, ReadonlyBytes context, ReadonlyBytes message)
{
    if (private_key.size() != size)
        return Error::from_string_literal("The key is for a different ML-DSA parameter set");

    auto params = context_parameters(context);
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, private_key.openssl_key(), nullptr)));
    auto* sign_algorithm = OPENSSL_TRY_PTR(EVP_SIGNATURE_fetch(nullptr, openssl_name(size), nullptr));
    ScopeGuard const free_sign_algorithm = [&] { EVP_SIGNATURE_free(sign_algorithm); };

    size_t sign_size;
    OPENSSL_TRY(EVP_PKEY_sign_message_init(ctx.ptr(), sign_algorithm, params.data()));
    OPENSSL_TRY(EVP_PKEY_sign(ctx.ptr(), nullptr, &sign_size, message.data(), message.size()));

    auto result = TRY(ByteBuffer::create_uninitialized(sign_size));
    OPENSSL_TRY(EVP_PKEY_sign(ctx.ptr(), result.data(), &sign_size, message.data(), message.size()));

    return result;
}

ErrorOr<bool> mldsa_verify(MLDSASize size, MLDSAPublicKey const& public_key, ReadonlyBytes context, ReadonlyBytes message, ReadonlyBytes signature)
{
    if (public_key.size() != size)
        return Error::from_string_literal("The key is for a different ML-DSA parameter set");

    auto params = context_parameters(context);
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, public_key.openssl_key(), nullptr)));
    auto* sign_algorithm = OPENSSL_TRY_PTR(EVP_SIGNATURE_fetch(nullptr, openssl_name(size), nullptr));
    ScopeGuard const free_sign_algorithm = [&] { EVP_SIGNATURE_free(sign_algorithm); };

    OPENSSL_TRY(EVP_PKEY_verify_message_init(ctx.ptr(), sign_algorithm, params.data()));

    auto ret = EVP_PKEY_verify(ctx.ptr(), signature.data(), signature.size(), message.data(), message.size());
    if (ret == 1)
        return true;
    if (ret == 0)
        return false;
    OPENSSL_TRY(ret);
    VERIFY_NOT_REACHED();
}

}
