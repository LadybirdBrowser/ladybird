/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Hex.h>

TEST_CASE(should_decode_hex_digit)
{
    EXPECT_EQ(0u, decode_hex_digit('0'));
    EXPECT_EQ(1u, decode_hex_digit('1'));
    EXPECT_EQ(2u, decode_hex_digit('2'));
    EXPECT_EQ(3u, decode_hex_digit('3'));
    EXPECT_EQ(4u, decode_hex_digit('4'));
    EXPECT_EQ(5u, decode_hex_digit('5'));
    EXPECT_EQ(6u, decode_hex_digit('6'));
    EXPECT_EQ(7u, decode_hex_digit('7'));
    EXPECT_EQ(8u, decode_hex_digit('8'));
    EXPECT_EQ(9u, decode_hex_digit('9'));
    EXPECT_EQ(10u, decode_hex_digit('a'));
    EXPECT_EQ(11u, decode_hex_digit('b'));
    EXPECT_EQ(12u, decode_hex_digit('c'));
    EXPECT_EQ(13u, decode_hex_digit('d'));
    EXPECT_EQ(14u, decode_hex_digit('e'));
    EXPECT_EQ(15u, decode_hex_digit('f'));
    EXPECT_EQ(10u, decode_hex_digit('A'));
    EXPECT_EQ(11u, decode_hex_digit('B'));
    EXPECT_EQ(12u, decode_hex_digit('C'));
    EXPECT_EQ(13u, decode_hex_digit('D'));
    EXPECT_EQ(14u, decode_hex_digit('E'));
    EXPECT_EQ(15u, decode_hex_digit('F'));
}

TEST_CASE(should_constexpr_decode_hex_digit)
{
    static_assert(0u == decode_hex_digit('0'));
    static_assert(1u == decode_hex_digit('1'));
    static_assert(2u == decode_hex_digit('2'));
    static_assert(3u == decode_hex_digit('3'));
    static_assert(4u == decode_hex_digit('4'));
    static_assert(5u == decode_hex_digit('5'));
    static_assert(6u == decode_hex_digit('6'));
    static_assert(7u == decode_hex_digit('7'));
    static_assert(8u == decode_hex_digit('8'));
    static_assert(9u == decode_hex_digit('9'));
    static_assert(10u == decode_hex_digit('a'));
    static_assert(11u == decode_hex_digit('b'));
    static_assert(12u == decode_hex_digit('c'));
    static_assert(13u == decode_hex_digit('d'));
    static_assert(14u == decode_hex_digit('e'));
    static_assert(15u == decode_hex_digit('f'));
    static_assert(10u == decode_hex_digit('A'));
    static_assert(11u == decode_hex_digit('B'));
    static_assert(12u == decode_hex_digit('C'));
    static_assert(13u == decode_hex_digit('D'));
    static_assert(14u == decode_hex_digit('E'));
    static_assert(15u == decode_hex_digit('F'));
}
