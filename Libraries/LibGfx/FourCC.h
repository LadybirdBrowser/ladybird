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

    static constexpr FourCC from_u32(u32 value)
    {
        FourCC result;
        result.cc[0] = static_cast<char>(value >> 24);
        result.cc[1] = static_cast<char>(value >> 16);
        result.cc[2] = static_cast<char>(value >> 8);
        result.cc[3] = static_cast<char>(value);
        return result;
    }

    bool operator==(FourCC const&) const = default;
    bool operator!=(FourCC const&) const = default;

    u32 to_u32() const
    {
        return (static_cast<u8>(cc[0]) << 24)
            | (static_cast<u8>(cc[1]) << 16)
            | (static_cast<u8>(cc[2]) << 8)
            | static_cast<u8>(cc[3]);
    }

    char cc[4];
};

}
