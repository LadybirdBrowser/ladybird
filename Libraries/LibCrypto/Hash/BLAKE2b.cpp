/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/BLAKE2b.h>

#include <openssl/evp.h>

namespace Crypto::Hash {

BLAKE2b::BLAKE2b(EVP_MD_CTX* context)
    : OpenSSLHashFunction(EVP_blake2b512(), context)
{
}

}
