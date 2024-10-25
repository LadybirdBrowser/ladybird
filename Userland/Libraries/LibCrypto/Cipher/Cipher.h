/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Types.h>

namespace Crypto::Cipher {

enum class Intent {
    Encryption,
    Decryption,
};

enum class PaddingMode {
    CMS,     // RFC 1423
    RFC5246, // very similar to CMS, but filled with |length - 1|, instead of |length|
    Null,
    ZeroLength,
    // FIXME: We do not implement these yet
    Bit,
    Random,
    Space,
};

bool padding_always_needs_extra_block(PaddingMode);

template<typename B, typename T>
class Cipher;

struct CipherBlock {
public:
    explicit CipherBlock(PaddingMode mode)
        : m_padding_mode(mode)
    {
    }

    virtual ReadonlyBytes bytes() const = 0;

    virtual void overwrite(ReadonlyBytes) = 0;
    virtual void overwrite(u8 const* data, size_t size) { overwrite({ data, size }); }

    virtual void apply_initialization_vector(ReadonlyBytes ivec) = 0;

    PaddingMode padding_mode() const { return m_padding_mode; }
    void set_padding_mode(PaddingMode mode) { m_padding_mode = mode; }

    template<typename T>
    void put(size_t offset, T value)
    {
        VERIFY(offset + sizeof(T) <= bytes().size());
        auto* ptr = bytes().offset_pointer(offset);
        auto index { 0 };

        VERIFY(sizeof(T) <= 4);

        if constexpr (sizeof(T) > 3)
            ptr[index++] = (u8)(value >> 24);

        if constexpr (sizeof(T) > 2)
            ptr[index++] = (u8)(value >> 16);

        if constexpr (sizeof(T) > 1)
            ptr[index++] = (u8)(value >> 8);

        ptr[index] = (u8)value;
    }

protected:
    virtual ~CipherBlock() = default;

private:
    virtual Bytes bytes() = 0;
    PaddingMode m_padding_mode;
};

struct CipherKey {
    virtual ReadonlyBytes bytes() const = 0;
    static bool is_valid_key_size(size_t) { return false; }

    virtual ~CipherKey() = default;

protected:
    virtual void expand_encrypt_key(ReadonlyBytes user_key, size_t bits) = 0;
    virtual void expand_decrypt_key(ReadonlyBytes user_key, size_t bits) = 0;
    size_t bits { 0 };
};

template<typename KeyT = CipherKey, typename BlockT = CipherBlock>
class Cipher {
public:
    using KeyType = KeyT;
    using BlockType = BlockT;

    explicit Cipher(PaddingMode mode)
        : m_padding_mode(mode)
    {
    }

    virtual KeyType const& key() const = 0;
    virtual KeyType& key() = 0;

    constexpr static size_t block_size() { return BlockType::block_size(); }

    PaddingMode padding_mode() const { return m_padding_mode; }

    virtual void encrypt_block(BlockType const& in, BlockType& out) = 0;
    virtual void decrypt_block(BlockType const& in, BlockType& out) = 0;

    virtual ByteString class_name() const = 0;

protected:
    virtual ~Cipher() = default;

private:
    PaddingMode m_padding_mode;
};
}
