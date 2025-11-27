/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibCrypto/OpenSSLForward.h>

namespace Crypto::Hash {

enum class Argon2Type {
    Argon2d,
    Argon2i,
    Argon2id
};

class Argon2 {
    AK_MAKE_NONCOPYABLE(Argon2);

public:
    explicit Argon2(Argon2Type);

    ~Argon2();

    ErrorOr<ByteBuffer> derive_key(
        ReadonlyBytes message,
        ReadonlyBytes nonce,
        u32 parallelism,
        u32 memory,
        u32 passes,
        u32 version,
        Optional<ReadonlyBytes> secret_value,
        Optional<ReadonlyBytes> associated_data,
        u32 tag_length) const;

private:
    EVP_KDF* m_kdf;
};

}
