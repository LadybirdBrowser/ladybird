/*
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/BigFraction/BigFraction.h>
#include <LibTest/TestCase.h>

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
