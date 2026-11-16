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
#include <LibCrypto/PK/MLKEM.h>
#include <LibCrypto/PK/ModuleLatticePrivateKey.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>

namespace Crypto::PK {

static constexpr MLKEMSize sizes[] { MLKEMSize::MLKEM512, MLKEMSize::MLKEM768, MLKEMSize::MLKEM1024 };

static char const* openssl_name(MLKEMSize size)
{
    switch (size) {
    case MLKEMSize::MLKEM512:
        return "ML-KEM-512";
    case MLKEMSize::MLKEM768:
        return "ML-KEM-768";
    case MLKEMSize::MLKEM1024:
        return "ML-KEM-1024";
    }
    VERIFY_NOT_REACHED();
}

static size_t public_key_size(MLKEMSize size)
{
    switch (size) {
    case MLKEMSize::MLKEM512:
        return 800;
    case MLKEMSize::MLKEM768:
        return 1184;
    case MLKEMSize::MLKEM1024:
        return 1568;
    }
    VERIFY_NOT_REACHED();
}

static size_t expanded_key_size(MLKEMSize size)
{
    switch (size) {
    case MLKEMSize::MLKEM512:
        return 1632;
    case MLKEMSize::MLKEM768:
        return 2400;
    case MLKEMSize::MLKEM1024:
        return 3168;
    }
    VERIFY_NOT_REACHED();
}

static ErrorOr<MLKEMSize> size_for_length(ReadonlyBytes bytes, size_t (*size_of)(MLKEMSize))
{
    for (auto size : sizes) {
        if (size_of(size) == bytes.size())
            return size;
    }
    return Error::from_string_literal("Invalid ML-KEM key length");
}

static ErrorOr<OpenSSL_SharedPKEY> key_from_octets(MLKEMSize size, int selection, char const* name, ReadonlyBytes octets)
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

ErrorOr<MLKEMPublicKey> MLKEMPublicKey::from_bytes(ReadonlyBytes bytes)
{
    MLKEMPublicKey public_key;
    public_key.m_public_key = TRY(ByteBuffer::copy(bytes));

    // NB: OpenSSL builds a key only from bytes of a parameter set's length whose coefficients are all below q, which
    //     are the two parts of the encapsulation key check.
    auto size = size_for_length(bytes, public_key_size);
    if (size.is_error())
        return public_key;
    auto key = key_from_octets(size.value(), EVP_PKEY_PUBLIC_KEY, OSSL_PKEY_PARAM_PUB_KEY, bytes);
    if (key.is_error())
        return public_key;

    public_key.m_key = key.release_value();
    public_key.m_size = size.value();
    return public_key;
}

ErrorOr<MLKEMPrivateKey> MLKEMPrivateKey::generate(MLKEMSize size, ReadonlyBytes seed)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, openssl_name(size), nullptr)));
    OPENSSL_TRY(EVP_PKEY_keygen_init(ctx.ptr()));

    if (!seed.is_empty()) {
        OSSL_PARAM params[] {
            OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ML_KEM_SEED, const_cast<u8*>(seed.data()), seed.size()),
            OSSL_PARAM_END,
        };
        OPENSSL_TRY(EVP_PKEY_CTX_set_params(ctx.ptr(), params));
    }

    EVP_PKEY* generated = nullptr;
    OPENSSL_TRY(EVP_PKEY_generate(ctx.ptr(), &generated));
    auto key = OpenSSL_SharedPKEY { TRY(OpenSSL_PKEY::wrap(generated)) };

    auto public_key = TRY(get_byte_buffer_param_from_key(key.ptr(), OSSL_PKEY_PARAM_PUB_KEY));
    return MLKEMPrivateKey { move(key), move(public_key), size };
}

MLKEMPrivateKey MLKEMPrivateKey::in_expanded_key_form(MLKEMSize size)
{
    MLKEMPrivateKey private_key;
    private_key.m_size = size;
    return private_key;
}

ErrorOr<ReadonlyBytes> MLKEMPrivateKey::seed(Bytes buffer) const
{
    if (!m_key.ptr())
        return ReadonlyBytes {};
    size_t size = 0;
    OPENSSL_TRY(EVP_PKEY_get_octet_string_param(m_key.ptr(), OSSL_PKEY_PARAM_ML_KEM_SEED, buffer.data(), buffer.size(), &size));
    return buffer.trim(size);
}

ErrorOr<ByteBuffer> MLKEMPrivateKey::expanded_key() const
{
    if (!m_key.ptr())
        return ByteBuffer {};
    return get_byte_buffer_param_from_key(m_key.ptr(), OSSL_PKEY_PARAM_PRIV_KEY);
}

ErrorOr<ByteBuffer> MLKEMPrivateKey::export_as_der() const
{
    // NB: The key is exported in the seed-only form, which a key without its seed cannot be.
    Array<u8, seed_size> buffer;
    ScopeGuard const clear_buffer = [&] { secure_zero(buffer.data(), buffer.size()); };
    auto seed = TRY(this->seed(buffer));
    if (seed.is_empty())
        return Error::from_string_literal("Cannot export an ML-KEM private key without its seed");

    ASN1::Encoder encoder;
    TRY(encoder.write<ReadonlyBytes>(seed, ASN1::Class::Context, static_cast<ASN1::Kind>(0)));

    return encoder.finish();
}

ErrorOr<MLKEM::KeyPairType> MLKEM::parse_mlkem_key(MLKEMSize size, ReadonlyBytes der)
{
    auto encoded_key = TRY(parse_module_lattice_private_key(der, 64, expanded_key_size(size)));

    if (encoded_key.seed.is_empty())
        return KeyPairType { {}, MLKEMPrivateKey::in_expanded_key_form(size) };

    auto key_pair = TRY(generate_key_pair(size, encoded_key.seed));
    if (!encoded_key.expanded_key.is_empty()) {
        auto expanded_key = TRY(key_pair.private_key.expanded_key());
        ScopeGuard const clear_expanded_key = [&] { secure_zero(expanded_key.data(), expanded_key.size()); };
        if (expanded_key != encoded_key.expanded_key)
            return Error::from_string_literal("The expanded key does not match the seed");
    }
    return key_pair;
}

ErrorOr<MLKEM::KeyPairType> MLKEM::generate_key_pair(MLKEMSize size, ReadonlyBytes seed)
{
    auto private_key = TRY(MLKEMPrivateKey::generate(size, seed));
    auto public_key = TRY(MLKEMPublicKey::from_bytes(private_key.public_key()));
    return KeyPairType { move(public_key), move(private_key) };
}

ErrorOr<MLKEMEncapsulation> MLKEM::encapsulate(MLKEMSize size, MLKEMPublicKey const& key)
{
    if (!key.size().has_value() || *key.size() != size)
        return Error::from_string_literal("The key fails the encapsulation key check");

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key.openssl_key(), nullptr)));

    OPENSSL_TRY(EVP_PKEY_encapsulate_init(ctx.ptr(), nullptr));

    size_t shared_key_size;
    size_t ciphertext_length;
    OPENSSL_TRY(EVP_PKEY_encapsulate(ctx.ptr(), nullptr, &ciphertext_length, nullptr, &shared_key_size));

    auto shared_key = TRY(ByteBuffer::create_uninitialized(shared_key_size));
    auto ciphertext = TRY(ByteBuffer::create_uninitialized(ciphertext_length));

    OPENSSL_TRY(EVP_PKEY_encapsulate(ctx.ptr(), ciphertext.data(), &ciphertext_length, shared_key.data(), &shared_key_size));

    return MLKEMEncapsulation { move(shared_key), move(ciphertext) };
}

ErrorOr<ByteBuffer> MLKEM::decapsulate(MLKEMSize size, MLKEMPrivateKey const& key, ReadonlyBytes ciphertext)
{
    if (key.size() != size)
        return Error::from_string_literal("The key is for a different ML-KEM parameter set");

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key.openssl_key(), nullptr)));

    OPENSSL_TRY(EVP_PKEY_decapsulate_init(ctx.ptr(), nullptr));

    size_t shared_key_size;
    OPENSSL_TRY(EVP_PKEY_decapsulate(ctx.ptr(), nullptr, &shared_key_size, ciphertext.data(), ciphertext.size()));

    auto shared_key = TRY(ByteBuffer::create_uninitialized(shared_key_size));
    OPENSSL_TRY(EVP_PKEY_decapsulate(ctx.ptr(), shared_key.data(), &shared_key_size, ciphertext.data(), ciphertext.size()));

    return shared_key;
}

}
