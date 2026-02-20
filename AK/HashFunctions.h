/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

// MurmurHash3 32-bit finalizer (fmix32).
constexpr unsigned u32_hash(u32 key)
{
    key ^= key >> 16;
    key *= 0x85ebca6bU;
    key ^= key >> 13;
    key *= 0xc2b2ae35U;
    key ^= key >> 16;
    return key;
}

// MurmurHash3 64-bit finalizer (fmix64).
constexpr unsigned u64_hash(u64 key)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return static_cast<unsigned>(key);
}

constexpr unsigned pair_int_hash(u32 key1, u32 key2)
{
    return u64_hash((static_cast<u64>(key1) << 32) | key2);
}

constexpr unsigned ptr_hash(FlatPtr ptr)
{
    if constexpr (sizeof(ptr) == 8)
        return u64_hash(ptr);
    else
        return u32_hash(ptr);
}

inline unsigned ptr_hash(void const* ptr)
{
    return ptr_hash(FlatPtr(ptr));
}
