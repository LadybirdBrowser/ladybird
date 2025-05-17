/*
 * Copyright (c) 2021, Peter Bocan  <me@pbocan.net>
 * Copyright (c) 2025, Manuel Zahariev  <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Tuple.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibTest/TestCase.h>

#include <math.h>

static Crypto::UnsignedBigInteger bigint_fibonacci(size_t n)
{
    Crypto::UnsignedBigInteger num1(0);
    Crypto::UnsignedBigInteger num2(1);
    for (size_t i = 0; i < n; ++i) {
        Crypto::UnsignedBigInteger t = num1.plus(num2);
        num2 = num1;
        num1 = t;
    }
    return num1;
}

static Crypto::SignedBigInteger bigint_signed_fibonacci(size_t n)
{
    Crypto::SignedBigInteger num1(0);
    Crypto::SignedBigInteger num2(1);
    for (size_t i = 0; i < n; ++i) {
        Crypto::SignedBigInteger t = num1.plus(num2);
        num2 = num1;
        num1 = t;
    }
    return num1;
}

TEST_CASE(test_bigint_fib500)
{
    Vector<u32> result {
        315178285, 505575602, 1883328078, 125027121, 3649625763,
        347570207, 74535262, 3832543808, 2472133297, 1600064941, 65273441
    };

    EXPECT_EQ(bigint_fibonacci(500).words(), result);
}

BENCHMARK_CASE(bench_bigint_fib100000)
{
    auto res = bigint_fibonacci(100000);
    (void)res;
}

TEST_CASE(test_unsigned_bigint_addition_initialization)
{
    Crypto::UnsignedBigInteger num1;
    Crypto::UnsignedBigInteger num2(70);
    Crypto::UnsignedBigInteger num3 = num1.plus(num2);
    bool pass = (num3 == num2);
    pass &= (num1 == Crypto::UnsignedBigInteger(0));
    EXPECT(pass);
}

TEST_CASE(test_unsigned_bigint_addition_borrow_with_zero)
{
    Crypto::UnsignedBigInteger num1({ UINT32_MAX - 3, UINT32_MAX });
    Crypto::UnsignedBigInteger num2({ UINT32_MAX - 2, 0 });
    Vector<u32> expected_result { 4294967289, 0, 1 };
    EXPECT_EQ(num1.plus(num2).words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_simple_subtraction)
{
    Crypto::UnsignedBigInteger num1(80);
    Crypto::UnsignedBigInteger num2(70);

    EXPECT_EQ(TRY_OR_FAIL(num1.minus(num2)), Crypto::UnsignedBigInteger(10));
}

TEST_CASE(test_unsigned_bigint_simple_subtraction_invalid)
{
    Crypto::UnsignedBigInteger num1(50);
    Crypto::UnsignedBigInteger num2(70);

    EXPECT(num1.minus(num2).is_error());
}

TEST_CASE(test_unsigned_bigint_simple_subtraction_with_borrow)
{
    Crypto::UnsignedBigInteger num1(UINT32_MAX);
    Crypto::UnsignedBigInteger num2(1);
    Crypto::UnsignedBigInteger num3 = num1.plus(num2);
    Crypto::UnsignedBigInteger result = TRY_OR_FAIL(num3.minus(num2));
    EXPECT_EQ(result, num1);
}

TEST_CASE(test_unsigned_bigint_subtraction_with_large_numbers)
{
    Crypto::UnsignedBigInteger num1 = bigint_fibonacci(343);
    Crypto::UnsignedBigInteger num2 = bigint_fibonacci(218);
    Crypto::UnsignedBigInteger result = TRY_OR_FAIL(num1.minus(num2));

    Vector<u32> expected_result {
        811430588, 2958904896, 1130908877, 2830569969, 3243275482,
        3047460725, 774025231, 7990
    };
    EXPECT_EQ(result.plus(num2), num1);
    EXPECT_EQ(result.words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_subtraction_with_large_numbers2)
{
    Crypto::UnsignedBigInteger num1(Vector<u32> { 1483061863, 446680044, 1123294122, 191895498, 3347106536, 16, 0, 0, 0 });
    Crypto::UnsignedBigInteger num2(Vector<u32> { 4196414175, 1117247942, 1123294122, 191895498, 3347106536, 16 });
    ErrorOr<Crypto::UnsignedBigInteger> result = num1.minus(num2);
    // this test only verifies that we don't crash on an assertion
    (void)result;
}

TEST_CASE(test_unsigned_bigint_subtraction_regression_1)
{
    auto num = TRY_OR_FAIL(Crypto::UnsignedBigInteger { 1 }.shift_left(256));
    Vector<u32> expected_result {
        4294967295, 4294967295, 4294967295, 4294967295, 4294967295,
        4294967295, 4294967295, 4294967295
    };
    EXPECT_EQ(TRY_OR_FAIL(num.minus(1)).words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_simple_multiplication)
{
    Crypto::UnsignedBigInteger num1(8);
    Crypto::UnsignedBigInteger num2(251);
    Crypto::UnsignedBigInteger result = num1.multiplied_by(num2);
    EXPECT_EQ(result.words(), Vector<u32> { 2008 });
}

TEST_CASE(test_unsigned_bigint_multiplication_with_big_numbers1)
{
    Crypto::UnsignedBigInteger num1 = bigint_fibonacci(200);
    Crypto::UnsignedBigInteger num2(12345678);
    Crypto::UnsignedBigInteger result = num1.multiplied_by(num2);
    Vector<u32> expected_result { 669961318, 143970113, 4028714974, 3164551305, 1589380278, 2 };
    EXPECT_EQ(result.words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_multiplication_with_big_numbers2)
{
    Crypto::UnsignedBigInteger num1 = bigint_fibonacci(200);
    Crypto::UnsignedBigInteger num2 = bigint_fibonacci(341);
    Crypto::UnsignedBigInteger result = num1.multiplied_by(num2);
    Vector<u32> expected_result {
        3017415433, 2741793511, 1957755698, 3731653885, 3154681877,
        785762127, 3200178098, 4260616581, 529754471, 3632684436,
        1073347813, 2516430
    };
    EXPECT_EQ(result.words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_simple_division)
{
    Crypto::UnsignedBigInteger num1(27194);
    Crypto::UnsignedBigInteger num2(251);
    auto result = num1.divided_by(num2);
    Crypto::UnsignedDivisionResult expected = { Crypto::UnsignedBigInteger(108), Crypto::UnsignedBigInteger(86) };
    EXPECT_EQ(result.quotient, expected.quotient);
    EXPECT_EQ(result.remainder, expected.remainder);
}

TEST_CASE(test_unsigned_bigint_division_with_big_numbers)
{
    Crypto::UnsignedBigInteger num1 = bigint_fibonacci(386);
    Crypto::UnsignedBigInteger num2 = bigint_fibonacci(238);
    auto result = num1.divided_by(num2);
    Crypto::UnsignedDivisionResult expected = {
        Crypto::UnsignedBigInteger(Vector<u32> { 2300984486, 2637503534, 2022805584, 107 }),
        Crypto::UnsignedBigInteger(Vector<u32> { 1483061863, 446680044, 1123294122, 191895498, 3347106536, 16, 0, 0, 0 })
    };
    EXPECT_EQ(result.quotient, expected.quotient);
    EXPECT_EQ(result.remainder, expected.remainder);
}

TEST_CASE(test_unsigned_bigint_division_combined_test)
{
    auto num1 = bigint_fibonacci(497);
    auto num2 = bigint_fibonacci(238);
    auto div_result = num1.divided_by(num2);
    EXPECT_EQ(div_result.quotient.multiplied_by(num2).plus(div_result.remainder), num1);
}

TEST_CASE(test_unsigned_bigint_base10_from_string)
{
    auto result = TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(10, "57195071295721390579057195715793"sv));
    Vector<u32> expected_result { 3806301393, 954919431, 3879607298, 721 };
    EXPECT_EQ(result.words(), expected_result);

    Vector<StringView> invalid_base10_number_strings { "1A"sv, "1:"sv, "Z1"sv, "1/"sv };
    for (auto invalid_base10_number_string : invalid_base10_number_strings)
        EXPECT_EQ(Crypto::UnsignedBigInteger::from_base(10, invalid_base10_number_string).is_error(), true);
}

TEST_CASE(test_unsigned_bigint_base10_to_string)
{
    auto bigint = Crypto::UnsignedBigInteger {
        Vector<u32> { 3806301393, 954919431, 3879607298, 721 }
    };
    auto result = MUST(bigint.to_base(10));
    EXPECT_EQ(result, "57195071295721390579057195715793");
}

TEST_CASE(test_bigint_import_big_endian_decode_encode_roundtrip)
{
    u8 random_bytes[128];
    u8 target_buffer[128];
    fill_with_random(random_bytes);
    auto encoded = Crypto::UnsignedBigInteger::import_data(random_bytes, 128);
    (void)encoded.export_data({ target_buffer, 128 });
    EXPECT(memcmp(target_buffer, random_bytes, 128) == 0);
}

TEST_CASE(test_bigint_import_big_endian_encode_decode_roundtrip)
{
    u8 target_buffer[128];
    auto encoded = "12345678901234567890"_bigint;
    auto size = encoded.export_data({ target_buffer, 128 });
    auto decoded = Crypto::UnsignedBigInteger::import_data(target_buffer, size);
    EXPECT_EQ(encoded, decoded);
}

TEST_CASE(test_bigint_big_endian_import)
{
    auto number = Crypto::UnsignedBigInteger::import_data("hello"sv);
    EXPECT_EQ(number, "448378203247"_bigint);
}

TEST_CASE(test_bigint_big_endian_export)
{
    auto number = "448378203247"_bigint;
    char exported[8] {};
    auto exported_length = number.export_data({ exported, 8 });
    EXPECT_EQ(exported_length, 5u);
    EXPECT(memcmp(exported, "hello", 5) == 0);
}

TEST_CASE(test_bigint_one_based_index_of_highest_set_bit)
{
    EXPECT_EQ("0"_bigint.one_based_index_of_highest_set_bit(), 0u);
    EXPECT_EQ("1"_bigint.one_based_index_of_highest_set_bit(), 1u);
    EXPECT_EQ("7"_bigint.one_based_index_of_highest_set_bit(), 3u);
    EXPECT_EQ("4294967296"_bigint.one_based_index_of_highest_set_bit(), 33u);
}

TEST_CASE(test_signed_bigint_bitwise_not_fill_to_one_based_index)
{
    EXPECT_EQ(TRY_OR_FAIL("0"_bigint.bitwise_not_fill_to_one_based_index(0)), "0"_bigint);
    EXPECT_EQ(TRY_OR_FAIL("0"_bigint.bitwise_not_fill_to_one_based_index(1)), "1"_bigint);
    EXPECT_EQ(TRY_OR_FAIL("0"_bigint.bitwise_not_fill_to_one_based_index(2)), "3"_bigint);
    EXPECT_EQ(TRY_OR_FAIL("0"_bigint.bitwise_not_fill_to_one_based_index(4)), "15"_bigint);
    EXPECT_EQ(TRY_OR_FAIL("0"_bigint.bitwise_not_fill_to_one_based_index(32)), "4294967295"_bigint);
    EXPECT_EQ(TRY_OR_FAIL("0"_bigint.bitwise_not_fill_to_one_based_index(33)), "8589934591"_bigint);
}

TEST_CASE(test_bigint_bitwise_or)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "1234567"_bigint;
    EXPECT_EQ(num1.bitwise_or(num2), num1);
}

TEST_CASE(test_bigint_bitwise_or_different_lengths)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "123456789012345678901234567890"_bigint;
    auto expected = "123456789012345678901234622167"_bigint;
    auto result = num1.bitwise_or(num2);
    EXPECT_EQ(result, expected);
}

TEST_CASE(test_signed_bigint_bitwise_or)
{
    auto num1 = "-1234567"_sbigint;
    auto num2 = "1234567"_sbigint;
    EXPECT_EQ(num1.bitwise_or(num1), num1);
    EXPECT_EQ(num1.bitwise_or(num2), "-1"_sbigint);
    EXPECT_EQ(num2.bitwise_or(num1), "-1"_sbigint);
    EXPECT_EQ(num2.bitwise_or(num2), num2);

    EXPECT_EQ("0"_sbigint.bitwise_or("-1"_sbigint), "-1"_sbigint);
}

TEST_CASE(test_bigint_bitwise_and)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "1234561"_bigint;
    EXPECT_EQ(num1.bitwise_and(num2), "1234561"_bigint);
}

TEST_CASE(test_bigint_bitwise_and_different_lengths)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "123456789012345678901234567890"_bigint;
    EXPECT_EQ(num1.bitwise_and(num2), "1180290"_bigint);
}

TEST_CASE(test_signed_bigint_bitwise_not)
{
    EXPECT_EQ("3"_sbigint.bitwise_not(), "-4"_sbigint);
    EXPECT_EQ("-1"_sbigint.bitwise_not(), "0"_sbigint);
}

TEST_CASE(test_signed_bigint_bitwise_and)
{
    auto num1 = "-1234567"_sbigint;
    auto num2 = "1234567"_sbigint;
    EXPECT_EQ(num1.bitwise_and(num1), num1);
    EXPECT_EQ(num1.bitwise_and(num2), "1"_sbigint);
    EXPECT_EQ(num2.bitwise_and(num1), "1"_sbigint);
    EXPECT_EQ(num2.bitwise_and(num2), num2);

    EXPECT_EQ("-3"_sbigint.bitwise_and("-2"_sbigint), "-4"_sbigint);
}

TEST_CASE(test_bigint_bitwise_xor)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "1234561"_bigint;
    EXPECT_EQ(num1.bitwise_xor(num2), 6);
}

TEST_CASE(test_bigint_bitwise_xor_different_lengths)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "123456789012345678901234567890"_bigint;
    EXPECT_EQ(num1.bitwise_xor(num2), "123456789012345678901233441877"_bigint);
}

TEST_CASE(test_signed_bigint_bitwise_xor)
{
    auto num1 = "-3"_sbigint;
    auto num2 = "1"_sbigint;
    EXPECT_EQ(num1.bitwise_xor(num1), "0"_sbigint);
    EXPECT_EQ(num1.bitwise_xor(num2), "-4"_sbigint);
    EXPECT_EQ(num2.bitwise_xor(num1), "-4"_sbigint);
    EXPECT_EQ(num2.bitwise_xor(num2), "0"_sbigint);
}

TEST_CASE(test_bigint_shift_left)
{
    Crypto::UnsignedBigInteger const num(Vector<u32> { 0x22222222, 0xffffffff });

    size_t const tests = 8;
    AK::Tuple<size_t, Vector<u32>> results[] = {
        { 0, { 0x22222222, 0xffffffff } },
        { 8, { 0x22222200, 0xffffff22, 0x000000ff } },
        { 16, { 0x22220000, 0xffff2222, 0x0000ffff } },
        { 32, { 0x00000000, 0x22222222, 0xffffffff } },
        { 36, { 0x00000000, 0x22222220, 0xfffffff2, 0x0000000f } },
        { 40, { 0x00000000, 0x22222200, 0xffffff22, 0x000000ff } },
        { 64, { 0x00000000, 0x00000000, 0x22222222, 0xffffffff } },
        { 68, { 0x00000000, 0x00000000, 0x22222220, 0xfffffff2, 0x0000000f } },
    };

    for (size_t i = 0; i < tests; ++i)
        EXPECT_EQ(TRY_OR_FAIL(num.shift_left(results[i].get<0>())).words(), results[i].get<1>());
}

TEST_CASE(test_bigint_shift_right)
{
    Crypto::UnsignedBigInteger const num1(Vector<u32> { 0x100, 0x20, 0x4, 0x2, 0x1 });

    size_t const tests1 = 11;
    AK::Tuple<size_t, Vector<u32>> results1[] = {
        { 8, { 0x20000001, 0x04000000, 0x02000000, 0x01000000 } },
        { 16, { 0x00200000, 0x00040000, 0x00020000, 0x00010000 } }, // shift by exact number of words
        { 32, { 0x00000020, 0x00000004, 0x00000002, 0x00000001 } }, // shift by exact number of words
        { 36, { 0x40000002, 0x20000000, 0x10000000 } },
        { 64, { 0x00000004, 0x00000002, 0x00000001 } }, // shift by exact number of words
        { 72, { 0x02000000, 0x01000000 } },
        { 80, { 0x00020000, 0x00010000 } },
        { 88, { 0x00000200, 0x00000100 } },
        { 128, { 0x00000001 } }, // shifted to most significant digit
        { 129, {} },             // all digits have been shifted right
        { 160, {} },
    };

    size_t const tests2 = 2;
    Crypto::UnsignedBigInteger const num2(Vector<u32> { 0x44444444, 0xffffffff });

    AK::Tuple<size_t, Vector<u32>> results2[] = {
        { 1, { 0xa2222222, 0x7fffffff } },
        { 2, { 0xd1111111, 0x3fffffff } },
    };

    for (size_t i = 0; i < tests1; ++i)
        EXPECT_EQ(num1.shift_right(results1[i].get<0>()).words(), results1[i].get<1>());

    for (size_t i = 0; i < tests2; ++i)
        EXPECT_EQ(num2.shift_right(results2[i].get<0>()).words(), results2[i].get<1>());
}

TEST_CASE(test_signed_bigint_fibo500)
{
    Vector<u32> expected_result {
        315178285, 505575602, 1883328078, 125027121,
        3649625763, 347570207, 74535262, 3832543808,
        2472133297, 1600064941, 65273441
    };
    auto result = bigint_signed_fibonacci(500);
    EXPECT_EQ(result.unsigned_value().words(), expected_result);
}

BENCHMARK_CASE(bench_signed_bigint_fib100000)
{
    auto res = bigint_signed_fibonacci(100000);
    (void)res;
}

TEST_CASE(test_signed_addition_edgecase_borrow_with_zero)
{
    Crypto::SignedBigInteger num1 { Crypto::UnsignedBigInteger { { UINT32_MAX - 3, UINT32_MAX } }, false };
    Crypto::SignedBigInteger num2 { Crypto::UnsignedBigInteger { UINT32_MAX - 2 }, false };
    Vector<u32> expected_result { 4294967289, 0, 1 };
    EXPECT_EQ(num1.plus(num2).unsigned_value().words(), expected_result);
}

TEST_CASE(test_signed_addition_edgecase_addition_to_other_sign)
{
    Crypto::SignedBigInteger num1 = INT32_MAX;
    Crypto::SignedBigInteger num2 = num1;
    num2.negate();
    EXPECT_EQ(num1.plus(num2), Crypto::SignedBigInteger { 0 });
}

TEST_CASE(test_signed_subtraction_simple_subtraction_positive_result)
{
    Crypto::SignedBigInteger num1(80);
    Crypto::SignedBigInteger num2(70);
    EXPECT_EQ(num1.minus(num2), Crypto::SignedBigInteger(10));
}

TEST_CASE(test_signed_subtraction_simple_subtraction_negative_result)
{
    Crypto::SignedBigInteger num1(50);
    Crypto::SignedBigInteger num2(70);

    EXPECT_EQ(num1.minus(num2), Crypto::SignedBigInteger { -20 });
}

TEST_CASE(test_signed_subtraction_both_negative)
{
    Crypto::SignedBigInteger num1(-50);
    Crypto::SignedBigInteger num2(-70);

    EXPECT_EQ(num1.minus(num2), Crypto::SignedBigInteger { 20 });
    EXPECT_EQ(num2.minus(num1), Crypto::SignedBigInteger { -20 });
}

TEST_CASE(test_signed_subtraction_simple_subtraction_with_borrow)
{
    Crypto::SignedBigInteger num1(Crypto::UnsignedBigInteger { UINT32_MAX });
    Crypto::SignedBigInteger num2(1);
    Crypto::SignedBigInteger num3 = num1.plus(num2);
    Crypto::SignedBigInteger result = num2.minus(num3);
    num1.negate();
    EXPECT_EQ(result, num1);
}

TEST_CASE(test_signed_subtraction_with_large_numbers)
{
    Crypto::SignedBigInteger num1 = bigint_signed_fibonacci(343);
    Crypto::SignedBigInteger num2 = bigint_signed_fibonacci(218);
    Crypto::SignedBigInteger result = num2.minus(num1);
    auto expected = Crypto::UnsignedBigInteger { Vector<u32> { 811430588, 2958904896, 1130908877, 2830569969, 3243275482, 3047460725, 774025231, 7990 } };
    EXPECT_EQ(result.plus(num1), num2);
    EXPECT_EQ(result.unsigned_value(), expected);
}

TEST_CASE(test_signed_subtraction_with_large_numbers_check_for_assertion)
{
    Crypto::SignedBigInteger num1(Crypto::UnsignedBigInteger { Vector<u32> { 1483061863, 446680044, 1123294122, 191895498, 3347106536, 16, 0, 0, 0 } });
    Crypto::SignedBigInteger num2(Crypto::UnsignedBigInteger { Vector<u32> { 4196414175, 1117247942, 1123294122, 191895498, 3347106536, 16 } });
    Crypto::SignedBigInteger result = num1.minus(num2);
    // this test only verifies that we don't crash on an assertion
    (void)result;
}

TEST_CASE(test_signed_multiplication_with_negative_number)
{
    Crypto::SignedBigInteger num1(8);
    Crypto::SignedBigInteger num2(-251);
    Crypto::SignedBigInteger result = num1.multiplied_by(num2);
    EXPECT_EQ(result, Crypto::SignedBigInteger { -2008 });
}

TEST_CASE(test_signed_multiplication_with_big_number)
{
    Crypto::SignedBigInteger num1 = bigint_signed_fibonacci(200);
    Crypto::SignedBigInteger num2(-12345678);
    Crypto::SignedBigInteger result = num1.multiplied_by(num2);
    Vector<u32> expected_result { 669961318, 143970113, 4028714974, 3164551305, 1589380278, 2 };
    EXPECT_EQ(result.unsigned_value().words(), expected_result);
    EXPECT(result.is_negative());
}

TEST_CASE(test_signed_multiplication_with_two_big_numbers)
{
    Crypto::SignedBigInteger num1 = bigint_signed_fibonacci(200);
    Crypto::SignedBigInteger num2 = bigint_signed_fibonacci(341);
    num1.negate();
    Crypto::SignedBigInteger result = num1.multiplied_by(num2);
    Vector<u32> expected_results {
        3017415433, 2741793511, 1957755698, 3731653885,
        3154681877, 785762127, 3200178098, 4260616581,
        529754471, 3632684436, 1073347813, 2516430
    };
    EXPECT_EQ(result.unsigned_value().words(), expected_results);
    EXPECT(result.is_negative());
}

TEST_CASE(test_negative_zero_is_not_allowed)
{
    Crypto::SignedBigInteger zero(Crypto::UnsignedBigInteger(0), true);
    EXPECT(!zero.is_negative());

    zero.negate();
    EXPECT(!zero.is_negative());

    Crypto::SignedBigInteger positive_five(Crypto::UnsignedBigInteger(5), false);
    Crypto::SignedBigInteger negative_five(Crypto::UnsignedBigInteger(5), true);
    zero = positive_five.plus(negative_five);

    EXPECT(zero.unsigned_value().is_zero());
    EXPECT(!zero.is_negative());
}

TEST_CASE(test_i32_limits)
{
    Crypto::SignedBigInteger min { AK::NumericLimits<i32>::min() };
    EXPECT(min.is_negative());
    EXPECT(min.unsigned_value().to_u64() == static_cast<u32>(AK::NumericLimits<i32>::max()) + 1);

    Crypto::SignedBigInteger max { AK::NumericLimits<i32>::max() };
    EXPECT(!max.is_negative());
    EXPECT(max.unsigned_value().to_u64() == AK::NumericLimits<i32>::max());
}

TEST_CASE(double_comparisons)
{
#define EXPECT_LESS_THAN(bigint, double_value) EXPECT_EQ(bigint.compare_to_double(double_value), Crypto::UnsignedBigInteger::CompareResult::DoubleGreaterThanBigInt)
#define EXPECT_GREATER_THAN(bigint, double_value) EXPECT_EQ(bigint.compare_to_double(double_value), Crypto::UnsignedBigInteger::CompareResult::DoubleLessThanBigInt)
#define EXPECT_EQUAL_TO(bigint, double_value) EXPECT_EQ(bigint.compare_to_double(double_value), Crypto::UnsignedBigInteger::CompareResult::DoubleEqualsBigInt)
    {
        Crypto::SignedBigInteger zero { 0 };
        EXPECT_EQUAL_TO(zero, 0.0);
        EXPECT_EQUAL_TO(zero, -0.0);
    }

    {
        Crypto::SignedBigInteger one { 1 };
        EXPECT_EQUAL_TO(one, 1.0);
        EXPECT_GREATER_THAN(one, -1.0);
        EXPECT_GREATER_THAN(one, 0.5);
        EXPECT_GREATER_THAN(one, -0.5);
        EXPECT_LESS_THAN(one, 1.000001);

        one.negate();
        auto const& negative_one = one;
        EXPECT_EQUAL_TO(negative_one, -1.0);
        EXPECT_LESS_THAN(negative_one, 1.0);
        EXPECT_LESS_THAN(one, 0.5);
        EXPECT_LESS_THAN(one, -0.5);
        EXPECT_GREATER_THAN(one, -1.5);
        EXPECT_LESS_THAN(one, 1.000001);
        EXPECT_GREATER_THAN(one, -1.000001);
    }

    {
        double double_infinity = HUGE_VAL;
        VERIFY(isinf(double_infinity));
        Crypto::SignedBigInteger one { 1 };
        EXPECT_LESS_THAN(one, double_infinity);
        EXPECT_GREATER_THAN(one, -double_infinity);
    }

    {
        double double_max_value = NumericLimits<double>::max();
        double double_below_max_value = nextafter(double_max_value, 0.0);
        VERIFY(double_below_max_value < double_max_value);
        VERIFY(double_below_max_value < (double_max_value - 1.0));
        auto max_value_in_bigint = TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "fffffffffffff800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv));
        auto max_value_plus_one = max_value_in_bigint.plus(Crypto::SignedBigInteger { 1 });
        auto max_value_minus_one = max_value_in_bigint.minus(Crypto::SignedBigInteger { 1 });

        auto below_max_value_in_bigint = TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "fffffffffffff000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv));

        EXPECT_EQUAL_TO(max_value_in_bigint, double_max_value);
        EXPECT_LESS_THAN(max_value_minus_one, double_max_value);
        EXPECT_GREATER_THAN(max_value_plus_one, double_max_value);
        EXPECT_LESS_THAN(below_max_value_in_bigint, double_max_value);

        EXPECT_GREATER_THAN(max_value_in_bigint, double_below_max_value);
        EXPECT_GREATER_THAN(max_value_minus_one, double_below_max_value);
        EXPECT_GREATER_THAN(max_value_plus_one, double_below_max_value);
        EXPECT_EQUAL_TO(below_max_value_in_bigint, double_below_max_value);
    }

    {
        double double_min_value = NumericLimits<double>::lowest();
        double double_above_min_value = nextafter(double_min_value, 0.0);
        VERIFY(double_above_min_value > double_min_value);
        VERIFY(double_above_min_value > (double_min_value + 1.0));
        auto min_value_in_bigint = TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "-fffffffffffff800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv));
        auto min_value_plus_one = min_value_in_bigint.plus(Crypto::SignedBigInteger { 1 });
        auto min_value_minus_one = min_value_in_bigint.minus(Crypto::SignedBigInteger { 1 });

        auto above_min_value_in_bigint = TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "-fffffffffffff000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv));

        EXPECT_EQUAL_TO(min_value_in_bigint, double_min_value);
        EXPECT_LESS_THAN(min_value_minus_one, double_min_value);
        EXPECT_GREATER_THAN(min_value_plus_one, double_min_value);
        EXPECT_GREATER_THAN(above_min_value_in_bigint, double_min_value);

        EXPECT_LESS_THAN(min_value_in_bigint, double_above_min_value);
        EXPECT_LESS_THAN(min_value_minus_one, double_above_min_value);
        EXPECT_LESS_THAN(min_value_plus_one, double_above_min_value);
        EXPECT_EQUAL_TO(above_min_value_in_bigint, double_above_min_value);
    }

    {
        double just_above_255 = bit_cast<double>(0x406fe00000000001ULL);
        double just_below_255 = bit_cast<double>(0x406fdfffffffffffULL);
        double double_255 = 255.0;
        Crypto::SignedBigInteger bigint_255 { 255 };

        EXPECT_EQUAL_TO(bigint_255, double_255);
        EXPECT_GREATER_THAN(bigint_255, just_below_255);
        EXPECT_LESS_THAN(bigint_255, just_above_255);
    }

#undef EXPECT_LESS_THAN
#undef EXPECT_GREATER_THAN
#undef EXPECT_EQUAL_TO
}

TEST_CASE(to_double)
{
#define EXPECT_TO_EQUAL_DOUBLE(bigint, double_value) \
    EXPECT_EQ((bigint).to_double(Crypto::UnsignedBigInteger::RoundingMode::RoundTowardZero), double_value)

    EXPECT_TO_EQUAL_DOUBLE(Crypto::UnsignedBigInteger(0), 0.0);
    // Make sure we don't get negative zero!
    EXPECT_EQ(signbit(Crypto::UnsignedBigInteger(0).to_double()), 0);
    {
        Crypto::SignedBigInteger zero { 0 };

        EXPECT(!zero.is_negative());
        EXPECT_TO_EQUAL_DOUBLE(zero, 0.0);
        EXPECT_EQ(signbit(zero.to_double()), 0);

        zero.negate();

        EXPECT(!zero.is_negative());
        EXPECT_TO_EQUAL_DOUBLE(zero, 0.0);
        EXPECT_EQ(signbit(zero.to_double()), 0);
    }

    EXPECT_TO_EQUAL_DOUBLE(Crypto::UnsignedBigInteger(9682), 9682.0);
    EXPECT_TO_EQUAL_DOUBLE(Crypto::SignedBigInteger(-9660), -9660.0);

    double double_max_value = NumericLimits<double>::max();
    double infinity = INFINITY;

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "fffffffffffff800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv)),
        double_max_value);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "ffffffffffffff00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv)),
        double_max_value);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"sv)),
        double_max_value);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv)),
        infinity);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "-fffffffffffff800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv)),
        -double_max_value);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "-ffffffffffffff00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv)),
        -double_max_value);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "-ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"sv)),
        -double_max_value);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "-10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv)),
        -infinity);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "ffffffffffffffff"sv)),
        18446744073709549568.0);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "fffffffffffff800"sv)),
        18446744073709549568.0);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "fffffffffffff8ff"sv)),
        18446744073709549568.0);

    EXPECT_TO_EQUAL_DOUBLE(TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(10, "1234567890123456789"sv)),
        1234567890123456800.0);

    EXPECT_TO_EQUAL_DOUBLE(TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(10, "2345678901234567890"sv)),
        2345678901234567680.0);

    EXPECT_EQ(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "1fffffffffffff00"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        2305843009213693696.0);

    EXPECT_EQ(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "1fffffffffffff00"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::RoundTowardZero),
        2305843009213693696.0);

    EXPECT_EQ(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "1fffffffffffff80"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        2305843009213693952.0);

    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "20000000000001"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        9007199254740992.0);

    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "20000000000002"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        9007199254740994.0);

    // 2^53 = 20000000000000, +3 Rounds up because of tiesRoundToEven
    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "20000000000003"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        9007199254740996.0);

    // +4 is exactly 9007199254740996
    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "20000000000004"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        9007199254740996.0);

    // +5 rounds down because of tiesRoundToEven
    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "20000000000005"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        9007199254740996.0);

    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "20000000000006"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        9007199254740998.0);

    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(10, "98382635059784269824"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        bit_cast<double>(0x4415555555555555ULL));

#undef EXPECT_TO_EQUAL_DOUBLE
}

TEST_CASE(bigint_from_double)
{
    {
        Crypto::UnsignedBigInteger from_zero { 0.0 };
        EXPECT(from_zero.is_zero());
    }

#define SURVIVES_ROUND_TRIP_UNSIGNED(double_value)            \
    {                                                         \
        Crypto::UnsignedBigInteger bigint { (double_value) }; \
        EXPECT_EQ(bigint.to_double(), (double_value));        \
    }

    SURVIVES_ROUND_TRIP_UNSIGNED(0.0);
    SURVIVES_ROUND_TRIP_UNSIGNED(1.0);
    SURVIVES_ROUND_TRIP_UNSIGNED(100000.0);
    SURVIVES_ROUND_TRIP_UNSIGNED(1000000000000.0);
    SURVIVES_ROUND_TRIP_UNSIGNED(10000000000000000000.0);
    SURVIVES_ROUND_TRIP_UNSIGNED(NumericLimits<double>::max());

    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x4340000000000002ULL));
    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x4340000000000001ULL));
    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x4340000000000000ULL));

    // Failed on last bits of mantissa
    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x7EDFFFFFFFFFFFFFULL));
    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x7ed5555555555555ULL));
    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x7EDCBA9876543210ULL));

    // Has exactly exponent of 32
    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x41f22f74e0000000ULL));

#define SURVIVES_ROUND_TRIP_SIGNED(double_value)                      \
    {                                                                 \
        Crypto::SignedBigInteger bigint_positive { (double_value) };  \
        EXPECT_EQ(bigint_positive.to_double(), (double_value));       \
        Crypto::SignedBigInteger bigint_negative { -(double_value) }; \
        EXPECT_EQ(bigint_negative.to_double(), -(double_value));      \
        EXPECT(bigint_positive != bigint_negative);                   \
        bigint_positive.negate();                                     \
        EXPECT(bigint_positive == bigint_negative);                   \
    }

    {
        // Negative zero should be converted to positive zero
        double const negative_zero = bit_cast<double>(0x8000000000000000);

        // However it should give a bit exact +0.0
        Crypto::SignedBigInteger from_negative_zero { negative_zero };
        EXPECT(from_negative_zero.is_zero());
        EXPECT(!from_negative_zero.is_negative());
        double result = from_negative_zero.to_double();
        EXPECT_EQ(result, 0.0);
        EXPECT_EQ(bit_cast<u64>(result), 0ULL);
    }

    SURVIVES_ROUND_TRIP_SIGNED(1.0);
    SURVIVES_ROUND_TRIP_SIGNED(100000.0);
    SURVIVES_ROUND_TRIP_SIGNED(-1000000000000.0);
    SURVIVES_ROUND_TRIP_SIGNED(10000000000000000000.0);
    SURVIVES_ROUND_TRIP_SIGNED(NumericLimits<double>::max());
    SURVIVES_ROUND_TRIP_SIGNED(NumericLimits<double>::lowest());

    SURVIVES_ROUND_TRIP_SIGNED(bit_cast<double>(0x4340000000000002ULL));
    SURVIVES_ROUND_TRIP_SIGNED(bit_cast<double>(0x4340000000000001ULL));
    SURVIVES_ROUND_TRIP_SIGNED(bit_cast<double>(0x4340000000000000ULL));
    SURVIVES_ROUND_TRIP_SIGNED(bit_cast<double>(0x7EDFFFFFFFFFFFFFULL));
    SURVIVES_ROUND_TRIP_SIGNED(bit_cast<double>(0x7ed5555555555555ULL));
    SURVIVES_ROUND_TRIP_SIGNED(bit_cast<double>(0x7EDCBA9876543210ULL));

#undef SURVIVES_ROUND_TRIP_SIGNED
#undef SURVIVES_ROUND_TRIP_UNSIGNED
}

TEST_CASE(unsigned_bigint_double_comparisons)
{
#define EXPECT_LESS_THAN(bigint, double_value) EXPECT_EQ(bigint.compare_to_double(double_value), Crypto::UnsignedBigInteger::CompareResult::DoubleGreaterThanBigInt)
#define EXPECT_GREATER_THAN(bigint, double_value) EXPECT_EQ(bigint.compare_to_double(double_value), Crypto::UnsignedBigInteger::CompareResult::DoubleLessThanBigInt)
#define EXPECT_EQUAL_TO(bigint, double_value) EXPECT_EQ(bigint.compare_to_double(double_value), Crypto::UnsignedBigInteger::CompareResult::DoubleEqualsBigInt)

    {
        Crypto::UnsignedBigInteger zero { 0 };
        EXPECT_EQUAL_TO(zero, 0.0);
        EXPECT_EQUAL_TO(zero, -0.0);
    }

    {
        Crypto::UnsignedBigInteger one { 1 };
        EXPECT_EQUAL_TO(one, 1.0);
        EXPECT_GREATER_THAN(one, -1.0);
        EXPECT_GREATER_THAN(one, 0.5);
        EXPECT_GREATER_THAN(one, -0.5);
        EXPECT_LESS_THAN(one, 1.000001);
    }

    {
        double double_infinity = HUGE_VAL;
        VERIFY(isinf(double_infinity));
        Crypto::UnsignedBigInteger one { 1 };
        EXPECT_LESS_THAN(one, double_infinity);
        EXPECT_GREATER_THAN(one, -double_infinity);
    }

    {
        double double_max_value = NumericLimits<double>::max();
        double double_below_max_value = nextafter(double_max_value, 0.0);
        VERIFY(double_below_max_value < double_max_value);
        VERIFY(double_below_max_value < (double_max_value - 1.0));
        auto max_value_in_bigint = TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "fffffffffffff800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv));
        auto max_value_plus_one = max_value_in_bigint.plus(Crypto::UnsignedBigInteger { 1 });
        auto max_value_minus_one = TRY_OR_FAIL(max_value_in_bigint.minus(Crypto::UnsignedBigInteger { 1 }));

        auto below_max_value_in_bigint = TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "fffffffffffff000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv));

        EXPECT_EQUAL_TO(max_value_in_bigint, double_max_value);
        EXPECT_LESS_THAN(max_value_minus_one, double_max_value);
        EXPECT_GREATER_THAN(max_value_plus_one, double_max_value);
        EXPECT_LESS_THAN(below_max_value_in_bigint, double_max_value);

        EXPECT_GREATER_THAN(max_value_in_bigint, double_below_max_value);
        EXPECT_GREATER_THAN(max_value_minus_one, double_below_max_value);
        EXPECT_GREATER_THAN(max_value_plus_one, double_below_max_value);
        EXPECT_EQUAL_TO(below_max_value_in_bigint, double_below_max_value);
    }

    {
        double just_above_255 = bit_cast<double>(0x406fe00000000001ULL);
        double just_below_255 = bit_cast<double>(0x406fdfffffffffffULL);
        double double_255 = 255.0;
        Crypto::UnsignedBigInteger bigint_255 { 255 };

        EXPECT_EQUAL_TO(bigint_255, double_255);
        EXPECT_GREATER_THAN(bigint_255, just_below_255);
        EXPECT_LESS_THAN(bigint_255, just_above_255);
    }

#undef EXPECT_LESS_THAN
#undef EXPECT_GREATER_THAN
#undef EXPECT_EQUAL_TO
}

namespace AK {

template<>
struct Formatter<Crypto::UnsignedBigInteger::CompareResult> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Crypto::UnsignedBigInteger::CompareResult const& compare_result)
    {
        switch (compare_result) {
        case Crypto::UnsignedBigInteger::CompareResult::DoubleEqualsBigInt:
            return builder.put_string("Equals"sv);
        case Crypto::UnsignedBigInteger::CompareResult::DoubleLessThanBigInt:
            return builder.put_string("LessThan"sv);
        case Crypto::UnsignedBigInteger::CompareResult::DoubleGreaterThanBigInt:
            return builder.put_string("GreaterThan"sv);
        default:
            return builder.put_string("???"sv);
        }
    }
};

}
