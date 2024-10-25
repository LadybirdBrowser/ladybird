/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <LibCrypto/Cipher/Mode/Mode.h>

namespace Crypto::Cipher {

template<typename T>
class CBC : public Mode<T> {
public:
    constexpr static size_t IVSizeInBits = 128;

    virtual ~CBC() = default;
    template<typename... Args>
    explicit constexpr CBC(Args... args)
        : Mode<T>(args...)
    {
    }

    virtual ByteString class_name() const override
    {
        StringBuilder builder;
        builder.append(this->cipher().class_name());
        builder.append("_CBC"sv);
        return builder.to_byte_string();
    }

    virtual size_t IV_length() const override
    {
        return IVSizeInBits / 8;
    }

    virtual void encrypt(ReadonlyBytes in, Bytes& out, ReadonlyBytes ivec = {}, Bytes* ivec_out = nullptr) override
    {
        auto length = in.size();
        if (length == 0)
            return;

        auto& cipher = this->cipher();

        // FIXME: We should have two of these encrypt/decrypt functions that
        //        we SFINAE out based on whether the Cipher mode needs an ivec
        VERIFY(!ivec.is_empty());
        ReadonlyBytes iv = ivec;

        m_cipher_block.set_padding_mode(cipher.padding_mode());
        size_t offset { 0 };
        auto block_size = cipher.block_size();

        while (length >= block_size) {
            m_cipher_block.overwrite(in.slice(offset, block_size));
            m_cipher_block.apply_initialization_vector(iv);
            cipher.encrypt_block(m_cipher_block, m_cipher_block);
            VERIFY(offset + block_size <= out.size());
            __builtin_memcpy(out.offset(offset), m_cipher_block.bytes().data(), block_size);
            iv = out.slice(offset);
            length -= block_size;
            offset += block_size;
        }

        if (length > 0 || padding_always_needs_extra_block(cipher.padding_mode())) {
            m_cipher_block.overwrite(in.slice(offset, length));
            m_cipher_block.apply_initialization_vector(iv);
            cipher.encrypt_block(m_cipher_block, m_cipher_block);
            VERIFY(offset + block_size <= out.size());
            __builtin_memcpy(out.offset(offset), m_cipher_block.bytes().data(), block_size);
            iv = out.slice(offset);
            offset += block_size;
        }

        if (ivec_out)
            __builtin_memcpy(ivec_out->data(), iv.data(), min(IV_length(), ivec_out->size()));

        // Indicate how much output was generated. (This can be non-trivial, as it depends on the padding mode.)
        out = out.slice(0, offset);
    }

    virtual void decrypt(ReadonlyBytes in, Bytes& out, ReadonlyBytes ivec = {}) override
    {
        auto length = in.size();
        if (length == 0)
            return;

        auto& cipher = this->cipher();

        VERIFY(!ivec.is_empty());
        ReadonlyBytes iv = ivec;

        auto block_size = cipher.block_size();

        // if the data is not aligned, it's not correct encrypted data
        // FIXME (ponder): Should we simply decrypt as much as we can?
        VERIFY(length % block_size == 0);

        m_cipher_block.set_padding_mode(cipher.padding_mode());
        size_t offset { 0 };

        while (length > 0) {
            auto slice = in.slice(offset);
            m_cipher_block.overwrite(slice.data(), block_size);
            cipher.decrypt_block(m_cipher_block, m_cipher_block);
            m_cipher_block.apply_initialization_vector(iv);
            auto decrypted = m_cipher_block.bytes();
            VERIFY(offset + decrypted.size() <= out.size());
            __builtin_memcpy(out.offset(offset), decrypted.data(), decrypted.size());
            iv = slice;
            length -= block_size;
            offset += block_size;
        }
        out = out.slice(0, offset);
        this->prune_padding(out);
    }

private:
    typename T::BlockType m_cipher_block {};
};

}
