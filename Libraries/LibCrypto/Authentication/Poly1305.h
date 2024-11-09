/*
 * Copyright (c) 2022, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Crypto::Authentication {

struct State {
    u32 r[4] {};
    u32 s[4] {};
    u64 a[8] {};
    u8 blocks[17] {};
    u8 block_count {};
};

class Poly1305 {

public:
    explicit Poly1305(ReadonlyBytes key);
    void update(ReadonlyBytes message);
    ErrorOr<ByteBuffer> digest();

private:
    void process_block();

    State m_state;
};

}
