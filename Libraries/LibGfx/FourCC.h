/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Gfx {

struct [[gnu::packed]] FourCC {
    FourCC() = default;

    constexpr FourCC(char const name[4])
    {
        cc[0] = name[0];
        cc[1] = name[1];
        cc[2] = name[2];
        cc[3] = name[3];
    }

    bool operator==(FourCC const&) const = default;

    u32 to_u32() const
    {
        return (static_cast<u8>(cc[0]) << 24)
            | (static_cast<u8>(cc[1]) << 16)
            | (static_cast<u8>(cc[2]) << 8)
            | static_cast<u8>(cc[3]);
    }

    bool operator<(FourCC const& other) const
    {
        return this->to_u32() < other.to_u32();
    }

    char cc[4];
};

}

template<>
struct AK::Traits<Gfx::FourCC> : public AK::DefaultTraits<Gfx::FourCC> {
    static unsigned hash(Gfx::FourCC const& key)
    {
        return key.to_u32();
    }
};
