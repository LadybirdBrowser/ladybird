/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Math.h>
#include <AK/NumericLimits.h>

TEST_CASE(round_to_rounds_ties_to_even)
{
    EXPECT_EQ(round_to<i32>(2.5), 2);
    EXPECT_EQ(round_to<i32>(3.5), 4);
    EXPECT_EQ(round_to<i32>(-2.5f), -2);
    EXPECT_EQ(round_to<u8>(254.6f), 255);
}

TEST_CASE(round_to_saturates)
{
    EXPECT_EQ(round_to<i32>(3e9), NumericLimits<i32>::max());
    EXPECT_EQ(round_to<i32>(-3e9f), NumericLimits<i32>::min());
    EXPECT_EQ(round_to<u8>(-1.0f), 0);
    EXPECT_EQ(round_to<u8>(300.0f), 255);
    EXPECT_EQ(round_to<i64>(1e19), NumericLimits<i64>::max());
    EXPECT_EQ(round_to<i64>(static_cast<double>(NumericLimits<i64>::max())), NumericLimits<i64>::max());
    EXPECT_EQ(round_to<u64>(1e19), 10'000'000'000'000'000'000ull);
    EXPECT_EQ(round_to<i32>(AK::Infinity<double>), NumericLimits<i32>::max());
    EXPECT_EQ(round_to<i32>(-AK::Infinity<float>), NumericLimits<i32>::min());
}

TEST_CASE(round_to_maps_nan_to_zero)
{
    EXPECT_EQ(round_to<i32>(AK::NaN<double>), 0);
    EXPECT_EQ(round_to<u64>(AK::NaN<float>), 0u);
}

TEST_CASE(constant_evaluated_rounding_out_of_i64_range)
{
    constexpr double two_to_the_63 = 9223372036854775808.0;
    static_assert(AK::floor(two_to_the_63) == two_to_the_63);
    static_assert(AK::ceil(-two_to_the_63) == -two_to_the_63);
    static_assert(AK::trunc(1e300) == 1e300);
    static_assert(AK::floor(-1.5) == -2.0);
    static_assert(AK::ceil(1.25f) == 2.0f);
}

TEST_CASE(pow_with_large_integral_exponent)
{
    EXPECT_EQ(AK::pow(1.0, 1e18), 1.0);
    EXPECT_EQ(AK::pow(2.0, 1e9), AK::Infinity<double>);
    EXPECT_EQ(AK::pow(2.0, -1e9), 0.0);
}
