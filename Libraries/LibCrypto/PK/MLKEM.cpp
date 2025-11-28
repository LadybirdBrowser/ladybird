/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/PK/MLKEM.h>

#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>

namespace Crypto::PK {

static char const* mlkem_size_to_openssl_name(MLKEMSize size)
{
    switch (size) {
    case MLKEM512:
        return "ML-KEM-512";
    case MLKEM768:
        return "ML-KEM-768";
    case MLKEM1024:
        return "ML-KEM-1024";
    default:
        VERIFY_NOT_REACHED();
    }
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

    // We reserve enough memory for the key size to be able to fit them all
    auto pub = TRY(ByteBuffer::create_uninitialized(1568));
    auto priv = TRY(ByteBuffer::create_uninitialized(3168));
    seed = TRY(ByteBuffer::create_uninitialized(64));

    size_t priv_len, pub_len, seed_len;
    OPENSSL_TRY(EVP_PKEY_get_octet_string_param(key.ptr(), OSSL_PKEY_PARAM_ML_KEM_SEED, seed.data(), seed.size(), &seed_len));
    OPENSSL_TRY(EVP_PKEY_get_octet_string_param(key.ptr(), OSSL_PKEY_PARAM_PRIV_KEY, priv.data(), priv.size(), &priv_len));
    OPENSSL_TRY(EVP_PKEY_get_octet_string_param(key.ptr(), OSSL_PKEY_PARAM_PUB_KEY, pub.data(), pub.size(), &pub_len));

    pub.trim(pub_len, true);
    priv.trim(priv_len, true);

    return KeyPairType {
        { pub },
        { seed, pub, priv }
    };
}

}
