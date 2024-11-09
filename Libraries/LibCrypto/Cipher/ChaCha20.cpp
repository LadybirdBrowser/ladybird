/*
 * Copyright (c) 2022, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/Endian.h>
#include <LibCrypto/Cipher/ChaCha20.h>

namespace Crypto::Cipher {

ChaCha20::ChaCha20(ReadonlyBytes key, ReadonlyBytes nonce, u32 initial_counter)
{
    VERIFY(key.size() == 16 || key.size() == 32);
    VERIFY(nonce.size() == 8 || nonce.size() == 12);

    // The first four words (0-3) are constants
    if (key.size() == 32) {
        m_state[0] = CONSTANT_32_BYTES[0];
        m_state[1] = CONSTANT_32_BYTES[1];
        m_state[2] = CONSTANT_32_BYTES[2];
        m_state[3] = CONSTANT_32_BYTES[3];
    } else {
        m_state[0] = CONSTANT_16_BYTES[0];
        m_state[1] = CONSTANT_16_BYTES[1];
        m_state[2] = CONSTANT_16_BYTES[2];
        m_state[3] = CONSTANT_16_BYTES[3];
    }

    // The next eight words (4-11) are taken from the key by reading the bytes in little-endian order, in 4-byte chunks.
    for (u32 i = 0; i < 16; i += 4) {
        m_state[(i / 4) + 4] = AK::convert_between_host_and_little_endian(ByteReader::load32(key.offset(i)));
    }

    // NOTE: For the 128-bit keys we read the same bytes twice to fill the state
    u32 key_offset = key.size() == 32 ? 16 : 0;
    for (u32 i = 0; i < 16; i += 4) {
        m_state[(i / 4) + 8] = AK::convert_between_host_and_little_endian(ByteReader::load32(key.offset(key_offset + i)));
    }

    // Word 12 is a block counter.  Since each block is 64-bytes, a 32-bit word is enough for 256 gigabytes of data.
    m_state[12] = initial_counter;

    // Words 13-15 are a nonce, which should not be repeated for the same key.
    // The 13th word is the first 32 bits of the input nonce taken as a little-endian integer,
    // while the 15th word is the last 32 bits.

    // NOTE: In the case of an 8-byte nonce, we skip the 13th word
    u32 nonce_offset = nonce.size() == 8 ? 1 : 0;
    for (u32 i = 0; i < 12; i += 4) {
        m_state[(i / 4) + 13 + nonce_offset] = AK::convert_between_host_and_little_endian(ByteReader::load32(nonce.offset(i)));
    }
}

// https://datatracker.ietf.org/doc/html/rfc7539#section-2.3
void ChaCha20::generate_block()
{
    // Copy the current state into the block
    memcpy(m_block, m_state, 16 * sizeof(u32));

    // ChaCha20 runs 20 rounds, alternating between "column rounds" and "diagonal rounds".
    // Each round consists of four quarter-rounds
    for (u32 i = 0; i < 20; i += 2) {
        // Column rounds
        do_quarter_round(m_block[0], m_block[4], m_block[8], m_block[12]);
        do_quarter_round(m_block[1], m_block[5], m_block[9], m_block[13]);
        do_quarter_round(m_block[2], m_block[6], m_block[10], m_block[14]);
        do_quarter_round(m_block[3], m_block[7], m_block[11], m_block[15]);

        // Diagonal rounds
        do_quarter_round(m_block[0], m_block[5], m_block[10], m_block[15]);
        do_quarter_round(m_block[1], m_block[6], m_block[11], m_block[12]);
        do_quarter_round(m_block[2], m_block[7], m_block[8], m_block[13]);
        do_quarter_round(m_block[3], m_block[4], m_block[9], m_block[14]);
    }

    // At the end of 20 rounds, we add the original input words to the output words,
    for (u32 i = 0; i < 16; i++) {
        m_block[i] += m_state[i];
    }

    // and serialize the result by sequencing the words one-by-one in little-endian order.
    for (u32 i = 0; i < 16; i++) {
        m_block[i] = AK::convert_between_host_and_little_endian(m_block[i]);
    }
}

ALWAYS_INLINE static void rotl(u32& x, u32 n)
{
    x = (x << n) | (x >> (32 - n));
}

// https://datatracker.ietf.org/doc/html/rfc8439#section-2.1
void ChaCha20::do_quarter_round(u32& a, u32& b, u32& c, u32& d)
{
    a += b;
    d ^= a;
    rotl(d, 16);

    c += d;
    b ^= c;
    rotl(b, 12);

    a += b;
    d ^= a;
    rotl(d, 8);

    c += d;
    b ^= c;
    rotl(b, 7);
}

void ChaCha20::run_cipher(ReadonlyBytes input, Bytes& output)
{
    size_t offset = 0;
    size_t block_offset = 0;
    while (offset < input.size()) {
        if (block_offset == 0 || block_offset >= 64) {
            // Generate a new XOR block
            generate_block();

            // Increment the block counter, and carry over to block 13
            m_state[12]++;
            if (m_state[12] == 0) {
                m_state[13]++;
            }

            block_offset = 0;
        }

        // XOR the input and the current block
        u32 n = min(input.size() - offset, 64 - block_offset);
        u8* key_block = (u8*)m_block + block_offset;
        for (u32 i = 0; i < n; i++) {
            u8 input_byte = input.offset_pointer(offset)[i];
            u8 key_byte = key_block[i];
            u8 output_byte = input_byte ^ key_byte;

            ByteReader::store(output.offset_pointer(offset + i), output_byte);
        }

        offset += n;
        block_offset += n;
    }
}

void ChaCha20::encrypt(ReadonlyBytes input, Bytes& output)
{
    VERIFY(input.size() <= output.size());
    this->run_cipher(input, output);
}

void ChaCha20::decrypt(ReadonlyBytes input, Bytes& output)
{
    VERIFY(input.size() <= output.size());
    this->run_cipher(input, output);
}

}
