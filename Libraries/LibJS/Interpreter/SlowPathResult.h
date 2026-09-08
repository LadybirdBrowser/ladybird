/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace JS {

// Successful helpers preserve the current frame and continue at the next
// instruction. Keep its PC in the low word for interpreter continuations.
inline constexpr u8 slow_path_continuation_bit = 32;

constexpr i64 continue_after_slow_path(u32 next_pc)
{
    return (i64 { 1 } << slow_path_continuation_bit) | next_pc;
}

}
