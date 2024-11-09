/*
 * Copyright (c) 2024, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Types.h>
#include <LibCrypto/Checksum/ChecksumFunction.h>

namespace Crypto::Checksum {

class cksum : public ChecksumFunction<u32> {
public:
    cksum() = default;
    cksum(ReadonlyBytes data)
    {
        update(data);
    }

    virtual void update(ReadonlyBytes data) override;
    virtual u32 digest() override;

private:
    u32 m_state { 0 };
    off_t m_size { 0 };
};

}
