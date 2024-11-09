/*
 * Copyright (c) 2022, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Crypto::Cipher {

class ChaCha20 {
    static constexpr u32 CONSTANT_16_BYTES[] { 0x61707865, 0x3120646E, 0x79622D36, 0x6B206574 };
    static constexpr u32 CONSTANT_32_BYTES[] { 0x61707865, 0x3320646E, 0x79622D32, 0x6B206574 };

public:
    ChaCha20(ReadonlyBytes key, ReadonlyBytes nonce, u32 initial_counter = 0);

    void encrypt(ReadonlyBytes input, Bytes& output);
    void decrypt(ReadonlyBytes input, Bytes& output);
    void generate_block();
    ReadonlyBytes block() const { return { m_block, 64 }; }

private:
    void run_cipher(ReadonlyBytes input, Bytes& output);
    ALWAYS_INLINE void do_quarter_round(u32& a, u32& b, u32& c, u32& d);

    u32 m_state[16] {};
    u32 m_block[16] {};
};
}
