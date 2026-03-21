/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/NumericLimits.h>
#include <AK/SaturatingMath.h>

// Normal signed add (nowhere near saturation)

TEST_CASE(signed_add_normal)
{
    EXPECT_EQ(saturating_add(0, 0), 0);
    EXPECT_EQ(saturating_add(1, 1), 2);
    EXPECT_EQ(saturating_add(3, 4), 7);
    EXPECT_EQ(saturating_add(-3, -4), -7);
    EXPECT_EQ(saturating_add(3, -4), -1);
    EXPECT_EQ(saturating_add(-3, 4), 1);
    EXPECT_EQ(saturating_add(100, 200), 300);
    EXPECT_EQ(saturating_add(-100, -200), -300);
    EXPECT_EQ(saturating_add(100, -200), -100);
    EXPECT_EQ(saturating_add(1000000, 2000000), 3000000);
}

// Signed add: cross-boundary (large values that cancel out, should NOT saturate)

TEST_CASE(signed_add_cross_boundary)
{
    EXPECT_EQ(saturating_add(NumericLimits<i32>::max(), NumericLimits<i32>::min()), -1);
    EXPECT_EQ(saturating_add(NumericLimits<i32>::min(), NumericLimits<i32>::max()), -1);
    EXPECT_EQ(saturating_add(NumericLimits<i32>::max(), -NumericLimits<i32>::max()), 0);
    EXPECT_EQ(saturating_add(NumericLimits<i32>::max() / 2, -(NumericLimits<i32>::max() / 2)), 0);
}

// Signed add: off-by-one at saturation point

TEST_CASE(signed_add_off_by_one)
{
    // Exactly at max: no saturation
    EXPECT_EQ(saturating_add(NumericLimits<i32>::max() - 1, 1), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_add(NumericLimits<i32>::max() - 10, 10), NumericLimits<i32>::max());
    // One past max: saturates
    EXPECT_EQ(saturating_add(NumericLimits<i32>::max(), 1), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_add(NumericLimits<i32>::max() - 9, 10), NumericLimits<i32>::max());

    // Exactly at min: no saturation
    EXPECT_EQ(saturating_add(NumericLimits<i32>::min() + 1, -1), NumericLimits<i32>::min());
    EXPECT_EQ(saturating_add(NumericLimits<i32>::min() + 10, -10), NumericLimits<i32>::min());
    // One past min: saturates
    EXPECT_EQ(saturating_add(NumericLimits<i32>::min(), -1), NumericLimits<i32>::min());
    EXPECT_EQ(saturating_add(NumericLimits<i32>::min() + 9, -10), NumericLimits<i32>::min());
}

// Signed add: extreme overflow

TEST_CASE(signed_add_extreme_overflow)
{
    EXPECT_EQ(saturating_add(NumericLimits<i32>::max(), NumericLimits<i32>::max()), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_add(NumericLimits<i32>::min(), NumericLimits<i32>::min()), NumericLimits<i32>::min());
}

// Signed add: identity

TEST_CASE(signed_add_identity)
{
    EXPECT_EQ(saturating_add(NumericLimits<i32>::max(), 0), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_add(NumericLimits<i32>::min(), 0), NumericLimits<i32>::min());
    EXPECT_EQ(saturating_add(0, NumericLimits<i32>::max()), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_add(0, NumericLimits<i32>::min()), NumericLimits<i32>::min());
}

// Normal signed sub (nowhere near saturation)

TEST_CASE(signed_sub_normal)
{
    EXPECT_EQ(saturating_sub(0, 0), 0);
    EXPECT_EQ(saturating_sub(7, 4), 3);
    EXPECT_EQ(saturating_sub(4, 7), -3);
    EXPECT_EQ(saturating_sub(-7, -4), -3);
    EXPECT_EQ(saturating_sub(3, -4), 7);
    EXPECT_EQ(saturating_sub(-3, 4), -7);
    EXPECT_EQ(saturating_sub(300, 200), 100);
    EXPECT_EQ(saturating_sub(-300, -200), -100);
    EXPECT_EQ(saturating_sub(1000000, 2000000), -1000000);
}

// Signed sub: cross-boundary (should NOT saturate)

TEST_CASE(signed_sub_cross_boundary)
{
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::max(), NumericLimits<i32>::max()), 0);
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::min(), NumericLimits<i32>::min()), 0);
    EXPECT_EQ(saturating_sub(0, NumericLimits<i32>::max()), NumericLimits<i32>::min() + 1);
}

// Signed sub: off-by-one at saturation point

TEST_CASE(signed_sub_off_by_one)
{
    // Exactly at max: no saturation
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::max() - 1, -1), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::max() - 10, -10), NumericLimits<i32>::max());
    // One past max: saturates
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::max(), -1), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::max() - 9, -10), NumericLimits<i32>::max());

    // Exactly at min: no saturation
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::min() + 1, 1), NumericLimits<i32>::min());
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::min() + 10, 10), NumericLimits<i32>::min());
    // One past min: saturates
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::min(), 1), NumericLimits<i32>::min());
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::min() + 9, 10), NumericLimits<i32>::min());
}

// Signed sub: extreme overflow

TEST_CASE(signed_sub_extreme_overflow)
{
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::max(), NumericLimits<i32>::min()), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::min(), NumericLimits<i32>::max()), NumericLimits<i32>::min());
}

// Signed sub: identity

TEST_CASE(signed_sub_identity)
{
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::max(), 0), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_sub(NumericLimits<i32>::min(), 0), NumericLimits<i32>::min());
}

// Normal unsigned add (nowhere near saturation)

TEST_CASE(unsigned_add_normal)
{
    EXPECT_EQ(saturating_add(0u, 0u), 0u);
    EXPECT_EQ(saturating_add(1u, 1u), 2u);
    EXPECT_EQ(saturating_add(3u, 4u), 7u);
    EXPECT_EQ(saturating_add(100u, 200u), 300u);
    EXPECT_EQ(saturating_add(1000000u, 2000000u), 3000000u);
}

// Unsigned add: off-by-one at saturation point

TEST_CASE(unsigned_add_off_by_one)
{
    EXPECT_EQ(saturating_add(NumericLimits<u32>::max() - 1, 1u), NumericLimits<u32>::max());
    EXPECT_EQ(saturating_add(NumericLimits<u32>::max() - 10, 10u), NumericLimits<u32>::max());
    EXPECT_EQ(saturating_add(NumericLimits<u32>::max(), 1u), NumericLimits<u32>::max());
    EXPECT_EQ(saturating_add(NumericLimits<u32>::max() - 9, 10u), NumericLimits<u32>::max());
}

// Unsigned add: extreme overflow and identity

TEST_CASE(unsigned_add_extreme_and_identity)
{
    EXPECT_EQ(saturating_add(NumericLimits<u32>::max(), NumericLimits<u32>::max()), NumericLimits<u32>::max());
    EXPECT_EQ(saturating_add(NumericLimits<u32>::max(), 0u), NumericLimits<u32>::max());
    EXPECT_EQ(saturating_add(0u, NumericLimits<u32>::max()), NumericLimits<u32>::max());
}

// Normal unsigned sub (nowhere near saturation)

TEST_CASE(unsigned_sub_normal)
{
    EXPECT_EQ(saturating_sub(0u, 0u), 0u);
    EXPECT_EQ(saturating_sub(7u, 4u), 3u);
    EXPECT_EQ(saturating_sub(4u, 4u), 0u);
    EXPECT_EQ(saturating_sub(300u, 200u), 100u);
    EXPECT_EQ(saturating_sub(NumericLimits<u32>::max(), NumericLimits<u32>::max()), 0u);
}

// Unsigned sub: off-by-one at saturation point

TEST_CASE(unsigned_sub_off_by_one)
{
    EXPECT_EQ(saturating_sub(1u, 1u), 0u);
    EXPECT_EQ(saturating_sub(10u, 10u), 0u);
    EXPECT_EQ(saturating_sub(0u, 1u), 0u);
    EXPECT_EQ(saturating_sub(9u, 10u), 0u);
}

// Unsigned sub: extreme underflow and identity

TEST_CASE(unsigned_sub_extreme_and_identity)
{
    EXPECT_EQ(saturating_sub(0u, NumericLimits<u32>::max()), 0u);
    EXPECT_EQ(saturating_sub(5u, 10u), 0u);
    EXPECT_EQ(saturating_sub(NumericLimits<u32>::max(), 0u), NumericLimits<u32>::max());
}

// Normal signed mul (nowhere near saturation)

TEST_CASE(signed_mul_normal)
{
    EXPECT_EQ(saturating_mul(0, 0), 0);
    EXPECT_EQ(saturating_mul(1, 1), 1);
    EXPECT_EQ(saturating_mul(3, 4), 12);
    EXPECT_EQ(saturating_mul(-3, 4), -12);
    EXPECT_EQ(saturating_mul(3, -4), -12);
    EXPECT_EQ(saturating_mul(-3, -4), 12);
    EXPECT_EQ(saturating_mul(100, 200), 20000);
    EXPECT_EQ(saturating_mul(0, NumericLimits<i32>::max()), 0);
    EXPECT_EQ(saturating_mul(NumericLimits<i32>::max(), 0), 0);
    EXPECT_EQ(saturating_mul(1, NumericLimits<i32>::max()), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_mul(NumericLimits<i32>::max(), 1), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_mul(-1, NumericLimits<i32>::max()), -NumericLimits<i32>::max());
}

TEST_CASE(signed_mul_overflow)
{
    EXPECT_EQ(saturating_mul(NumericLimits<i32>::max(), 2), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_mul(NumericLimits<i32>::max(), NumericLimits<i32>::max()), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_mul(NumericLimits<i32>::min(), 2), NumericLimits<i32>::min());
    EXPECT_EQ(saturating_mul(NumericLimits<i32>::min(), NumericLimits<i32>::min()), NumericLimits<i32>::max());
    EXPECT_EQ(saturating_mul(NumericLimits<i32>::max(), -2), NumericLimits<i32>::min());
    EXPECT_EQ(saturating_mul(NumericLimits<i32>::max(), NumericLimits<i32>::min()), NumericLimits<i32>::min());
}

// Normal unsigned mul (nowhere near saturation)

TEST_CASE(unsigned_mul_normal)
{
    EXPECT_EQ(saturating_mul(0u, 0u), 0u);
    EXPECT_EQ(saturating_mul(3u, 4u), 12u);
    EXPECT_EQ(saturating_mul(100u, 200u), 20000u);
    EXPECT_EQ(saturating_mul(0u, NumericLimits<u32>::max()), 0u);
    EXPECT_EQ(saturating_mul(1u, NumericLimits<u32>::max()), NumericLimits<u32>::max());
}

TEST_CASE(unsigned_mul_overflow)
{
    EXPECT_EQ(saturating_mul(NumericLimits<u32>::max(), 2u), NumericLimits<u32>::max());
    EXPECT_EQ(saturating_mul(NumericLimits<u32>::max(), NumericLimits<u32>::max()), NumericLimits<u32>::max());
}

// All integer sizes

TEST_CASE(i8_saturation)
{
    EXPECT_EQ(saturating_add(static_cast<i8>(50), static_cast<i8>(30)), static_cast<i8>(80));
    EXPECT_EQ(saturating_add(static_cast<i8>(100), static_cast<i8>(100)), NumericLimits<i8>::max());
    EXPECT_EQ(saturating_add(static_cast<i8>(-100), static_cast<i8>(-100)), NumericLimits<i8>::min());
    EXPECT_EQ(saturating_add(NumericLimits<i8>::max(), NumericLimits<i8>::min()), static_cast<i8>(-1));

    EXPECT_EQ(saturating_sub(static_cast<i8>(50), static_cast<i8>(30)), static_cast<i8>(20));
    EXPECT_EQ(saturating_sub(static_cast<i8>(-100), static_cast<i8>(100)), NumericLimits<i8>::min());
    EXPECT_EQ(saturating_sub(static_cast<i8>(100), static_cast<i8>(-100)), NumericLimits<i8>::max());
}

TEST_CASE(i16_saturation)
{
    EXPECT_EQ(saturating_add(static_cast<i16>(1000), static_cast<i16>(2000)), static_cast<i16>(3000));
    EXPECT_EQ(saturating_add(NumericLimits<i16>::max(), static_cast<i16>(1)), NumericLimits<i16>::max());
    EXPECT_EQ(saturating_add(NumericLimits<i16>::min(), static_cast<i16>(-1)), NumericLimits<i16>::min());
    EXPECT_EQ(saturating_add(NumericLimits<i16>::max(), NumericLimits<i16>::min()), static_cast<i16>(-1));

    EXPECT_EQ(saturating_sub(static_cast<i16>(3000), static_cast<i16>(1000)), static_cast<i16>(2000));
    EXPECT_EQ(saturating_sub(NumericLimits<i16>::max(), static_cast<i16>(-1)), NumericLimits<i16>::max());
    EXPECT_EQ(saturating_sub(NumericLimits<i16>::min(), static_cast<i16>(1)), NumericLimits<i16>::min());
}

TEST_CASE(i64_saturation)
{
    EXPECT_EQ(saturating_add(static_cast<i64>(1000000000), static_cast<i64>(2000000000)), static_cast<i64>(3000000000));
    EXPECT_EQ(saturating_add(NumericLimits<i64>::max(), static_cast<i64>(1)), NumericLimits<i64>::max());
    EXPECT_EQ(saturating_add(NumericLimits<i64>::min(), static_cast<i64>(-1)), NumericLimits<i64>::min());
    EXPECT_EQ(saturating_add(NumericLimits<i64>::max(), NumericLimits<i64>::min()), static_cast<i64>(-1));

    EXPECT_EQ(saturating_sub(NumericLimits<i64>::min(), static_cast<i64>(1)), NumericLimits<i64>::min());
    EXPECT_EQ(saturating_sub(NumericLimits<i64>::max(), static_cast<i64>(-1)), NumericLimits<i64>::max());
    EXPECT_EQ(saturating_sub(NumericLimits<i64>::max(), NumericLimits<i64>::max()), static_cast<i64>(0));
}

TEST_CASE(u8_saturation)
{
    EXPECT_EQ(saturating_add(static_cast<u8>(50), static_cast<u8>(30)), static_cast<u8>(80));
    EXPECT_EQ(saturating_add(static_cast<u8>(200), static_cast<u8>(200)), NumericLimits<u8>::max());

    EXPECT_EQ(saturating_sub(static_cast<u8>(80), static_cast<u8>(30)), static_cast<u8>(50));
    EXPECT_EQ(saturating_sub(static_cast<u8>(0), static_cast<u8>(1)), static_cast<u8>(0));
}

TEST_CASE(u16_saturation)
{
    EXPECT_EQ(saturating_add(static_cast<u16>(1000), static_cast<u16>(2000)), static_cast<u16>(3000));
    EXPECT_EQ(saturating_add(NumericLimits<u16>::max(), static_cast<u16>(1)), NumericLimits<u16>::max());

    EXPECT_EQ(saturating_sub(static_cast<u16>(3000), static_cast<u16>(1000)), static_cast<u16>(2000));
    EXPECT_EQ(saturating_sub(static_cast<u16>(0), static_cast<u16>(1)), static_cast<u16>(0));
}

TEST_CASE(u64_saturation)
{
    EXPECT_EQ(saturating_add(static_cast<u64>(1000000000), static_cast<u64>(2000000000)), static_cast<u64>(3000000000));
    EXPECT_EQ(saturating_add(NumericLimits<u64>::max(), static_cast<u64>(1)), NumericLimits<u64>::max());

    EXPECT_EQ(saturating_sub(static_cast<u64>(3000000000), static_cast<u64>(1000000000)), static_cast<u64>(2000000000));
    EXPECT_EQ(saturating_sub(static_cast<u64>(0), static_cast<u64>(1)), static_cast<u64>(0));
}
