/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGfx/Size.h>

#include "LookupTables.h"

namespace Media::Video::VP9 {

// FIXME: Once everything is working, replace this with plain clamp
// since parameter order is different
template<typename T>
T clip_3(T x, T y, T z)
{
    return clamp(z, x, y);
}

template<typename T>
u16 clip_1(u8 bit_depth, T x)
{
    if (x < 0) {
        return 0u;
    }
    T const max = (1u << bit_depth) - 1u;
    if (x > max)
        return max;
    return x;
}

template<u8 bits>
inline u8 brev(u8 value)
{
    static_assert(bits <= 8, "brev() expects an 8-bit value.");

    static constexpr auto lookup_table = [] {
        constexpr size_t value_count = 1 << bits;
        Array<u8, value_count> the_table;
        for (u8 lookup_value = 0; lookup_value < value_count; lookup_value++) {
            u8 reversed = 0;
            for (u8 bit_index = 0; bit_index < bits; bit_index++) {
                auto bit = (lookup_value >> bit_index) & 1;
                reversed |= bit << (bits - 1 - bit_index);
            }
            the_table[lookup_value] = reversed;
        }
        return the_table;
    }();

    return lookup_table[value];
}

inline BlockSubsize get_subsampled_block_size(BlockSubsize size, bool subsampling_x, bool subsampling_y)
{
    return ss_size_lookup[size < Block_8x8 ? Block_8x8 : size][subsampling_x][subsampling_y];
}

inline Gfx::Size<u8> block_size_to_blocks(BlockSubsize size)
{
    return Gfx::Size<u8>(num_8x8_blocks_wide_lookup[size], num_8x8_blocks_high_lookup[size]);
}

inline Gfx::Size<u8> block_size_to_sub_blocks(BlockSubsize size)
{
    return Gfx::Size<u8>(num_4x4_blocks_wide_lookup[size], num_4x4_blocks_high_lookup[size]);
}

template<Integral T>
inline T blocks_to_superblocks(T blocks)
{
    return blocks >> 3;
}

template<Integral T>
inline T superblocks_to_blocks(T superblocks)
{
    return superblocks << 3;
}

template<Integral T>
inline T blocks_ceiled_to_superblocks(T blocks)
{
    return blocks_to_superblocks(blocks + 7);
}

template<Integral T>
inline T blocks_to_sub_blocks(T blocks)
{
    return blocks << 1;
}

template<Integral T>
inline T sub_blocks_to_blocks(T sub_blocks)
{
    return sub_blocks >> 1;
}

template<Integral T>
inline T sub_blocks_to_pixels(T sub_blocks)
{
    return sub_blocks << 2;
}

template<Integral T>
inline T pixels_to_sub_blocks(T pixels)
{
    return pixels >> 2;
}

template<Integral T>
inline T blocks_to_pixels(T blocks)
{
    return sub_blocks_to_pixels(blocks_to_sub_blocks(blocks));
}

template<Integral T>
inline T pixels_to_blocks(T pixels)
{
    return sub_blocks_to_blocks(pixels_to_sub_blocks(pixels));
}

inline u8 transform_size_to_sub_blocks(TransformSize transform_size)
{
    return 1 << transform_size;
}

}
