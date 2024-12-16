/*
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <LibCrypto/Cipher/Mode/Mode.h>
#include <LibCrypto/Verification.h>

namespace Crypto::Cipher {

template<typename T>
class KW : public Mode<T> {
public:
    constexpr static size_t IVSizeInBits = 128;
    constexpr static u8 default_iv[8] = { 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6 };

    virtual ~KW() = default;
    template<typename... Args>
    explicit constexpr KW(Args... args)
        : Mode<T>(args...)
    {
    }

    virtual ByteString class_name() const override
    {
        StringBuilder builder;
        builder.append(this->cipher().class_name());
        builder.append("_KW"sv);
        return builder.to_byte_string();
    }

    virtual size_t IV_length() const override
    {
        return IVSizeInBits / 8;
    }

    // FIXME: This overload throws away the validation, think up a better way to return more than a single bytebuffer.
    virtual void encrypt(ReadonlyBytes in, Bytes& out, [[maybe_unused]] ReadonlyBytes ivec = {}, [[maybe_unused]] Bytes* ivec_out = nullptr) override
    {
        this->wrap(in, out);
    }

    virtual void decrypt(ReadonlyBytes in, Bytes& out, [[maybe_unused]] ReadonlyBytes ivec = {}) override
    {
        this->unwrap(in, out);
    }

    void wrap(ReadonlyBytes in, Bytes& out)
    {
        // The plaintext consists of n 64-bit blocks, containing the key data being wrapped.
        VERIFY(in.size() % 8 == 0);
        VERIFY(out.size() >= in.size() + 8);

        auto& cipher = this->cipher();

        auto iv = MUST(ByteBuffer::copy(default_iv, 8));
        auto data = MUST(ByteBuffer::copy(in));
        auto data_blocks = data.size() / 8;

        // For j = 0 to 5
        for (size_t j = 0; j < 6; ++j) {
            // For i=1 to n
            for (size_t i = 0; i < data_blocks; ++i) {
                // B = AES(K, A | R[i])
                m_cipher_block.bytes().overwrite(0, iv.data(), 8);
                m_cipher_block.bytes().overwrite(8, data.data() + i * 8, 8);
                cipher.encrypt_block(m_cipher_block, m_cipher_block);

                // A = MSB(64, B) ^ t where t = (n*j)+i
                u64 a = AK::convert_between_host_and_big_endian(ByteReader::load64(m_cipher_block.bytes().data())) ^ ((data_blocks * j) + i + 1);
                ByteReader::store(iv.data(), AK::convert_between_host_and_big_endian(a));

                // R[i] = LSB(64, B)
                data.overwrite(i * 8, m_cipher_block.bytes().data() + 8, 8);
            }
        }

        out.overwrite(0, iv.data(), 8);
        out.overwrite(8, data.data(), data.size());
    }

    VerificationConsistency unwrap(ReadonlyBytes in, Bytes& out)
    {
        // The inputs to the unwrap process are the KEK and (n+1) 64-bit blocks
        // of ciphertext consisting of previously wrapped key.
        VERIFY(in.size() % 8 == 0);
        VERIFY(in.size() > 8);

        // It returns n blocks of plaintext consisting of the n 64 - bit blocks of the decrypted key data.
        VERIFY(out.size() >= in.size() - 8);

        auto& cipher = this->cipher();

        auto iv = MUST(ByteBuffer::copy(in.slice(0, 8)));
        auto data = MUST(ByteBuffer::copy(in.slice(8, in.size() - 8)));
        auto data_blocks = data.size() / 8;

        // For j = 5 to 0
        for (size_t j = 6; j > 0; --j) {
            // For i = n to 1
            for (size_t i = data_blocks; i > 0; --i) {
                // B = AES-1(K, (A ^ t) | R[i]) where t = n*j+i
                u64 a = AK::convert_between_host_and_big_endian(ByteReader::load64(iv.data())) ^ ((data_blocks * (j - 1)) + i);
                ByteReader::store(m_cipher_block.bytes().data(), AK::convert_between_host_and_big_endian(a));
                m_cipher_block.bytes().overwrite(8, data.data() + ((i - 1) * 8), 8);
                cipher.decrypt_block(m_cipher_block, m_cipher_block);

                // A = MSB(64, B)
                iv.overwrite(0, m_cipher_block.bytes().data(), 8);

                // R[i] = LSB(64, B)
                data.overwrite((i - 1) * 8, m_cipher_block.bytes().data() + 8, 8);
            }
        }

        if (ReadonlyBytes { default_iv, 8 } != iv.bytes())
            return VerificationConsistency::Inconsistent;

        out.overwrite(0, data.data(), data.size());
        return VerificationConsistency::Consistent;
    }

private:
    typename T::BlockType m_cipher_block {};
};

}
