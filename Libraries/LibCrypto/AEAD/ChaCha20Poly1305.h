/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Crypto::AEAD {

class ChaCha20Poly1305 {
public:
    explicit ChaCha20Poly1305(ReadonlyBytes key, ReadonlyBytes nonce)
    {
        m_key = MUST(ByteBuffer::copy(key));
        m_nonce = MUST(ByteBuffer::copy(nonce));
    }

    ErrorOr<ByteBuffer> encrypt(ReadonlyBytes aad, ReadonlyBytes plaintext);
    ErrorOr<ByteBuffer> decrypt(ReadonlyBytes aad, ReadonlyBytes ciphertext);
    ErrorOr<ByteBuffer> poly1305_key();
    static bool verify_tag(ReadonlyBytes encrypted, ReadonlyBytes decrypted);

private:
    u8 pad_to_16(ReadonlyBytes data)
    {
        return 16 - (data.size() % 16);
    }

    ByteBuffer m_key;
    ByteBuffer m_nonce;
};
}
