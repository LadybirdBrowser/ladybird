/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibCrypto/Hash/HashFunction.h>

namespace Crypto::Hash {

namespace SHA1Constants {

constexpr static u32 InitializationHashes[5] { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };

constexpr static u32 RoundConstants[4] {
    0X5a827999,
    0X6ed9eba1,
    0X8f1bbcdc,
    0Xca62c1d6,
};

}

class SHA1 final : public HashFunction<512, 160> {
public:
    using HashFunction::update;

    SHA1()
    {
        reset();
    }

    virtual void update(u8 const*, size_t) override;

    virtual DigestType digest() override;
    virtual DigestType peek() override;

    static DigestType hash(u8 const* data, size_t length)
    {
        SHA1 sha;
        sha.update(data, length);
        return sha.digest();
    }

    static DigestType hash(ByteBuffer const& buffer) { return hash(buffer.data(), buffer.size()); }
    static DigestType hash(StringView buffer) { return hash((u8 const*)buffer.characters_without_null_termination(), buffer.length()); }

    virtual ByteString class_name() const override
    {
        return "SHA1";
    }

    virtual void reset() override
    {
        m_data_length = 0;
        m_bit_length = 0;
        for (auto i = 0; i < 5; ++i)
            m_state[i] = SHA1Constants::InitializationHashes[i];
    }

private:
    inline void transform(u8 const*);

    u8 m_data_buffer[BlockSize] {};
    size_t m_data_length { 0 };

    u64 m_bit_length { 0 };
    u32 m_state[5];

    constexpr static auto FinalBlockDataSize = BlockSize - 8;
    constexpr static auto Rounds = 80;
};

}
