/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibCrypto/OpenSSLForward.h>

namespace Crypto::Cipher {

class AESCipher {
public:
    size_t block_size() const;

protected:
    explicit AESCipher(EVP_CIPHER const* cipher, ReadonlyBytes key)
        : m_cipher(cipher)
        , m_key(key)
    {
    }

    EVP_CIPHER const* m_cipher;
    ReadonlyBytes m_key;
};

class AESCBCCipher final : public AESCipher {
public:
    explicit AESCBCCipher(ReadonlyBytes key, bool no_padding = false);

    ErrorOr<ByteBuffer> encrypt(ReadonlyBytes plaintext, ReadonlyBytes iv) const;
    ErrorOr<ByteBuffer> decrypt(ReadonlyBytes ciphertext, ReadonlyBytes iv) const;

private:
    bool m_no_padding { false };
};

class AESCTRCipher final : public AESCipher {
public:
    explicit AESCTRCipher(ReadonlyBytes key);

    ErrorOr<ByteBuffer> encrypt(ReadonlyBytes plaintext, ReadonlyBytes iv) const;
    ErrorOr<ByteBuffer> decrypt(ReadonlyBytes ciphertext, ReadonlyBytes iv) const;
};

class AESGCMCipher final : public AESCipher {
public:
    explicit AESGCMCipher(ReadonlyBytes key);

    struct EncryptedData {
        ByteBuffer ciphertext;
        ByteBuffer tag;
    };

    ErrorOr<EncryptedData> encrypt(ReadonlyBytes plaintext, ReadonlyBytes iv, ReadonlyBytes aad, size_t taglen) const;
    ErrorOr<ByteBuffer> decrypt(ReadonlyBytes ciphertext, ReadonlyBytes iv, ReadonlyBytes aad, ReadonlyBytes tag) const;
};

class AESKWCipher final : public AESCipher {
public:
    explicit AESKWCipher(ReadonlyBytes key);

    ErrorOr<ByteBuffer> wrap(ReadonlyBytes) const;
    ErrorOr<ByteBuffer> unwrap(ReadonlyBytes) const;
};

}
