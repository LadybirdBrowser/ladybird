/*
 * Copyright (c) 2018-2020, Nick Johnson <sylvyrfysh@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/BuiltinWrappers.h>
#include <AK/Types.h>

using namespace Test::Randomized;

TEST_CASE(wrapped_popcount)
{
    EXPECT_EQ(popcount(NumericLimits<u8>::max()), 8);
    EXPECT_EQ(popcount(NumericLimits<u16>::max()), 16);
    EXPECT_EQ(popcount(NumericLimits<u32>::max()), 32);
    EXPECT_EQ(popcount(NumericLimits<u64>::max()), 64);
    EXPECT_EQ(popcount(NumericLimits<size_t>::max()), static_cast<int>(8 * sizeof(size_t)));
    EXPECT_EQ(popcount(0u), 0);
    EXPECT_EQ(popcount(0b01010101ULL), 4);
}

TEST_CASE(wrapped_count_leading_zeroes)
{
    EXPECT_EQ(count_leading_zeroes(NumericLimits<u8>::max()), 0);
    EXPECT_EQ(count_leading_zeroes(static_cast<u8>(0x20)), 2);
    EXPECT_EQ(count_leading_zeroes_safe(static_cast<u8>(0)), 8);
    EXPECT_EQ(count_leading_zeroes(NumericLimits<u16>::max()), 0);
    EXPECT_EQ(count_leading_zeroes(static_cast<u16>(0x20)), 10);
    EXPECT_EQ(count_leading_zeroes_safe(static_cast<u16>(0)), 16);
    EXPECT_EQ(count_leading_zeroes(NumericLimits<u32>::max()), 0);
    EXPECT_EQ(count_leading_zeroes(static_cast<u32>(0x20)), 26);
    EXPECT_EQ(count_leading_zeroes_safe(static_cast<u32>(0)), 32);
    EXPECT_EQ(count_leading_zeroes(NumericLimits<u64>::max()), 0);
}

TEST_CASE(wrapped_count_trailing_zeroes)
{
    EXPECT_EQ(count_trailing_zeroes(NumericLimits<u8>::max()), 0);
    EXPECT_EQ(count_trailing_zeroes(static_cast<u8>(1)), 0);
    EXPECT_EQ(count_trailing_zeroes(static_cast<u8>(2)), 1);
    EXPECT_EQ(count_trailing_zeroes_safe(static_cast<u8>(0)), 8);
    EXPECT_EQ(count_trailing_zeroes(NumericLimits<u16>::max()), 0);
    EXPECT_EQ(count_trailing_zeroes(static_cast<u16>(1)), 0);
    EXPECT_EQ(count_trailing_zeroes(static_cast<u16>(2)), 1);
    EXPECT_EQ(count_trailing_zeroes_safe(static_cast<u16>(0)), 16);
    EXPECT_EQ(count_trailing_zeroes(NumericLimits<u32>::max()), 0);
    EXPECT_EQ(count_trailing_zeroes(static_cast<u32>(1)), 0);
    EXPECT_EQ(count_trailing_zeroes(static_cast<u32>(2)), 1);
    EXPECT_EQ(count_trailing_zeroes_safe(static_cast<u32>(0)), 32);
    EXPECT_EQ(count_trailing_zeroes(NumericLimits<u64>::max()), 0);
    EXPECT_EQ(count_trailing_zeroes(static_cast<u64>(1)), 0);
    EXPECT_EQ(count_trailing_zeroes(static_cast<u64>(2)), 1);
}

TEST_CASE(wrapped_count_required_bits)
{
    EXPECT_EQ(count_required_bits(0b0u), 1ul);
    EXPECT_EQ(count_required_bits(0b1u), 1ul);
    EXPECT_EQ(count_required_bits(0b10u), 2ul);
    EXPECT_EQ(count_required_bits(0b11u), 2ul);
    EXPECT_EQ(count_required_bits(0b100u), 3ul);
    EXPECT_EQ(count_required_bits(0b111u), 3ul);
    EXPECT_EQ(count_required_bits(0b1000u), 4ul);
    EXPECT_EQ(count_required_bits(0b1111u), 4ul);
    EXPECT_EQ(count_required_bits(NumericLimits<u32>::max()), sizeof(u32) * 8);
}

RANDOMIZED_TEST_CASE(count_leading_zeroes)
{
    //    count_leading_zeroes(0b000...0001000...000)
    // == count_leading_zeroes(0b000...0001___...___) (where _ is 0 or 1)
    GEN(e, Gen::number_u64(0, 63));
    auto power_of_two = 1ULL << e;

    // We add random one-bits below the leftmost (and only) one-bit.
    // This shouldn't change the output of count_leading_zeroes because
    // the function should only care about the most significant one.
    GEN(below, Gen::number_u64(0, power_of_two - 1));
    auto n = power_of_two + below;

    EXPECT_EQ(count_leading_zeroes(n), count_leading_zeroes(power_of_two));
}

RANDOMIZED_TEST_CASE(count_required_bits)
{
    // count_required_bits(n) == log2(n) + 1

    // log2(0) is -infinity, we don't care about that
    GEN(n, Gen::number_u32(1, NumericLimits<u32>::max()));

    size_t expected = AK::log2(static_cast<double>(n)) + 1;

    EXPECT_EQ(count_required_bits(n), expected);
}

RANDOMIZED_TEST_CASE(bit_scan_forward_count_trailing_zeroes)
{
    // Behaviour for 0 differs, so we skip it.
    GEN(n, Gen::number_u32(1, 1 << 31));
    EXPECT_EQ(bit_scan_forward(n), count_trailing_zeroes(n) + 1);
}
