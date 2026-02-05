/*
 * Copyright (c) 2025-2026, Colleirose <criticskate@pm.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteBuffer.h>
#include <AK/Random.h>

TEST_CASE(csprng_generates_unique_values)
{
    constexpr size_t size = 800;
    constexpr size_t max_failures = 3;
    size_t failures = 0;

    for (size_t i = 0; i < 3; i++) {
        ByteBuffer first_buffer = MUST(ByteBuffer::create_zeroed(size));
        ByteBuffer second_buffer = MUST(ByteBuffer::create_zeroed(size));

        AK::fill_with_random(first_buffer);
        AK::fill_with_random(second_buffer);

        u32 first_u32 = AK::get_random_uniform(size);
        u32 second_u32 = AK::get_random_uniform(size);

        u64 first_u64 = AK::get_random_uniform_64(size);
        u64 second_u64 = AK::get_random_uniform_64(size);

        if (first_buffer == second_buffer)
            failures++;

        if (first_u32 == second_u32)
            failures++;

        if (first_u64 == second_u64)
            failures++;
    }

    EXPECT(failures < max_failures);
}
