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

class CRC32 : public ChecksumFunction<u32> {
public:
    CRC32() = default;
    CRC32(ReadonlyBytes data)
    {
        update(data);
    }

    CRC32(u32 initial_state, ReadonlyBytes data)
        : m_state(initial_state)
    {
        update(data);
    }

    virtual void update(ReadonlyBytes data) override;
    virtual u32 digest() override;

private:
    u32 m_state { ~0u };
};

}
