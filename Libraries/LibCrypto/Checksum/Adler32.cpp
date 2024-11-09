/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Span.h>
#include <AK/Types.h>
#include <LibCrypto/Checksum/Adler32.h>

namespace Crypto::Checksum {

void Adler32::update(ReadonlyBytes data)
{
    // See https://github.com/SerenityOS/serenity/pull/24408#discussion_r1609051678
    constexpr size_t iterations_without_overflow = 380368439;

    u64 state_a = m_state_a;
    u64 state_b = m_state_b;
    while (data.size()) {
        // You can verify that no overflow will happen here during at least
        // `iterations_without_overflow` iterations using the following Python script:
        //
        // state_a = 65520
        // state_b = 65520
        // for i in range(380368439):
        //     state_a += 255
        //     state_b += state_a
        // print(state_b < 2 ** 64)
        auto chunk = data.slice(0, min(data.size(), iterations_without_overflow));
        for (u8 byte : chunk) {
            state_a += byte;
            state_b += state_a;
        }
        state_a %= 65521;
        state_b %= 65521;
        data = data.slice(chunk.size());
    }
    m_state_a = state_a;
    m_state_b = state_b;
}

u32 Adler32::digest()
{
    return (m_state_b << 16) | m_state_a;
}

}
