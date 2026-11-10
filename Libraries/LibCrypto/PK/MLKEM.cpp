/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <LibCrypto/PK/MLKEM.h>

#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Curves/SECPxxxr1.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/PK/ModuleLatticePrivateKey.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace Crypto::PK {

void MLKEMPrivateKey::clear_sensitive_data()
{
    secure_zero(m_seed.data(), m_seed.size());
    secure_zero(m_private_key.data(), m_private_key.size());
}

static char const* mlkem_size_to_openssl_name(MLKEMSize size)
{
    switch (size) {
    case MLKEMSize::MLKEM512:
        return "ML-KEM-512";
    case MLKEMSize::MLKEM768:
        return "ML-KEM-768";
    case MLKEMSize::MLKEM1024:
        return "ML-KEM-1024";
    default:
        VERIFY_NOT_REACHED();
    }
}

static size_t mlkem_expanded_key_size(MLKEMSize size)
{
    switch (size) {
    case MLKEMSize::MLKEM512:
        return 1632;
    case MLKEMSize::MLKEM768:
        return 2400;
    case MLKEMSize::MLKEM1024:
        return 3168;
    default:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<ByteBuffer> MLKEMPrivateKey::export_as_der() const
{
    // NB: The key is exported in the seed-only form, which a key without its seed cannot be.
    if (m_seed.is_empty())
        return Error::from_string_literal("Cannot export an ML-KEM private key without its seed");

    ASN1::Encoder encoder;
    TRY(encoder.write<ReadonlyBytes>(m_seed, ASN1::Class::Context, static_cast<ASN1::Kind>(0)));

    return encoder.finish();
}

ErrorOr<MLKEM::KeyPairType> MLKEM::parse_mlkem_key(MLKEMSize size, ReadonlyBytes der)
{
    auto encoded_key = TRY(parse_module_lattice_private_key(der, 64, mlkem_expanded_key_size(size)));

    // NB: Web Crypto rejects a key in the expandedKey form before it validates the key, so that form is kept as it is.
    if (encoded_key.seed.is_empty())
        return KeyPairType { {}, { {}, {}, TRY(ByteBuffer::copy(encoded_key.expanded_key)) } };

    auto key_pair = TRY(generate_key_pair(size, TRY(ByteBuffer::copy(encoded_key.seed))));
    if (!encoded_key.expanded_key.is_empty() && key_pair.private_key.private_key() != encoded_key.expanded_key)
        return Error::from_string_literal("The expanded key does not match the seed");
    return key_pair;
}

ErrorOr<MLKEMEncapsulation> MLKEM::encapsulate(MLKEMSize size, MLKEMPublicKey const& key)
{
    auto public_key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_public_key_ex(nullptr, mlkem_size_to_openssl_name(size), nullptr, key.public_key().data(), key.public_key().size())));

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, public_key.ptr(), nullptr)));

    OPENSSL_TRY(EVP_PKEY_encapsulate_init(ctx.ptr(), nullptr));

    size_t shared_key_size;
    size_t ciphertext_length;
    OPENSSL_TRY(EVP_PKEY_encapsulate(ctx.ptr(), nullptr, &ciphertext_length, nullptr, &shared_key_size));

    auto shared_key = TRY(ByteBuffer::create_uninitialized(shared_key_size));
    auto ciphertext = TRY(ByteBuffer::create_uninitialized(ciphertext_length));

    OPENSSL_TRY(EVP_PKEY_encapsulate(ctx.ptr(), ciphertext.data(), &ciphertext_length, shared_key.data(), &shared_key_size));

    return MLKEMEncapsulation { shared_key, ciphertext };
}

static ErrorOr<OpenSSL_PKEY> private_key_to_openssl_pkey(MLKEMSize size, MLKEMPrivateKey const& private_key)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, mlkem_size_to_openssl_name(size), nullptr)));

    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx.ptr()));

    auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_ML_KEM_SEED, private_key.seed().data(), private_key.seed().size()));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, private_key.public_key().data(), private_key.public_key().size()));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_PRIV_KEY, private_key.private_key().data(), private_key.private_key().size()));

    auto* params = OSSL_PARAM_BLD_to_param(params_bld);
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto key = TRY(OpenSSL_PKEY::create());
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx.ptr(), &key_ptr, EVP_PKEY_KEYPAIR, params));

    return key;
}

ErrorOr<ByteBuffer> MLKEM::decapsulate(MLKEMSize size, MLKEMPrivateKey const& key, ByteBuffer ciphertext)
{
    auto private_key = TRY(private_key_to_openssl_pkey(size, key));

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, private_key.ptr(), nullptr)));

    OPENSSL_TRY(EVP_PKEY_decapsulate_init(ctx.ptr(), nullptr));

    size_t shared_key_size;
    OPENSSL_TRY(EVP_PKEY_decapsulate(ctx.ptr(), nullptr, &shared_key_size, ciphertext.data(), ciphertext.size()));

    auto shared_key = TRY(ByteBuffer::create_uninitialized(shared_key_size));
    OPENSSL_TRY(EVP_PKEY_decapsulate(ctx.ptr(), shared_key.data(), &shared_key_size, ciphertext.data(), ciphertext.size()));

    return shared_key;
}

ErrorOr<MLKEM::KeyPairType> MLKEM::generate_key_pair(MLKEMSize size, ByteBuffer seed)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, mlkem_size_to_openssl_name(size), nullptr)));

    OPENSSL_TRY(EVP_PKEY_keygen_init(ctx.ptr()));

    OSSL_PARAM params[2] = {
        OSSL_PARAM_END,
        OSSL_PARAM_END
    };

    if (!seed.is_empty()) {
        params[0] = OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ML_KEM_SEED, seed.data(), seed.size());
    }

    OPENSSL_TRY(EVP_PKEY_CTX_set_params(ctx.ptr(), params));
    auto key = TRY(OpenSSL_PKEY::create());
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_generate(ctx.ptr(), &key_ptr));

    auto pub = TRY(get_byte_buffer_param_from_key(key, OSSL_PKEY_PARAM_PUB_KEY));
    auto priv = TRY(get_byte_buffer_param_from_key(key, OSSL_PKEY_PARAM_PRIV_KEY));
    seed = TRY(get_byte_buffer_param_from_key(key, OSSL_PKEY_PARAM_ML_KEM_SEED));

    return KeyPairType {
        MLKEMPublicKey { pub },
        { seed, pub, priv }
    };
}

}
