/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>

namespace AK {

template<size_t Size>
class FixedBitmap {
public:
    constexpr FixedBitmap(bool default_value)
    {
        fill(default_value);
    }

    constexpr void fill(bool value)
    {
        __builtin_memset(m_data.data(), value ? 0xff : 0x00, size_in_bytes());
    }

    constexpr void set(size_t index, bool value)
    {
        VERIFY(index < Size);
        if (value)
            m_data[index / 8] |= static_cast<u8>(1u << (index % 8));
        else
            m_data[index / 8] &= static_cast<u8>(~(1u << (index % 8)));
    }

    [[nodiscard]] constexpr bool get(size_t index) const
    {
        VERIFY(index < Size);
        return 0 != (m_data[index / 8] & (1u << (index % 8)));
    }

    [[nodiscard]] constexpr size_t size() const { return Size; }
    [[nodiscard]] constexpr size_t size_in_bytes() const { return ceil_div(Size, static_cast<size_t>(8)); }

private:
    Array<u8, ceil_div(Size, static_cast<size_t>(8))> m_data;
};

}
