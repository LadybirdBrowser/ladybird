/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/MD5.h>

#include <openssl/evp.h>

namespace Crypto::Hash {

MD5::MD5(EVP_MD_CTX* context)
    : OpenSSLHashFunction(EVP_md5(), context)
{
}

}
