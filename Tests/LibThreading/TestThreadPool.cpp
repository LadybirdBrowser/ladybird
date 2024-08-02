/*
 * Copyright (c) 2024, Braydn Moore <braydn.moore@uwaterloo.ca>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibCore/ElapsedTimer.h>
#include <LibTest/TestCase.h>
#include <LibThreading/ThreadPool.h>

using namespace AK::TimeLiterals;

TEST_CASE(thread_pool_deadlock)
{
    static constexpr auto RUN_TIMEOUT = 120_sec;
    static constexpr u64 NUM_RUNS = 1000;
    static constexpr u64 MAX_VALUE = 1 << 15;

    for (u64 i = 0; i < NUM_RUNS; ++i) {
        u64 expected_value = (MAX_VALUE * (MAX_VALUE + 1)) / 2;
        Atomic<u64> sum;

        // heap allocate the ThreadPool in case it deadlocks. Exiting in the
        // case of a deadlock will purposefully leak memory to avoid calling the
        // destructor and hanging the test
        auto* thread_pool = new Threading::ThreadPool<u64>(
            [&sum](u64 current_val) {
                sum += current_val;
            });

        for (u64 j = 0; j <= MAX_VALUE; ++j) {
            thread_pool->submit(j);
        }

        auto join_thread = Threading::Thread::construct([thread_pool]() -> intptr_t {
            thread_pool->wait_for_all();
            delete thread_pool;
            return 0;
        });

        join_thread->start();
        auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        while (!join_thread->has_exited() && timer.elapsed_milliseconds() < RUN_TIMEOUT.to_milliseconds())
            ;
        EXPECT(join_thread->has_exited());
        // exit since the current pool is deadlocked and we have no way of
        // unblocking the pool other than having the OS teardown the process
        // struct
        if (!join_thread->has_exited()) {
            return;
        }

        (void)join_thread->join();
        EXPECT_EQ(sum.load(), expected_value);
    }
}
