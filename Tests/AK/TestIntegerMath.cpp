/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/IntegralMath.h>
#include <AK/NumericLimits.h>
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

TEST_CASE(gcd)
{
    EXPECT_EQ(AK::gcd(0, 0), 0);
    EXPECT_EQ(AK::gcd(1, 1), 1);
    EXPECT_EQ(AK::gcd(0, 2), 2);
    EXPECT_EQ(AK::gcd(2, 0), 2);
    EXPECT_EQ(AK::gcd(8, 12), 4);
    EXPECT_EQ(AK::gcd(17, 23), 1);
    EXPECT_EQ(AK::gcd(48, 36), 12);
    EXPECT_EQ(AK::gcd(-8, 12), 4);
    EXPECT_EQ(AK::gcd(8, -12), 4);
    EXPECT_EQ(AK::gcd(-8, -12), 4);
    EXPECT_EQ(AK::gcd(100, 100), 100);
    EXPECT_EQ(AK::gcd(13, 1), 1);
    EXPECT_EQ(AK::gcd(-NumericLimits<i32>::max(), NumericLimits<i32>::max()), NumericLimits<i32>::max());
}

TEST_CASE(lcm)
{
    EXPECT_EQ(AK::lcm(0, 0), 0);
    EXPECT_EQ(AK::lcm(0, 5), 0);
    EXPECT_EQ(AK::lcm(5, 0), 0);
    EXPECT_EQ(AK::lcm(1, 1), 1);
    EXPECT_EQ(AK::lcm(4, 6), 12);
    EXPECT_EQ(AK::lcm(7, 13), 91);
    EXPECT_EQ(AK::lcm(12, 18), 36);
    EXPECT_EQ(AK::lcm(-4, 6), 12);
    EXPECT_EQ(AK::lcm(4, -6), 12);
    EXPECT_EQ(AK::lcm(-4, -6), 12);
    EXPECT_EQ(AK::lcm(10, 10), 10);
    EXPECT_EQ(AK::lcm(1, 8), 8);
}

TEST_CASE(multiply_divide)
{
    EXPECT_EQ(AK::multiply_divide(0, 12345, 678), 0ULL);
    EXPECT_EQ(AK::multiply_divide(6, 7, 3), 14ULL);

    // The quotient truncates towards zero.
    EXPECT_EQ(AK::multiply_divide(7, 3, 2), 10ULL);
    EXPECT_EQ(AK::multiply_divide(1, 1, 2), 0ULL);

    // The product overflows 64 bits, but the quotient does not.
    EXPECT_EQ(AK::multiply_divide(6'000'000'000, 3'000'000'000, 9'000'000'000), 2'000'000'000ULL);
    EXPECT_EQ(AK::multiply_divide(1'000'000'000'000'000'000, 1'000'000'000'000'000'000, 1'000'000'000'000'000'000), 1'000'000'000'000'000'000ULL);

    auto maximum = NumericLimits<u64>::max();
    EXPECT_EQ(AK::multiply_divide(maximum, maximum, maximum), maximum);
    EXPECT_EQ(AK::multiply_divide(maximum, 1, maximum), 1ULL);

    static_assert(AK::multiply_divide(6'000'000'000, 3'000'000'000, 9'000'000'000) == 2'000'000'000ULL);
    static_assert(AK::multiply_divide(7, 3, 2) == 10ULL);
    static_assert(AK::multiply_divide(NumericLimits<u64>::max(), NumericLimits<u64>::max(), NumericLimits<u64>::max()) == NumericLimits<u64>::max());
}

TEST_CASE(multiply_divide_matches_wide_arithmetic)
{
    // Verifying the definition of integer division needs only multiplication, which keeps this portable to
    // targets whose 128-bit division is a compiler runtime call.
    constexpr u64 values[] = {
        1, 2, 3, 7, 255, 65'537, 1'000'000'007,
        1ULL << 31, 1ULL << 32, (1ULL << 32) + 1, 1ULL << 63,
        NumericLimits<u64>::max() - 1, NumericLimits<u64>::max()
    };

    for (auto multiplicand : values) {
        for (auto multiplier : values) {
            auto product = static_cast<unsigned __int128>(multiplicand) * multiplier;
            auto high_word = static_cast<u64>(product >> 64);
            if (high_word == NumericLimits<u64>::max())
                continue;

            for (auto value : values) {
                // Raising the divisor above the product's high word is what keeps the quotient within 64
                // bits, and the smallest such divisor is the tightest case the division has to handle.
                auto divisor = max(value, high_word + 1);

                auto quotient = static_cast<unsigned __int128>(AK::multiply_divide(multiplicand, multiplier, divisor));
                EXPECT(quotient * divisor <= product);
                EXPECT((quotient + 1) * divisor > product);
            }
        }
    }
}

TEST_CASE(multiply_divide_rejects_results_that_do_not_fit)
{
    EXPECT_DEATH("Dividing by zero", (void)AK::multiply_divide(1, 1, 0));
    EXPECT_DEATH("Quotient wider than 64 bits", (void)AK::multiply_divide(NumericLimits<u64>::max(), NumericLimits<u64>::max(), 1));
}
