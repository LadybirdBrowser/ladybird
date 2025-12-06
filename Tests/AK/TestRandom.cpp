/*
 * Copyright (c) 2025, Colleirose <criticskate@pm.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteBuffer.h>
#include <AK/Random.h>

TEST_CASE(csprng_generates_unique_values)
{
    ByteBuffer first_buffer = MUST(ByteBuffer::create_zeroed(100));
    ByteBuffer second_buffer = MUST(ByteBuffer::create_zeroed(100));

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
