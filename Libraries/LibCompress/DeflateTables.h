/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <stdint.h>

namespace Compress {

static consteval u8 reverse8(u8 value)
{
    u8 result = 0;
    for (size_t i = 0; i < 8; i++) {
        if (value & (1 << i))
            result |= 1 << (7 - i);
    }
    return result;
}
static consteval Array<u8, UINT8_MAX + 1> generate_reverse8_lookup_table()
{
    Array<u8, UINT8_MAX + 1> array;
    for (size_t i = 0; i <= UINT8_MAX; i++) {
        array[i] = reverse8(i);
    }
    return array;
}
static constexpr auto reverse8_lookup_table = generate_reverse8_lookup_table();

// Lookup-table based bit swap
ALWAYS_INLINE static u16 fast_reverse16(u16 value, size_t bits)
{
    VERIFY(bits <= 16);

    u16 lo = value & 0xff;
    u16 hi = value >> 8;

    u16 reversed = (u16)((reverse8_lookup_table[lo] << 8) | reverse8_lookup_table[hi]);

    return reversed >> (16 - bits);
}

}
