/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <LibCrypto/PK/MLDSA.h>

#include <LibCrypto/Curves/SECPxxxr1.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/PK/ModuleLatticePrivateKey.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace Crypto::PK {

void MLDSAPrivateKey::clear_sensitive_data()
{
    secure_zero(m_seed.data(), m_seed.size());
    secure_zero(m_private_key.data(), m_private_key.size());
}

namespace {

char const* mldsa_size_to_openssl_name(MLDSASize size)
{
    switch (size) {
    case MLDSA44:
        return "ML-DSA-44";
    case MLDSA65:
        return "ML-DSA-65";
    case MLDSA87:
        return "ML-DSA-87";
    default:
        VERIFY_NOT_REACHED();
    }
}

}

static size_t mldsa_expanded_key_size(MLDSASize size)
{
    switch (size) {
    case MLDSA44:
        return 2560;
    case MLDSA65:
        return 4032;
    case MLDSA87:
        return 4896;
    default:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<ByteBuffer> MLDSAPrivateKey::export_as_der() const
{
    // NB: The key is exported in the seed-only form, which a key without its seed cannot be.
    if (m_seed.is_empty())
        return Error::from_string_literal("Cannot export an ML-DSA private key without its seed");

    ASN1::Encoder encoder;
    TRY(encoder.write<ReadonlyBytes>(m_seed, ASN1::Class::Context, static_cast<ASN1::Kind>(0)));

    return encoder.finish();
}

ErrorOr<MLDSA::KeyPairType> MLDSA::parse_mldsa_key(MLDSASize size, ReadonlyBytes der)
{
    auto encoded_key = TRY(parse_module_lattice_private_key(der, 32, mldsa_expanded_key_size(size)));

    // NB: Web Crypto rejects a key in the expandedKey form before it validates the key, so that form is kept as it is.
    if (encoded_key.seed.is_empty())
        return KeyPairType { {}, { {}, {}, TRY(ByteBuffer::copy(encoded_key.expanded_key)) } };

    auto key_pair = TRY(generate_key_pair(size, TRY(ByteBuffer::copy(encoded_key.seed))));
    if (!encoded_key.expanded_key.is_empty() && key_pair.private_key.private_key() != encoded_key.expanded_key)
        return Error::from_string_literal("The expanded key does not match the seed");
    return key_pair;
}

ErrorOr<MLDSA::KeyPairType> MLDSA::generate_key_pair(MLDSASize size, ByteBuffer seed)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, mldsa_size_to_openssl_name(size), nullptr)));

    OPENSSL_TRY(EVP_PKEY_keygen_init(ctx.ptr()));

    OSSL_PARAM params[2] = {
        OSSL_PARAM_END,
        OSSL_PARAM_END
    };

    if (!seed.is_empty()) {
        params[0] = OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ML_DSA_SEED, seed.data(), seed.size());
    }

    OPENSSL_TRY(EVP_PKEY_CTX_set_params(ctx.ptr(), params));
    auto key = TRY(OpenSSL_PKEY::create());
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_generate(ctx.ptr(), &key_ptr));

    auto pub = TRY(get_byte_buffer_param_from_key(key, OSSL_PKEY_PARAM_PUB_KEY));
    auto priv = TRY(get_byte_buffer_param_from_key(key, OSSL_PKEY_PARAM_PRIV_KEY));
    seed = TRY(get_byte_buffer_param_from_key(key, OSSL_PKEY_PARAM_ML_DSA_SEED));

    return KeyPairType {
        { pub },
        { seed, pub, priv }
    };
}

static ErrorOr<OpenSSL_PKEY> private_key_to_openssl_pkey(MLDSASize size, MLDSAPrivateKey const& private_key)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, mldsa_size_to_openssl_name(size), nullptr)));

    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx.ptr()));

    auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_ML_DSA_SEED, private_key.seed().data(), private_key.seed().size()));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_PUB_KEY, private_key.public_key().data(), private_key.public_key().size()));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_PRIV_KEY, private_key.private_key().data(), private_key.private_key().size()));

    auto* params = OSSL_PARAM_BLD_to_param(params_bld);
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto key = TRY(OpenSSL_PKEY::create());
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx.ptr(), &key_ptr, EVP_PKEY_KEYPAIR, params));

    return key;
}

ErrorOr<ByteBuffer> MLDSA::sign(ReadonlyBytes message)
{
    OSSL_PARAM params[2] = {
        OSSL_PARAM_END,
        OSSL_PARAM_END
    };

    if (!m_context.is_empty()) {
        params[0] = OSSL_PARAM_octet_string(OSSL_SIGNATURE_PARAM_CONTEXT_STRING, m_context.data(), m_context.size());
    }

    auto key = TRY(private_key_to_openssl_pkey(m_size, m_private_key));
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, mldsa_size_to_openssl_name(m_size), nullptr)));

    EVP_PKEY_CTX* sign_ctx = OPENSSL_TRY_PTR(EVP_PKEY_CTX_new_from_pkey(nullptr, key.ptr(), nullptr));
    ScopeGuard const free_sign_ctx = [&] { EVP_PKEY_CTX_free(sign_ctx); };
    auto* sign_algorithm = OPENSSL_TRY_PTR(EVP_SIGNATURE_fetch(nullptr, mldsa_size_to_openssl_name(m_size), nullptr));
    ScopeGuard const free_sign_algorithm = [&] { EVP_SIGNATURE_free(sign_algorithm); };

    size_t sign_size;
    OPENSSL_TRY(EVP_PKEY_sign_message_init(sign_ctx, sign_algorithm, params));
    OPENSSL_TRY(EVP_PKEY_sign(sign_ctx, nullptr, &sign_size, message.data(), message.size()));

    auto result = TRY(ByteBuffer::create_uninitialized(sign_size));
    OPENSSL_TRY(EVP_PKEY_sign(sign_ctx, result.data(), &sign_size, message.data(), message.size()));

    return result;
}

static ErrorOr<OpenSSL_PKEY> public_key_to_openssl_pkey(MLDSASize size, MLDSAPublicKey const& public_key)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, mldsa_size_to_openssl_name(size), nullptr)));

    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx.ptr()));

    auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_PUB_KEY, public_key.public_key().data(), public_key.public_key().size()));

    auto* params = OSSL_PARAM_BLD_to_param(params_bld);
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto key = TRY(OpenSSL_PKEY::create());
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx.ptr(), &key_ptr, EVP_PKEY_KEYPAIR, params));

    return key;
}

ErrorOr<bool> MLDSA::verify(ReadonlyBytes message, ReadonlyBytes signature)
{
    OSSL_PARAM params[2] = {
        OSSL_PARAM_END,
        OSSL_PARAM_END
    };

    if (!m_context.is_empty()) {
        params[0] = OSSL_PARAM_octet_string(OSSL_SIGNATURE_PARAM_CONTEXT_STRING, m_context.data(), m_context.size());
    }

    auto key = TRY(public_key_to_openssl_pkey(m_size, m_public_key));
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new(key.ptr(), nullptr)));
    auto* sign_algorithm = OPENSSL_TRY_PTR(EVP_SIGNATURE_fetch(nullptr, mldsa_size_to_openssl_name(m_size), nullptr));
    ScopeGuard const free_sign_algorithm = [&] { EVP_SIGNATURE_free(sign_algorithm); };

    OPENSSL_TRY(EVP_PKEY_verify_message_init(ctx.ptr(), sign_algorithm, params));

    auto ret = EVP_PKEY_verify(ctx.ptr(), signature.data(), signature.size(), message.data(), message.size());
    if (ret == 1)
        return true;
    if (ret == 0)
        return false;
    OPENSSL_TRY(ret);
    VERIFY_NOT_REACHED();
}

}
