/*
 * Copyright (c) 2024, Braydn Moore <braydn.moore@uwaterloo.ca>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibTest/TestCase.h>
#include <LibThreading/ThreadPool.h>

using namespace AK::TimeLiterals;

RANDOMIZED_TEST_CASE(thread_pool_race_condition)
{
    static constexpr u64 MIN_SUM_TO = 1 << 10;
    static constexpr u64 MAX_SUM_TO = 1 << 15;
    static constexpr auto SUM_SLEEP_TIME = 2_us;

    for (u64 max_value = MIN_SUM_TO; max_value <= MAX_SUM_TO; max_value <<= 1) {
        u64 expected_value = (max_value * (max_value + 1)) / 2;
        Atomic<u64> sum;
        auto thread_pool = Threading::ThreadPool<u64> {
            [&sum](u64 current_val) {
                sum += current_val;
                usleep(SUM_SLEEP_TIME.to_microseconds());
            },
        };

        for (u64 i = 0; i <= max_value; ++i) {
            thread_pool.submit(i);
        }
        thread_pool.wait_for_all();
        EXPECT(sum == expected_value);
    }
}
