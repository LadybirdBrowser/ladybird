/*
 * Copyright (c) 2026, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>

namespace Crypto::Cipher {

class ChaCha20Poly1305 {
public:
    // 256 bits
    static constexpr size_t key_size = 32;

    // 96 bits
    static constexpr size_t nonce_size = 12;

    // 128 bits
    static constexpr size_t tag_size = 16;

    static ErrorOr<ByteBuffer> encrypt(
        ReadonlyBytes key,
        ReadonlyBytes nonce,
        ReadonlyBytes plaintext,
        ReadonlyBytes aad);

    static ErrorOr<ByteBuffer> decrypt(
        ReadonlyBytes key,
        ReadonlyBytes nonce,
        ReadonlyBytes ciphertext_and_tag,
        ReadonlyBytes aad);
};

}
