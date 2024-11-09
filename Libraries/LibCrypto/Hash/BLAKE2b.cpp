/*
 * Copyright (c) 2023, the SerenityOS developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <LibCrypto/Hash/BLAKE2b.h>

namespace Crypto::Hash {
constexpr static auto ROTRIGHT(u64 a, size_t b) { return (a >> b) | (a << (64 - b)); }

void BLAKE2b::update(u8 const* in, size_t inlen)
{
    if (inlen > 0) {
        size_t left = m_internal_state.buffer_length;
        size_t fill = BLAKE2bConstants::blockbytes - left;
        if (inlen > fill) {
            m_internal_state.buffer_length = 0;
            // Fill the buffer.
            __builtin_memcpy(m_internal_state.buffer + left, in, fill);

            increment_counter_by(BLAKE2bConstants::blockbytes);
            transform(m_internal_state.buffer);
            in += fill;
            inlen -= fill;
            while (inlen > BLAKE2bConstants::blockbytes) {
                increment_counter_by(BLAKE2bConstants::blockbytes);
                transform(in);
                in += BLAKE2bConstants::blockbytes;
                inlen -= BLAKE2bConstants::blockbytes;
            }
        }
        __builtin_memcpy(m_internal_state.buffer + m_internal_state.buffer_length, in, inlen);
        m_internal_state.buffer_length += inlen;
    }
}

BLAKE2b::DigestType BLAKE2b::peek()
{
    DigestType digest;
    increment_counter_by(m_internal_state.buffer_length);

    // Set this as the last block
    m_internal_state.is_at_last_block = UINT64_MAX;

    // Pad the buffer with zeros
    __builtin_memset(m_internal_state.buffer + m_internal_state.buffer_length, 0, BLAKE2bConstants::blockbytes - m_internal_state.buffer_length);
    transform(m_internal_state.buffer);

    for (size_t i = 0; i < 8; ++i)
        __builtin_memcpy(&digest.data[0] + sizeof(m_internal_state.hash_state[i]) * i, &m_internal_state.hash_state[i], sizeof(m_internal_state.hash_state[i]));

    return digest;
}

BLAKE2b::DigestType BLAKE2b::digest()
{
    auto digest = peek();
    reset();
    return digest;
}

void BLAKE2b::increment_counter_by(u64 const amount)
{
    m_internal_state.message_byte_offset[0] += amount;
    m_internal_state.message_byte_offset[1] += (m_internal_state.message_byte_offset[0] < amount);
}

void BLAKE2b::mix(u64* work_array, u64 a, u64 b, u64 c, u64 d, u64 x, u64 y)
{
    constexpr auto rotation_constant_1 = 32;
    constexpr auto rotation_constant_2 = 24;
    constexpr auto rotation_constant_3 = 16;
    constexpr auto rotation_constant_4 = 63;

    work_array[a] = work_array[a] + work_array[b] + x;
    work_array[d] = ROTRIGHT(work_array[d] ^ work_array[a], rotation_constant_1);
    work_array[c] = work_array[c] + work_array[d];
    work_array[b] = ROTRIGHT(work_array[b] ^ work_array[c], rotation_constant_2);
    work_array[a] = work_array[a] + work_array[b] + y;
    work_array[d] = ROTRIGHT(work_array[d] ^ work_array[a], rotation_constant_3);
    work_array[c] = work_array[c] + work_array[d];
    work_array[b] = ROTRIGHT(work_array[b] ^ work_array[c], rotation_constant_4);
}

void BLAKE2b::transform(u8 const* block)
{
    u64 m[16];
    u64 v[16];

    for (size_t i = 0; i < 16; ++i)
        m[i] = ByteReader::load64(block + i * sizeof(m[i]));

    for (size_t i = 0; i < 8; ++i)
        v[i] = m_internal_state.hash_state[i];

    v[8] = SHA512Constants::InitializationHashes[0];
    v[9] = SHA512Constants::InitializationHashes[1];
    v[10] = SHA512Constants::InitializationHashes[2];
    v[11] = SHA512Constants::InitializationHashes[3];
    v[12] = SHA512Constants::InitializationHashes[4] ^ m_internal_state.message_byte_offset[0];
    v[13] = SHA512Constants::InitializationHashes[5] ^ m_internal_state.message_byte_offset[1];
    v[14] = SHA512Constants::InitializationHashes[6] ^ m_internal_state.is_at_last_block;
    v[15] = SHA512Constants::InitializationHashes[7];

    for (size_t i = 0; i < 12; ++i) {
        u64 sigma_selection[16];
        for (size_t j = 0; j < 16; ++j)
            sigma_selection[j] = BLAKE2bSigma[i % 10][j];
        mix(v, 0, 4, 8, 12, m[sigma_selection[0]], m[sigma_selection[1]]);
        mix(v, 1, 5, 9, 13, m[sigma_selection[2]], m[sigma_selection[3]]);
        mix(v, 2, 6, 10, 14, m[sigma_selection[4]], m[sigma_selection[5]]);
        mix(v, 3, 7, 11, 15, m[sigma_selection[6]], m[sigma_selection[7]]);

        mix(v, 0, 5, 10, 15, m[sigma_selection[8]], m[sigma_selection[9]]);
        mix(v, 1, 6, 11, 12, m[sigma_selection[10]], m[sigma_selection[11]]);
        mix(v, 2, 7, 8, 13, m[sigma_selection[12]], m[sigma_selection[13]]);
        mix(v, 3, 4, 9, 14, m[sigma_selection[14]], m[sigma_selection[15]]);
    }

    for (size_t i = 0; i < 8; ++i)
        m_internal_state.hash_state[i] = m_internal_state.hash_state[i] ^ v[i] ^ v[i + 8];
}

}
