/*
 * Copyright (c) 2023, the SerenityOS developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibCrypto/Hash/HashFunction.h>
#include <LibCrypto/Hash/SHA2.h>

namespace Crypto::Hash {

namespace BLAKE2bConstants {
static constexpr auto blockbytes { 128 };
static constexpr auto hash_length { 64 };
};

class BLAKE2b final : public HashFunction<1024, 512> {
public:
    using HashFunction::update;

    BLAKE2b()
    {
        reset();
    }

    virtual void update(u8 const*, size_t) override;
    virtual DigestType digest() override;
    virtual DigestType peek() override;

    static DigestType hash(u8 const* data, size_t length)
    {
        BLAKE2b blake2b;
        blake2b.update(data, length);
        return blake2b.digest();
    }

    static DigestType hash(ByteBuffer const& buffer) { return hash(buffer.data(), buffer.size()); }
    static DigestType hash(StringView buffer) { return hash((u8 const*)buffer.characters_without_null_termination(), buffer.length()); }

    virtual ByteString class_name() const override
    {
        return "BLAKE2b";
    }

    virtual void reset() override
    {
        m_internal_state = {};
        // BLAKE2b uses the same initialization vector as SHA512.
        for (size_t i = 0; i < 8; ++i)
            m_internal_state.hash_state[i] = SHA512Constants::InitializationHashes[i];
        m_internal_state.hash_state[0] ^= 0x01010000 ^ (0 << 8) ^ BLAKE2bConstants::hash_length;
    }

private:
    static constexpr u8 BLAKE2bSigma[12][16] = {
        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
        { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
        { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
        { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
        { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
        { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
        { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
        { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
        { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
        { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
        { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 }
    };

    struct BLAKE2bState {
        u64 hash_state[8] {};
        u64 message_byte_offset[2] {};
        u64 is_at_last_block { 0 };
        u8 buffer[BLAKE2bConstants::blockbytes] = {};
        size_t buffer_length { 0 };
    };

    BLAKE2bState m_internal_state {};

    void mix(u64* work_vector, u64 a, u64 b, u64 c, u64 d, u64 x, u64 y);
    void increment_counter_by(u64 const amount);
    void transform(u8 const*);
};

};
