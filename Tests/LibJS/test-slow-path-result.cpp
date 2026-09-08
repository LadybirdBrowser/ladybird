/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Interpreter/SlowPathResult.h>
#include <LibTest/TestCase.h>

TEST_CASE(encodes_same_frame_continuations)
{
    constexpr u32 next_pc = 42;
    constexpr auto result = JS::continue_after_slow_path(next_pc);

    EXPECT_EQ(static_cast<u32>(result), next_pc);
    EXPECT_EQ(static_cast<u64>(result) >> JS::slow_path_continuation_bit, 1u);
}
