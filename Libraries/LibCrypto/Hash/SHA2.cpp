/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/SHA2.h>

#include <openssl/evp.h>

namespace Crypto::Hash {

SHA256::SHA256(EVP_MD_CTX* context)
    : OpenSSLHashFunction(EVP_sha256(), context)
{
}

SHA384::SHA384(EVP_MD_CTX* context)
    : OpenSSLHashFunction(EVP_sha384(), context)
{
}

SHA512::SHA512(EVP_MD_CTX* context)
    : OpenSSLHashFunction(EVP_sha512(), context)
{
}

}
