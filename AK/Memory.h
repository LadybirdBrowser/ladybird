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

}

#if USING_AK_GLOBALLY
using AK::secure_zero;
#endif
