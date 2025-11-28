/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/PK/MLKEM.h>

#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace Crypto::PK {

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
