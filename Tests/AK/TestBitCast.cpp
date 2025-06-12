/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/BitCast.h>

template<typename A, typename B>
void check_cast_both_ways(A const& a, B const& b)
{
    EXPECT_EQ((bit_cast<A, B>(b)), a);
    EXPECT_EQ((bit_cast<B, A>(a)), b);
}

TEST_CASE(double_int_conversion)
{
    check_cast_both_ways(static_cast<u64>(0), 0.0);
    check_cast_both_ways(static_cast<u64>(1) << 63, -0.0);
    check_cast_both_ways(static_cast<u64>(0x4172f58bc0000000), 19880124.0);
}
