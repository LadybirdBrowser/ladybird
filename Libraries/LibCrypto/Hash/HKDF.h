/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/Hash/HashManager.h>

namespace Crypto::Hash {

class HKDF {
public:
    HKDF(HashKind hash_kind);

    ~HKDF()
    {
        EVP_KDF_free(m_kdf);
    }

    // Note: The output is different for a salt of length zero and an absent salt,
    // so Optional<ReadonlyBytes> really is the correct type.
    ErrorOr<ByteBuffer> derive_key(Optional<ReadonlyBytes> maybe_salt, ReadonlyBytes input_keying_material, ReadonlyBytes info, u32 key_length_bytes);

private:
    EVP_KDF* m_kdf;
    HashKind m_hash_kind;
};

}
