/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Cipher/AES.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/evp.h>

namespace Crypto::Cipher {

#define GET_CIPHER(key, mode)            \
    [&key] {                             \
        switch (key.size()) {            \
        case 16:                         \
            return EVP_aes_128_##mode(); \
        case 24:                         \
            return EVP_aes_192_##mode(); \
        case 32:                         \
            return EVP_aes_256_##mode(); \
        default:                         \
            VERIFY_NOT_REACHED();        \
        }                                \
    }()

size_t AESCipher::block_size() const
{
    auto size = EVP_CIPHER_get_block_size(m_cipher);
    VERIFY(size != 0);
    return size;
}

AESCBCCipher::AESCBCCipher(ReadonlyBytes key, bool no_padding)
    : AESCipher(GET_CIPHER(key, cbc), key)
    , m_no_padding(no_padding)
{
}

ErrorOr<ByteBuffer> AESCBCCipher::encrypt(ReadonlyBytes plaintext, ReadonlyBytes iv) const
{
    auto ctx = TRY(OpenSSL_CIPHER_CTX::create());

    OPENSSL_TRY(EVP_EncryptInit(ctx.ptr(), m_cipher, m_key.data(), iv.data()));
    OPENSSL_TRY(EVP_CIPHER_CTX_set_padding(ctx.ptr(), m_no_padding ? 0 : 1));

    auto out = TRY(ByteBuffer::create_uninitialized(plaintext.size() + block_size()));
    int out_size = 0;
    OPENSSL_TRY(EVP_EncryptUpdate(ctx.ptr(), out.data(), &out_size, plaintext.data(), plaintext.size()));

    int final_size = 0;
    OPENSSL_TRY(EVP_EncryptFinal(ctx.ptr(), out.data() + out_size, &final_size));

    return out.slice(0, out_size + final_size);
}

ErrorOr<ByteBuffer> AESCBCCipher::decrypt(ReadonlyBytes ciphertext, ReadonlyBytes iv) const
{
    auto ctx = TRY(OpenSSL_CIPHER_CTX::create());

    OPENSSL_TRY(EVP_DecryptInit(ctx.ptr(), m_cipher, m_key.data(), iv.data()));
    OPENSSL_TRY(EVP_CIPHER_CTX_set_padding(ctx.ptr(), m_no_padding ? 0 : 1));

    auto out = TRY(ByteBuffer::create_uninitialized(ciphertext.size() + block_size()));
    int out_size = 0;
    OPENSSL_TRY(EVP_DecryptUpdate(ctx.ptr(), out.data(), &out_size, ciphertext.data(), ciphertext.size()));

    int final_size = 0;
    OPENSSL_TRY(EVP_DecryptFinal(ctx.ptr(), out.data() + out_size, &final_size));

    return out.slice(0, out_size + final_size);
}

AESCTRCipher::AESCTRCipher(ReadonlyBytes key)
    : AESCipher(GET_CIPHER(key, ctr), key)
{
}

ErrorOr<ByteBuffer> AESCTRCipher::encrypt(ReadonlyBytes plaintext, ReadonlyBytes iv) const
{
    auto ctx = TRY(OpenSSL_CIPHER_CTX::create());

    OPENSSL_TRY(EVP_EncryptInit(ctx.ptr(), m_cipher, m_key.data(), iv.data()));

    auto out = TRY(ByteBuffer::create_uninitialized(plaintext.size() + block_size()));
    int out_size = 0;
    OPENSSL_TRY(EVP_EncryptUpdate(ctx.ptr(), out.data(), &out_size, plaintext.data(), plaintext.size()));

    int final_size = 0;
    OPENSSL_TRY(EVP_EncryptFinal(ctx.ptr(), out.data() + out_size, &final_size));

    return out.slice(0, out_size + final_size);
}

ErrorOr<ByteBuffer> AESCTRCipher::decrypt(ReadonlyBytes ciphertext, ReadonlyBytes iv) const
{
    auto ctx = TRY(OpenSSL_CIPHER_CTX::create());

    OPENSSL_TRY(EVP_DecryptInit(ctx.ptr(), m_cipher, m_key.data(), iv.data()));

    auto out = TRY(ByteBuffer::create_uninitialized(ciphertext.size() + block_size()));
    int out_size = 0;
    OPENSSL_TRY(EVP_DecryptUpdate(ctx.ptr(), out.data(), &out_size, ciphertext.data(), ciphertext.size()));

    int final_size = 0;
    OPENSSL_TRY(EVP_DecryptFinal(ctx.ptr(), out.data() + out_size, &final_size));

    return out.slice(0, out_size + final_size);
}

AESGCMCipher::AESGCMCipher(ReadonlyBytes key)
    : AESCipher(GET_CIPHER(key, gcm), key)
{
}

ErrorOr<AESGCMCipher::EncryptedData> AESGCMCipher::encrypt(ReadonlyBytes plaintext, ReadonlyBytes iv, ReadonlyBytes aad, size_t taglen) const
{
    auto ctx = TRY(OpenSSL_CIPHER_CTX::create());

    OPENSSL_TRY(EVP_DecryptInit(ctx.ptr(), m_cipher, nullptr, nullptr));
    OPENSSL_TRY(EVP_CIPHER_CTX_ctrl(ctx.ptr(), EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr));

    OPENSSL_TRY(EVP_EncryptInit(ctx.ptr(), nullptr, m_key.data(), iv.data()));

    // To specify additional authenticated data (AAD), a call to EVP_CipherUpdate(), EVP_EncryptUpdate() or EVP_DecryptUpdate() should be made
    // with the output parameter out set to NULL.
    if (!aad.is_empty()) {
        int aad_size = 0;
        OPENSSL_TRY(EVP_EncryptUpdate(ctx.ptr(), nullptr, &aad_size, aad.data(), aad.size()));
    }

    auto out = TRY(ByteBuffer::create_uninitialized(plaintext.size() + block_size()));
    int out_size = 0;
    OPENSSL_TRY(EVP_EncryptUpdate(ctx.ptr(), out.data(), &out_size, plaintext.data(), plaintext.size()));

    int final_size = 0;
    OPENSSL_TRY(EVP_EncryptFinal(ctx.ptr(), out.data() + out_size, &final_size));

    auto tag = TRY(ByteBuffer::create_uninitialized(taglen));
    OPENSSL_TRY(EVP_CIPHER_CTX_ctrl(ctx.ptr(), EVP_CTRL_GCM_GET_TAG, taglen, tag.data()));

    return EncryptedData {
        .ciphertext = TRY(out.slice(0, out_size + final_size)),
        .tag = tag
    };
}

ErrorOr<ByteBuffer> AESGCMCipher::decrypt(ReadonlyBytes ciphertext, ReadonlyBytes iv, ReadonlyBytes aad, ReadonlyBytes tag) const
{
    auto ctx = TRY(OpenSSL_CIPHER_CTX::create());

    OPENSSL_TRY(EVP_DecryptInit(ctx.ptr(), m_cipher, nullptr, nullptr));
    OPENSSL_TRY(EVP_CIPHER_CTX_ctrl(ctx.ptr(), EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr));

    OPENSSL_TRY(EVP_DecryptInit(ctx.ptr(), nullptr, m_key.data(), iv.data()));
    OPENSSL_TRY(EVP_CIPHER_CTX_ctrl(ctx.ptr(), EVP_CTRL_GCM_SET_TAG, tag.size(), const_cast<u8*>(tag.data())));

    // To specify additional authenticated data (AAD), a call to EVP_CipherUpdate(), EVP_EncryptUpdate() or EVP_DecryptUpdate() should be made
    // with the output parameter out set to NULL.
    if (!aad.is_empty()) {
        int aad_size = 0;
        OPENSSL_TRY(EVP_DecryptUpdate(ctx.ptr(), nullptr, &aad_size, aad.data(), aad.size()));
    }

    auto out = TRY(ByteBuffer::create_uninitialized(ciphertext.size() + block_size()));
    int out_size = 0;
    OPENSSL_TRY(EVP_DecryptUpdate(ctx.ptr(), out.data(), &out_size, ciphertext.data(), ciphertext.size()));

    int final_size = 0;
    OPENSSL_TRY(EVP_DecryptFinal(ctx.ptr(), out.data() + out_size, &final_size));

    return out.slice(0, out_size + final_size);
}

AESKWCipher::AESKWCipher(ReadonlyBytes key)
    : AESCipher(GET_CIPHER(key, wrap), key)
{
}

ErrorOr<ByteBuffer> AESKWCipher::wrap(ReadonlyBytes plaintext) const
{
    auto ctx = TRY(OpenSSL_CIPHER_CTX::create());

    OPENSSL_TRY(EVP_EncryptInit(ctx.ptr(), m_cipher, m_key.data(), nullptr));

    auto out = TRY(ByteBuffer::create_uninitialized(plaintext.size() + block_size()));
    int out_size = 0;
    OPENSSL_TRY(EVP_EncryptUpdate(ctx.ptr(), out.data(), &out_size, plaintext.data(), plaintext.size()));

    int final_size = 0;
    OPENSSL_TRY(EVP_EncryptFinal(ctx.ptr(), out.data() + out_size, &final_size));

    return out.slice(0, out_size + final_size);
}

ErrorOr<ByteBuffer> AESKWCipher::unwrap(ReadonlyBytes ciphertext) const
{
    auto ctx = TRY(OpenSSL_CIPHER_CTX::create());

    OPENSSL_TRY(EVP_DecryptInit(ctx.ptr(), m_cipher, m_key.data(), nullptr));

    auto out = TRY(ByteBuffer::create_uninitialized(ciphertext.size() + block_size()));
    int out_size = 0;
    OPENSSL_TRY(EVP_DecryptUpdate(ctx.ptr(), out.data(), &out_size, ciphertext.data(), ciphertext.size()));

    int final_size = 0;
    OPENSSL_TRY(EVP_DecryptFinal(ctx.ptr(), out.data() + out_size, &final_size));

    return out.slice(0, out_size + final_size);
}

}
