/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/SHA1.h>

#include <openssl/evp.h>

namespace Crypto::Hash {

SHA1::SHA1(EVP_MD_CTX* context)
    : OpenSSLHashFunction(EVP_sha1(), context)
{
}

}
