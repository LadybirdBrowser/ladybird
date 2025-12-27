/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibCrypto/OpenSSLForward.h>

namespace Crypto::Hash {

class Argon2 {
    AK_MAKE_NONCOPYABLE(Argon2);

public:
    Argon2(char const* openssl_name);

    virtual ~Argon2()
    {
        EVP_KDF_free(m_kdf);
    }

    ErrorOr<ByteBuffer> derive_key(
        ReadonlyBytes message,
        ReadonlyBytes nonce,
        u32 parallelism,
        u32 memory,
        u32 passes,
        u32 version,
        Optional<ByteBuffer> secret_value,
        Optional<ByteBuffer> associated_data,
        u32 tag_length) const;

private:
    EVP_KDF* m_kdf;
};

}
