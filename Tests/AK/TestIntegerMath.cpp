/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/IntegralMath.h>
#include <initializer_list>

TEST_CASE(pow)
{
    EXPECT_EQ(AK::pow<u64>(0, 0), 1ull);
    EXPECT_EQ(AK::pow<u64>(10, 0), 1ull);
    EXPECT_EQ(AK::pow<u64>(10, 1), 10ull);
    EXPECT_EQ(AK::pow<u64>(10, 2), 100ull);
    EXPECT_EQ(AK::pow<u64>(10, 3), 1'000ull);
    EXPECT_EQ(AK::pow<u64>(10, 4), 10'000ull);
    EXPECT_EQ(AK::pow<u64>(10, 5), 100'000ull);
    EXPECT_EQ(AK::pow<u64>(10, 6), 1'000'000ull);
}

TEST_CASE(is_power_of)
{
    EXPECT(!AK::is_power_of<0>(10ull));
    // We don't have enough context to know if the input was from 0^0
    EXPECT(!AK::is_power_of<0>(1ull));

    EXPECT(!AK::is_power_of<1>(10ull));
    EXPECT(!AK::is_power_of<1>(0ull));

    constexpr auto check_prime = []<u64 prime>(u64 limit, u64 init = 0) {
        for (u64 power = init; power < limit; ++power)
            EXPECT(AK::is_power_of<prime>(AK::pow(prime, power)));
    };

    // Limits calculated as floor( log_{prime}(2^64) ) to prevent overflows.
    check_prime.operator()<0>(42, 1);
    check_prime.operator()<1>(36);
    check_prime.operator()<2>(64);
    check_prime.operator()<3>(40);
    check_prime.operator()<5>(27);
    check_prime.operator()<7>(20);
    check_prime.operator()<11>(18);
    check_prime.operator()<97>(9);
    check_prime.operator()<257>(7);
}

TEST_CASE(exp2)
{
    EXPECT_EQ(AK::exp2<u64>(0), 1ull);
    EXPECT_EQ(AK::exp2<u64>(1), 2ull);
    EXPECT_EQ(AK::exp2<i8>(6), 64);
    EXPECT_EQ(AK::exp2<u8>(7), 128);
    EXPECT_EQ(AK::exp2<u16>(9), 512);
    EXPECT_EQ(AK::exp2<i16>(14), 16384);
    EXPECT_EQ(AK::exp2<u16>(15), 32768);
    EXPECT_EQ(AK::exp2<u32>(17), 131072u);
    EXPECT_EQ(AK::exp2<i32>(30), 1073741824);
    EXPECT_EQ(AK::exp2<u32>(31), 2147483648);
    EXPECT_EQ(AK::exp2<i64>(32), 4294967296);
    EXPECT_EQ(AK::exp2<u64>(33), 8589934592ull);
    EXPECT_EQ(AK::exp2<i64>(62), 4611686018427387904);
    EXPECT_EQ(AK::exp2<u64>(63), 9223372036854775808ull);
}

TEST_CASE(log2)
{
    EXPECT_EQ(AK::log2<u64>(0), 0ull);
    EXPECT_EQ(AK::log2<u64>(1), 0ull);
    EXPECT_EQ(AK::log2<i8>(64), 6);
    EXPECT_EQ(AK::log2<u8>(128), 7);
    EXPECT_EQ(AK::log2<u16>(512), 9);
    EXPECT_EQ(AK::log2<i16>(16384), 14);
    EXPECT_EQ(AK::log2<u16>(32768), 15);
    EXPECT_EQ(AK::log2<i32>(131072), 17);
    EXPECT_EQ(AK::log2<i32>(1073741824), 30);
    EXPECT_EQ(AK::log2<u32>(2147483648), 31u);
    EXPECT_EQ(AK::log2<i64>(4294967296), 32);
    EXPECT_EQ(AK::log2<i64>(8589934592), 33);
    EXPECT_EQ(AK::log2<i64>(4611686018427387904), 62);
    EXPECT_EQ(AK::log2<u64>(9223372036854775808ull), 63ull);
}

TEST_CASE(ceil_log2)
{
    EXPECT_EQ(AK::ceil_log2<u64>(0), 0ull);
    EXPECT_EQ(AK::ceil_log2<u64>(1), 0ull);
    EXPECT_EQ(AK::ceil_log2<u8>(2), 1);
    EXPECT_EQ(AK::ceil_log2<u8>(3), 2);
    EXPECT_EQ(AK::ceil_log2<u8>(6), 3);
    EXPECT_EQ(AK::ceil_log2<i8>(96), 7);
    EXPECT_EQ(AK::ceil_log2<i8>(127), 7);
    EXPECT_EQ(AK::ceil_log2<u8>(128), 7);
    EXPECT_EQ(AK::ceil_log2<u8>(255), 8);
    EXPECT_EQ(AK::ceil_log2<i16>(256), 8);
    EXPECT_EQ(AK::ceil_log2<i16>(257), 9);
    EXPECT_EQ(AK::ceil_log2<i16>(384), 9);
    EXPECT_EQ(AK::ceil_log2<i16>(24576), 15);
    EXPECT_EQ(AK::ceil_log2<i16>(32767), 15);
    EXPECT_EQ(AK::ceil_log2<i32>(32768), 15);
    EXPECT_EQ(AK::ceil_log2<i32>(32769), 16);
    EXPECT_EQ(AK::ceil_log2<i32>(98304), 17);
    EXPECT_EQ(AK::ceil_log2<i32>(1610612736), 31);
    EXPECT_EQ(AK::ceil_log2<i32>(2147483647), 31);
    EXPECT_EQ(AK::ceil_log2<u32>(2147483648), 31u);
    EXPECT_EQ(AK::ceil_log2<u32>(2147483649), 32u);
    EXPECT_EQ(AK::ceil_log2<u32>(3221225472), 32u);
    EXPECT_EQ(AK::ceil_log2<u32>(4294967295), 32u);
    EXPECT_EQ(AK::ceil_log2<i64>(4294967296), 32);
    EXPECT_EQ(AK::ceil_log2<i64>(4294967297), 33);
    EXPECT_EQ(AK::ceil_log2<i64>(9223372036854775807), 63ll);
    EXPECT_EQ(AK::ceil_log2<u64>(9223372036854775808ull), 63ull);
    EXPECT_EQ(AK::ceil_log2<u64>(9223372036854775809ull), 64ull);
    EXPECT_EQ(AK::ceil_log2<u64>(13835058055282163712ull), 64ull);
    EXPECT_EQ(AK::ceil_log2<u64>(18446744073709551615ull), 64ull);
}

TEST_CASE(clamp_to)
{
    EXPECT_EQ((AK::clamp_to<i32>(1000000u)), 1000000);
    EXPECT_EQ((AK::clamp_to<i32>(NumericLimits<u64>::max())), NumericLimits<i32>::max());

    EXPECT_EQ((AK::clamp_to<u32>(-10)), 0u);
    EXPECT_EQ((AK::clamp_to<u32>(10)), 10u);

    EXPECT_EQ((AK::clamp_to<i32>(NumericLimits<i64>::min())), NumericLimits<i32>::min());
    EXPECT_EQ((AK::clamp_to<i32>(NumericLimits<i64>::max())), NumericLimits<i32>::max());

    EXPECT_EQ(AK::clamp_to<i64>(-9223372036854775808.0), NumericLimits<i64>::min());
    EXPECT_EQ(AK::clamp_to<i64>(9223372036854775807.0), NumericLimits<i64>::max());
}
