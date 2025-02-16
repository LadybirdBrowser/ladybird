/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/Hash/HashManager.h>

namespace Crypto::Hash {

class PBKDF2 {
public:
    PBKDF2(HashKind hash_kind);

    ~PBKDF2()
    {
        EVP_KDF_free(m_kdf);
    }

    ErrorOr<ByteBuffer> derive_key(ReadonlyBytes password, ReadonlyBytes salt, u32 iterations, u32 key_length_bytes);

private:
    EVP_KDF* m_kdf;
    HashKind m_hash_kind;
};

}
