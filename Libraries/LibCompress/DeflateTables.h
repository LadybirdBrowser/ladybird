/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <stdint.h>

namespace Compress {

// RFC 1951 - 3.2.5
static constexpr struct {
    u16 symbol;
    u16 base_length;
    u16 extra_bits;
} packed_length_symbols[29] = {
    { 257, 3, 0 },
    { 258, 4, 0 },
    { 259, 5, 0 },
    { 260, 6, 0 },
    { 261, 7, 0 },
    { 262, 8, 0 },
    { 263, 9, 0 },
    { 264, 10, 0 },
    { 265, 11, 1 },
    { 266, 13, 1 },
    { 267, 15, 1 },
    { 268, 17, 1 },
    { 269, 19, 2 },
    { 270, 23, 2 },
    { 271, 27, 2 },
    { 272, 31, 2 },
    { 273, 35, 3 },
    { 274, 43, 3 },
    { 275, 51, 3 },
    { 276, 59, 3 },
    { 277, 67, 4 },
    { 278, 83, 4 },
    { 279, 99, 4 },
    { 280, 115, 4 },
    { 281, 131, 5 },
    { 282, 163, 5 },
    { 283, 195, 5 },
    { 284, 227, 5 },
    { 285, 258, 0 }
};

// RFC 1951 - 3.2.5
static constexpr struct {
    u16 symbol;
    u16 base_distance;
    u16 extra_bits;
} packed_distances[31] = {
    { 0, 1, 0 },
    { 1, 2, 0 },
    { 2, 3, 0 },
    { 3, 4, 0 },
    { 4, 5, 1 },
    { 5, 7, 1 },
    { 6, 9, 2 },
    { 7, 13, 2 },
    { 8, 17, 3 },
    { 9, 25, 3 },
    { 10, 33, 4 },
    { 11, 49, 4 },
    { 12, 65, 5 },
    { 13, 97, 5 },
    { 14, 129, 6 },
    { 15, 193, 6 },
    { 16, 257, 7 },
    { 17, 385, 7 },
    { 18, 513, 8 },
    { 19, 769, 8 },
    { 20, 1025, 9 },
    { 21, 1537, 9 },
    { 22, 2049, 10 },
    { 23, 3073, 10 },
    { 24, 4097, 11 },
    { 25, 6145, 11 },
    { 26, 8193, 12 },
    { 27, 12289, 12 },
    { 28, 16385, 13 },
    { 29, 24577, 13 },
    { 30, 32 * KiB + 1, 0 }, // signifies end
};

// RFC 1951 - 3.2.6
static constexpr struct {
    u16 base_value;
    u16 bits;
} fixed_literal_bits[5] = {
    { 0, 8 },
    { 144, 9 },
    { 256, 7 },
    { 280, 8 },
    { 288, 0 } // signifies end
};

// RFC 1951 - 3.2.7
static constexpr size_t code_lengths_code_lengths_order[] { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static consteval Array<u16, 259> generate_length_to_symbol()
{
    Array<u16, 259> array = { UINT16_MAX, UINT16_MAX, UINT16_MAX }; // there are 256 valid lengths (3-258) + 3 invalid lengths (0-2)
    size_t base_length = 0;
    for (size_t len = 3; len < 259; len++) {
        if (len == packed_length_symbols[base_length + 1].base_length)
            base_length++;
        array[len] = packed_length_symbols[base_length].symbol;
    }
    return array;
}
static constexpr auto length_to_symbol = generate_length_to_symbol();

static consteval Array<u16, 256> generate_distance_to_base_lo()
{
    Array<u16, 256> array;
    size_t base_distance = 0;
    for (size_t dist = 1; dist <= 256; dist++) {
        if (dist == packed_distances[base_distance + 1].base_distance)
            base_distance++;
        array[dist - 1] = packed_distances[base_distance].symbol;
    }
    return array;
}
static constexpr auto distance_to_base_lo = generate_distance_to_base_lo();
static consteval Array<u16, 256> generate_distance_to_base_hi()
{
    Array<u16, 256> array = { UINT16_MAX, UINT16_MAX };
    size_t base_distance = 16;
    for (size_t dist = 257; dist <= 32 * KiB; dist++) {
        if (dist == packed_distances[base_distance + 1].base_distance)
            base_distance++;
        array[(dist - 1) >> 7] = packed_distances[base_distance].symbol;
    }
    return array;
}
static constexpr auto distance_to_base_hi = generate_distance_to_base_hi();

static consteval Array<u8, 288> generate_fixed_literal_bit_lengths()
{
    Array<u8, 288> array;
    for (size_t i = 0; i < 4; i++) {
        array.span().slice(fixed_literal_bits[i].base_value, fixed_literal_bits[i + 1].base_value - fixed_literal_bits[i].base_value).fill(fixed_literal_bits[i].bits);
    }
    return array;
}
static constexpr auto fixed_literal_bit_lengths = generate_fixed_literal_bit_lengths();

static consteval Array<u8, 32> generate_fixed_distance_bit_lengths()
{
    Array<u8, 32> array;
    array.fill(5);
    return array;
}
static constexpr auto fixed_distance_bit_lengths = generate_fixed_distance_bit_lengths();

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
