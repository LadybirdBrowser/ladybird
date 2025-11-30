/*
 * Copyright (c) 2025, Colleirose <criticskate@pm.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Random.h>

TEST_CASE(csprng_generates_unique_values)
{
    auto first_buffer = ByteBuffer::create_zeroed(100).release_value();
    auto second_buffer = ByteBuffer::create_zeroed(100).release_value();

    AK::fill_with_random(first_buffer);
    AK::fill_with_random(second_buffer);

    u32 first_u32 = get_random_uniform(100);
    u32 second_u32 = get_random_uniform(100);

    u64 first_u64 = AK::get_random_uniform_64(100);
    u64 second_u64 = AK::get_random_uniform_64(100);

    EXPECT_NE(first_buffer, second_buffer);
    EXPECT_NE(first_u32, second_u32);
    EXPECT_NE(first_u64, second_u64);
}
