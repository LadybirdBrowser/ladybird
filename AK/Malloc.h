/*
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef AK_OS_WINDOWS

#    include <malloc.h>

inline int posix_memalign(void** memptr, size_t alignment, size_t size)
{
    void* ptr = _aligned_malloc(size, alignment);
    if (!ptr)
        return errno;
    *memptr = ptr;
    return 0;
}

#endif
