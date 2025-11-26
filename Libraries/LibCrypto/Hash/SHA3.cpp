/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/SHA3.h>

#include <openssl/evp.h>

namespace Crypto::Hash {

SHA3_256::SHA3_256(EVP_MD_CTX* context)
    : OpenSSLHashFunction(EVP_sha3_256(), context)
{
}

SHA3_384::SHA3_384(EVP_MD_CTX* context)
    : OpenSSLHashFunction(EVP_sha3_384(), context)
{
}

SHA3_512::SHA3_512(EVP_MD_CTX* context)
    : OpenSSLHashFunction(EVP_sha3_512(), context)
{
}

}
