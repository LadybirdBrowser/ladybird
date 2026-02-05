/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Brian Gianforcaro <bgianf@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <string.h>

namespace AK {

inline void secure_zero(void* ptr, size_t size)
{
    __builtin_memset(ptr, 0, size);
    // The memory barrier is here to avoid the compiler optimizing
    // away the memset when we rely on it for wiping secrets.
    asm volatile("" ::
            : "memory");
}

// Naive implementation of a constant time buffer comparison function.
// The goal being to not use any conditional branching so calls are
// guarded against potential timing attacks.
//
// See OpenBSD's timingsafe_memcmp for more advanced implementations.
inline bool timing_safe_compare(void const* b1, void const* b2, size_t len)
{
    auto* c1 = static_cast<char const*>(b1);
    auto* c2 = static_cast<char const*>(b2);

    u8 res = 0;
    for (size_t i = 0; i < len; i++) {
        res |= c1[i] ^ c2[i];
    }

    // FIXME: !res can potentially inject branches depending
    // on which toolchain is being used for compilation. Ideally
    // we would use a more advanced algorithm.
    return !res;
}

}

#if USING_AK_GLOBALLY
using AK::secure_zero;
using AK::timing_safe_compare;
#endif
