/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Types.h>
#include <LibCrypto/Checksum/ChecksumFunction.h>

namespace Crypto::Checksum {

class Adler32 : public ChecksumFunction<u32> {
public:
    Adler32() = default;
    Adler32(ReadonlyBytes data)
    {
        update(data);
    }

    Adler32(u32 initial_a, u32 initial_b, ReadonlyBytes data)
        : m_state_a(initial_a)
        , m_state_b(initial_b)
    {
        update(data);
    }

    virtual void update(ReadonlyBytes data) override;
    virtual u32 digest() override;

private:
    u32 m_state_a { 1 };
    u32 m_state_b { 0 };
};

}
