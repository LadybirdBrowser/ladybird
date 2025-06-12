/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/CharacterTypes.h>
#include <ctype.h>

#define ASCII 0x80
#define UNICODE 0x10FFFF + 100

using namespace Test::Randomized;

void compare_bool_output_over(u32 range, auto& old_function, auto& new_function)
{
    bool result1 = false;
    bool result2 = false;
    for (u32 i = 0; i < range; ++i) {
        EXPECT_EQ(result1 = (old_function(i) > 0), result2 = (new_function(i) > 0));
        if (result1 != result2)
            FAIL(String::formatted("New result {} does not match old result {} for input {}.", result1, result2, i));
    }
}

void compare_value_output_over(u32 range, auto& old_function, auto& new_function)
{
    i64 result1 = 0;
    i64 result2 = 0;
    for (u32 i = 0; i < range; ++i) {
        EXPECT_EQ(result1 = old_function(i), result2 = new_function(i));
        if (result1 != result2)
            FAIL(String::formatted("New result {} does not match old result {} for input {}.", result1, result2, i));
    }
}

void randomized_compare_bool_output_over(u32 range, auto& old_function, auto& new_function)
{
    // NOTE: randomized tests also run multiple times (100 by default). This means we'll try 10k random numbers times each time the test suite is run.
    for (u32 n = 0; n < 100; ++n) {
        bool result1 = false;
        bool result2 = false;
        GEN(i, Gen::number_u64(range - 1));
        EXPECT_EQ(result1 = (old_function(i) > 0), result2 = (new_function(i) > 0));
        if (result1 != result2)
            FAIL(String::formatted("New result {} does not match old result {} for input {}.", result1, result2, i));
    }
}

void randomized_compare_value_output_over(u32 range, auto& old_function, auto& new_function)
{
    // NOTE: randomized tests also run multiple times (100 by default). This means we'll try 10k random numbers times each time the test suite is run.
    for (u32 n = 0; n < 100; ++n) {
        i64 result1 = false;
        i64 result2 = false;
        GEN(i, Gen::number_u64(range - 1));
        EXPECT_EQ(result1 = old_function(i), result2 = new_function(i));
        if (result1 != result2)
            FAIL(String::formatted("New result {} does not match old result {} for input {}.", result1, result2, i));
    }
}

TEST_CASE(is_ascii_alphanumeric)
{
    compare_bool_output_over(ASCII, isalnum, is_ascii_alphanumeric);
}

TEST_CASE(is_ascii_base36_digit)
{
    constexpr Array valid_base36_digits { '0', '9', 'A', 'Z', 'a', 'z' };
    for (auto valid_base36_digit : valid_base36_digits)
        EXPECT_EQ(is_ascii_base36_digit(valid_base36_digit), true);

    constexpr Array invalid_base36_digits { '/', ':', '@', '[', '`', '{' };
    for (auto invalid_base36_digit : invalid_base36_digits)
        EXPECT_EQ(is_ascii_base36_digit(invalid_base36_digit), false);
}

TEST_CASE(is_ascii_blank)
{
    compare_bool_output_over(ASCII, isblank, is_ascii_blank);
}

TEST_CASE(is_ascii_c0_control)
{
    compare_bool_output_over(ASCII - 1, iscntrl, is_ascii_c0_control);
}

TEST_CASE(is_ascii_control)
{
    compare_bool_output_over(ASCII, iscntrl, is_ascii_control);
}

TEST_CASE(is_ascii_digit)
{
    compare_bool_output_over(ASCII, isdigit, is_ascii_digit);
}

TEST_CASE(is_ascii_graphical)
{
    compare_bool_output_over(ASCII, isgraph, is_ascii_graphical);
}

TEST_CASE(is_ascii_hex_digit)
{
    compare_bool_output_over(ASCII, isxdigit, is_ascii_hex_digit);
}

TEST_CASE(is_ascii_lower_alpha)
{
    compare_bool_output_over(ASCII, islower, is_ascii_lower_alpha);
}

TEST_CASE(is_ascii_printable)
{
    compare_bool_output_over(ASCII, isprint, is_ascii_printable);
}

TEST_CASE(is_ascii_punctuation)
{
    compare_bool_output_over(ASCII, ispunct, is_ascii_punctuation);
}

TEST_CASE(is_ascii_space)
{
    compare_bool_output_over(ASCII, isspace, is_ascii_space);
}

TEST_CASE(is_ascii_upper_alpha)
{
    compare_bool_output_over(ASCII, isupper, is_ascii_upper_alpha);
}

TEST_CASE(to_ascii_lowercase)
{
    compare_value_output_over(ASCII, tolower, to_ascii_lowercase);
}

TEST_CASE(to_ascii_uppercase)
{
    compare_value_output_over(ASCII, toupper, to_ascii_uppercase);
}

TEST_CASE(parse_ascii_base36_digit)
{
    EXPECT_EQ(parse_ascii_base36_digit('0'), 0u);
    EXPECT_EQ(parse_ascii_base36_digit('9'), 9u);
    EXPECT_EQ(parse_ascii_base36_digit('A'), 10u);
    EXPECT_EQ(parse_ascii_base36_digit('Z'), 35u);
    EXPECT_EQ(parse_ascii_base36_digit('a'), 10u);
    EXPECT_EQ(parse_ascii_base36_digit('z'), 35u);

    EXPECT_DEATH("parsing Base36 digit before valid numeric range", parse_ascii_base36_digit('/'));
    EXPECT_DEATH("parsing Base36 digit after valid numeric range", parse_ascii_base36_digit(':'));
    EXPECT_DEATH("parsing Base36 digit before valid uppercase range", parse_ascii_base36_digit('@'));
    EXPECT_DEATH("parsing Base36 digit after valid uppercase range", parse_ascii_base36_digit('['));
    EXPECT_DEATH("parsing Base36 digit before valid lowercase range", parse_ascii_base36_digit('`'));
    EXPECT_DEATH("parsing Base36 digit after valid lowercase range", parse_ascii_base36_digit('{'));
}

TEST_CASE(parse_ascii_digit)
{
    EXPECT_EQ(parse_ascii_digit('0'), 0u);
    EXPECT_EQ(parse_ascii_digit('9'), 9u);

    EXPECT_DEATH("parsing invalid ASCII digit", parse_ascii_digit('a'));
    EXPECT_DEATH("parsing invalid unicode digit", parse_ascii_digit(0x00A9));
}

TEST_CASE(parse_ascii_hex_digit)
{
    EXPECT_EQ(parse_ascii_hex_digit('0'), 0u);
    EXPECT_EQ(parse_ascii_hex_digit('F'), 15u);
    EXPECT_EQ(parse_ascii_hex_digit('f'), 15u);

    EXPECT_DEATH("parsing invalid ASCII hex digit", parse_ascii_hex_digit('g'));
}

BENCHMARK_CASE(is_ascii)
{
    compare_bool_output_over(UNICODE, isascii, is_ascii);
}

BENCHMARK_CASE(to_ascii_lowercase_unicode)
{
    compare_value_output_over(UNICODE, tolower, to_ascii_lowercase);
}

BENCHMARK_CASE(to_ascii_uppercase_unicode)
{
    compare_value_output_over(UNICODE, toupper, to_ascii_uppercase);
}

// NOTE: This would take too long to run exhaustively. Let's at least run random subsets of it!
RANDOMIZED_TEST_CASE(is_ascii_unicode)
{
    randomized_compare_bool_output_over(UNICODE, isascii, is_ascii);
}

RANDOMIZED_TEST_CASE(to_ascii_lowercase_unicode)
{
    randomized_compare_value_output_over(UNICODE, tolower, to_ascii_lowercase);
}

RANDOMIZED_TEST_CASE(to_ascii_uppercase_unicode)
{
    randomized_compare_value_output_over(UNICODE, toupper, to_ascii_uppercase);
}
