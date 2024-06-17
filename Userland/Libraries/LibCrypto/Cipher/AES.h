/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Vector.h>
#include <LibCrypto/Cipher/Cipher.h>
#include <LibCrypto/Cipher/Mode/CBC.h>
#include <LibCrypto/Cipher/Mode/CTR.h>
#include <LibCrypto/Cipher/Mode/GCM.h>

namespace Crypto::Cipher {

struct AESCipherBlock : public CipherBlock {
public:
    static constexpr size_t BlockSizeInBits = 128;

    explicit AESCipherBlock(PaddingMode mode = PaddingMode::CMS)
        : CipherBlock(mode)
    {
    }
    AESCipherBlock(u8 const* data, size_t length, PaddingMode mode = PaddingMode::CMS)
        : AESCipherBlock(mode)
    {
        CipherBlock::overwrite(data, length);
    }

    constexpr static size_t block_size() { return BlockSizeInBits / 8; }

    virtual ReadonlyBytes bytes() const override { return ReadonlyBytes { m_data, sizeof(m_data) }; }
    virtual Bytes bytes() override { return Bytes { m_data, sizeof(m_data) }; }

    virtual void overwrite(ReadonlyBytes) override;
    virtual void overwrite(u8 const* data, size_t size) override { overwrite({ data, size }); }

    virtual void apply_initialization_vector(ReadonlyBytes ivec) override
    {
        for (size_t i = 0; i < min(block_size(), ivec.size()); ++i)
            m_data[i] ^= ivec[i];
    }

    ByteString to_byte_string() const;

private:
    constexpr static size_t data_size() { return sizeof(m_data); }

    u8 m_data[BlockSizeInBits / 8] {};
};

struct AESCipherKey : public CipherKey {
    virtual ReadonlyBytes bytes() const override { return ReadonlyBytes { m_rd_keys, sizeof(m_rd_keys) }; }
    virtual void expand_encrypt_key(ReadonlyBytes user_key, size_t bits) override;
    virtual void expand_decrypt_key(ReadonlyBytes user_key, size_t bits) override;
    static bool is_valid_key_size(size_t bits) { return bits == 128 || bits == 192 || bits == 256; }

    ByteString to_byte_string() const;

    u32 const* round_keys() const
    {
        return (u32 const*)m_rd_keys;
    }

    AESCipherKey(ReadonlyBytes user_key, size_t key_bits, Intent intent)
        : m_bits(key_bits)
    {
        if (intent == Intent::Encryption)
            expand_encrypt_key(user_key, key_bits);
        else
            expand_decrypt_key(user_key, key_bits);
    }

    virtual ~AESCipherKey() override = default;

    size_t rounds() const { return m_rounds; }
    size_t length() const { return m_bits / 8; }

protected:
    u32* round_keys()
    {
        return (u32*)m_rd_keys;
    }

private:
    static constexpr size_t MAX_ROUND_COUNT = 14;
    u32 m_rd_keys[(MAX_ROUND_COUNT + 1) * 4] { 0 };
    size_t m_rounds;
    size_t m_bits;
};

class AESCipher final : public Cipher<AESCipherKey, AESCipherBlock> {
public:
    using CBCMode = CBC<AESCipher>;
    using CTRMode = CTR<AESCipher>;
    using GCMMode = GCM<AESCipher>;

    constexpr static size_t BlockSizeInBits = BlockType::BlockSizeInBits;

    AESCipher(ReadonlyBytes user_key, size_t key_bits, Intent intent = Intent::Encryption, PaddingMode mode = PaddingMode::CMS)
        : Cipher<AESCipherKey, AESCipherBlock>(mode)
        , m_key(user_key, key_bits, intent)
    {
    }

    virtual AESCipherKey const& key() const override { return m_key; }
    virtual AESCipherKey& key() override { return m_key; }

    virtual void encrypt_block(BlockType const& in, BlockType& out) override;
    virtual void decrypt_block(BlockType const& in, BlockType& out) override;

    virtual ByteString class_name() const override
    {
        return "AES";
    }

protected:
    AESCipherKey m_key;
};

}
