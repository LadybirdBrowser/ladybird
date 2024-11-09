/*
 * Copyright (c) 2022, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/Endian.h>
#include <LibCrypto/Authentication/Poly1305.h>

namespace Crypto::Authentication {

Poly1305::Poly1305(ReadonlyBytes key)
{
    for (size_t i = 0; i < 16; i += 4) {
        m_state.r[i / 4] = AK::convert_between_host_and_little_endian(ByteReader::load32(key.offset(i)));
    }

    // r[3], r[7], r[11], and r[15] are required to have their top four bits clear (be smaller than 16)
    // r[4], r[8], and r[12] are required to have their bottom two bits clear (be divisible by 4)
    m_state.r[0] &= 0x0FFFFFFF;
    m_state.r[1] &= 0x0FFFFFFC;
    m_state.r[2] &= 0x0FFFFFFC;
    m_state.r[3] &= 0x0FFFFFFC;

    for (size_t i = 16; i < 32; i += 4) {
        m_state.s[(i - 16) / 4] = AK::convert_between_host_and_little_endian(ByteReader::load32(key.offset(i)));
    }
}

void Poly1305::update(ReadonlyBytes message)
{
    size_t offset = 0;
    while (offset < message.size()) {
        u32 n = min(message.size() - offset, 16 - m_state.block_count);
        memcpy(m_state.blocks + m_state.block_count, message.offset_pointer(offset), n);
        m_state.block_count += n;
        offset += n;

        if (m_state.block_count == 16) {
            process_block();
            m_state.block_count = 0;
        }
    }
}

void Poly1305::process_block()
{
    u32 a[5];
    u8 n = m_state.block_count;

    // Add one bit beyond the number of octets.  For a 16-byte block,
    // this is equivalent to adding 2^128 to the number.  For the shorter
    // block, it can be 2^120, 2^112, or any power of two that is evenly
    // divisible by 8, all the way down to 2^8.
    m_state.blocks[n++] = 0x01;

    // If the block is not 17 bytes long (the last block), pad it with zeros.
    // This is meaningless if you are treating the blocks as numbers.
    while (n < 17) {
        m_state.blocks[n++] = 0x00;
    }

    // Read the block as a little-endian number.
    for (size_t i = 0; i < 16; i += 4) {
        a[i / 4] = AK::convert_between_host_and_little_endian(ByteReader::load32(m_state.blocks + i));
    }
    a[4] = m_state.blocks[16];

    // Add this number to the accumulator.
    m_state.a[0] += a[0];
    m_state.a[1] += a[1];
    m_state.a[2] += a[2];
    m_state.a[3] += a[3];
    m_state.a[4] += a[4];

    // Carry
    m_state.a[1] += m_state.a[0] >> 32;
    m_state.a[2] += m_state.a[1] >> 32;
    m_state.a[3] += m_state.a[2] >> 32;
    m_state.a[4] += m_state.a[3] >> 32;

    // Only consider the least significant bits
    a[0] = m_state.a[0] & 0xFFFFFFFF;
    a[1] = m_state.a[1] & 0xFFFFFFFF;
    a[2] = m_state.a[2] & 0xFFFFFFFF;
    a[3] = m_state.a[3] & 0xFFFFFFFF;
    a[4] = m_state.a[4] & 0xFFFFFFFF;

    // Multiply by r
    m_state.a[0] = (u64)a[0] * m_state.r[0];
    m_state.a[1] = (u64)a[0] * m_state.r[1] + (u64)a[1] * m_state.r[0];
    m_state.a[2] = (u64)a[0] * m_state.r[2] + (u64)a[1] * m_state.r[1] + (u64)a[2] * m_state.r[0];
    m_state.a[3] = (u64)a[0] * m_state.r[3] + (u64)a[1] * m_state.r[2] + (u64)a[2] * m_state.r[1] + (u64)a[3] * m_state.r[0];
    m_state.a[4] = (u64)a[1] * m_state.r[3] + (u64)a[2] * m_state.r[2] + (u64)a[3] * m_state.r[1] + (u64)a[4] * m_state.r[0];
    m_state.a[5] = (u64)a[2] * m_state.r[3] + (u64)a[3] * m_state.r[2] + (u64)a[4] * m_state.r[1];
    m_state.a[6] = (u64)a[3] * m_state.r[3] + (u64)a[4] * m_state.r[2];
    m_state.a[7] = (u64)a[4] * m_state.r[3];

    // Carry
    m_state.a[1] += m_state.a[0] >> 32;
    m_state.a[2] += m_state.a[1] >> 32;
    m_state.a[3] += m_state.a[2] >> 32;
    m_state.a[4] += m_state.a[3] >> 32;
    m_state.a[5] += m_state.a[4] >> 32;
    m_state.a[6] += m_state.a[5] >> 32;
    m_state.a[7] += m_state.a[6] >> 32;

    // Save the high part of the accumulator
    a[0] = m_state.a[4] & 0xFFFFFFFC;
    a[1] = m_state.a[5] & 0xFFFFFFFF;
    a[2] = m_state.a[6] & 0xFFFFFFFF;
    a[3] = m_state.a[7] & 0xFFFFFFFF;

    // Only consider the least significant bits
    m_state.a[0] &= 0xFFFFFFFF;
    m_state.a[1] &= 0xFFFFFFFF;
    m_state.a[2] &= 0xFFFFFFFF;
    m_state.a[3] &= 0xFFFFFFFF;
    m_state.a[4] &= 0x00000003;

    // Fast modular reduction (first pass)
    m_state.a[0] += a[0];
    m_state.a[0] += (a[0] >> 2) | (a[1] << 30);
    m_state.a[1] += a[1];
    m_state.a[1] += (a[1] >> 2) | (a[2] << 30);
    m_state.a[2] += a[2];
    m_state.a[2] += (a[2] >> 2) | (a[3] << 30);
    m_state.a[3] += a[3];
    m_state.a[3] += (a[3] >> 2);

    // Carry
    m_state.a[1] += m_state.a[0] >> 32;
    m_state.a[2] += m_state.a[1] >> 32;
    m_state.a[3] += m_state.a[2] >> 32;
    m_state.a[4] += m_state.a[3] >> 32;

    // Save the high part of the accumulator
    a[0] = m_state.a[4] & 0xFFFFFFFC;

    // Only consider the least significant bits
    m_state.a[0] &= 0xFFFFFFFF;
    m_state.a[1] &= 0xFFFFFFFF;
    m_state.a[2] &= 0xFFFFFFFF;
    m_state.a[3] &= 0xFFFFFFFF;
    m_state.a[4] &= 0x00000003;

    // Fast modular reduction (second pass)
    m_state.a[0] += a[0];
    m_state.a[0] += a[0] >> 2;

    // Carry
    m_state.a[1] += m_state.a[0] >> 32;
    m_state.a[2] += m_state.a[1] >> 32;
    m_state.a[3] += m_state.a[2] >> 32;
    m_state.a[4] += m_state.a[3] >> 32;

    // Only consider the least significant bits
    m_state.a[0] &= 0xFFFFFFFF;
    m_state.a[1] &= 0xFFFFFFFF;
    m_state.a[2] &= 0xFFFFFFFF;
    m_state.a[3] &= 0xFFFFFFFF;
    m_state.a[4] &= 0x00000003;
}

ErrorOr<ByteBuffer> Poly1305::digest()
{
    if (m_state.block_count != 0)
        process_block();

    u32 b[4];

    // Save the accumulator
    b[0] = m_state.a[0] & 0xFFFFFFFF;
    b[1] = m_state.a[1] & 0xFFFFFFFF;
    b[2] = m_state.a[2] & 0xFFFFFFFF;
    b[3] = m_state.a[3] & 0xFFFFFFFF;

    // Compute a + 5
    m_state.a[0] += 5;

    // Carry
    m_state.a[1] += m_state.a[0] >> 32;
    m_state.a[2] += m_state.a[1] >> 32;
    m_state.a[3] += m_state.a[2] >> 32;
    m_state.a[4] += m_state.a[3] >> 32;

    // Select mask based on (a + 5) >= 2^130
    u32 mask = ((m_state.a[4] & 0x04) >> 2) - 1;

    // Select based on mask
    m_state.a[0] = (m_state.a[0] & ~mask) | (b[0] & mask);
    m_state.a[1] = (m_state.a[1] & ~mask) | (b[1] & mask);
    m_state.a[2] = (m_state.a[2] & ~mask) | (b[2] & mask);
    m_state.a[3] = (m_state.a[3] & ~mask) | (b[3] & mask);

    // Finally, the value of the secret key "s" is added to the accumulator,
    // and the 128 least significant bits are serialized in little-endian
    // order to form the tag.
    m_state.a[0] += m_state.s[0];
    m_state.a[1] += m_state.s[1];
    m_state.a[2] += m_state.s[2];
    m_state.a[3] += m_state.s[3];

    // Carry
    m_state.a[1] += m_state.a[0] >> 32;
    m_state.a[2] += m_state.a[1] >> 32;
    m_state.a[3] += m_state.a[2] >> 32;
    m_state.a[4] += m_state.a[3] >> 32;

    // Only consider the least significant bits
    b[0] = m_state.a[0] & 0xFFFFFFFF;
    b[1] = m_state.a[1] & 0xFFFFFFFF;
    b[2] = m_state.a[2] & 0xFFFFFFFF;
    b[3] = m_state.a[3] & 0xFFFFFFFF;

    ByteBuffer output = TRY(ByteBuffer::create_uninitialized(16));

    for (auto i = 0; i < 4; i++) {
        ByteReader::store(output.offset_pointer(i * 4), AK::convert_between_host_and_little_endian(b[i]));
    }

    return output;
}

}
