/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/Types.h>

namespace GC {

// Zeroes the 8 KiB of stack below its caller, on the real stack even under ASan.
inline NO_SANITIZE_ADDRESS NEVER_INLINE void scrub_stack_for_testing()
{
    u8 volatile filler[8 * KiB];
    for (size_t i = 0; i < sizeof(filler); ++i)
        filler[i] = 0;
}

}
