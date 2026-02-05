/*
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/BigFraction/BigFraction.h>
#include <LibTest/TestCase.h>

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

TEST_CASE(roundtrip_from_string)
{
    Array valid_number_strings {
        "0.1"sv,
        "-0.1"sv,
        "0.9"sv,
        "-0.9"sv,
        "1.2"sv,
        "-1.2"sv,
        "610888968122787804679.305596150292503043363"sv,
        "-610888968122787804679.305596150292503043363"sv
    };

    for (auto valid_number_string : valid_number_strings) {
        auto result = TRY_OR_FAIL(Crypto::BigFraction::from_string(valid_number_string));
        auto precision = valid_number_string.length() - valid_number_string.find('.').value();
        EXPECT_EQ(result.to_string(precision), valid_number_string);
    }
}

TEST_CASE(big_fraction_to_double)
{
    // Golden ratio:
    //  - limit (inf) ratio of two consecutive fibonacci numbers
    //  - also ( 1 + sqrt( 5 ))/2
    Crypto::BigFraction phi(Crypto::SignedBigInteger { bigint_fibonacci(500) }, bigint_fibonacci(499));
    // Power 64 of golden ratio:
    //  - limit ratio of two 64-separated fibonacci numbers
    //  - also (23725150497407 + 10610209857723 * sqrt( 5 ))/2
    Crypto::BigFraction phi_64(Crypto::SignedBigInteger { bigint_fibonacci(564) }, bigint_fibonacci(500));

    EXPECT_EQ(phi.to_double(), 1.618033988749895); //  1.6180339887498948482045868343656381177203091798057628621... (https://oeis.org/A001622)
    EXPECT_EQ(phi_64.to_double(), 23725150497407); //  23725150497406.9999999999999578506361799772097881088769... (https://www.calculator.net/big-number-calculator.html)
}

TEST_CASE(big_fraction_temporal_duration_precision_support)
{
    // https://github.com/tc39/test262/blob/main/test/built-ins/Temporal/Duration/prototype/total/precision-exact-mathematical-values-1.js
    // Express 4000h and 1ns in hours, as a double
    Crypto::BigFraction temporal_duration_precision_test = Crypto::BigFraction { Crypto::SignedBigInteger { "14400000000000001"_bigint }, "3600000000000"_bigint };

    EXPECT_EQ(temporal_duration_precision_test.to_double(), 4000.0000000000005);
}
