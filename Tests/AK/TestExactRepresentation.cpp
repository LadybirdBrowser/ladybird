/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibTest/TestCase.h>

TEST_CASE(Generate_ints_with_n_1s)
{
    EXPECT_EQ(AK::int_with_n_1s(0), 0u);
    EXPECT_EQ(AK::int_with_n_1s(1), 1u);
    EXPECT_EQ(AK::int_with_n_1s(10), 0b1111111111u);
    EXPECT_EQ(AK::int_with_n_1s(64), NumericLimits<unsigned long>::max());
}

TEST_CASE(has_exact_floatingpoint_representation)
{
    static_assert(AK::has_exact_representation<float>(0));
    static_assert(AK::has_exact_representation<float>(NumericLimits<int>::min()));
    static_assert(!AK::has_exact_representation<float>(NumericLimits<int>::max()));
    static_assert(AK::has_exact_representation<float>(999));
    static_assert(AK::has_exact_representation<double>(NumericLimits<int>::min()));
    static_assert(AK::has_exact_representation<double>(NumericLimits<int>::max()));
    static_assert(AK::has_exact_representation<double>(NumericLimits<long>::min()));
    static_assert(!AK::has_exact_representation<double>(NumericLimits<long>::max()));
}
