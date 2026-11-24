/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/Time.h>
#include <LibTest/TestCase.h>
#include <LibThreading/ThreadPool.h>

namespace {

struct PoolTestState : public AtomicRefCounted<PoolTestState> {
    Mutex mutex;
    ConditionVariable condition { mutex };
    size_t started { 0 };
    size_t finished { 0 };
    bool release { false };
    bool other_pool_ran { false };

    bool wait_until(Function<bool()> predicate)
    {
        MutexLocker locker(mutex);
        auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(5);
        while (!predicate()) {
            auto remaining = deadline - MonotonicTime::now();
            if (remaining <= AK::Duration {})
                return false;
            condition.wait_for(remaining);
        }
        return true;
    }
};

void expect_pool_isolation(Threading::ThreadPool& blocked_pool, Threading::ThreadPool& other_pool)
{
    auto state = make_ref_counted<PoolTestState>();
    auto worker_count = blocked_pool.worker_count();
    for (size_t i = 0; i < worker_count; ++i) {
        blocked_pool.submit([state] {
            MutexLocker locker(state->mutex);
            ++state->started;
            state->condition.broadcast();
            while (!state->release)
                state->condition.wait();
            ++state->finished;
            state->condition.broadcast();
        });
    }

    EXPECT(state->wait_until([&] { return state->started == worker_count; }));
    other_pool.submit([state] {
        MutexLocker locker(state->mutex);
        state->other_pool_ran = true;
        state->condition.broadcast();
    });
    EXPECT(state->wait_until([&] { return state->other_pool_ran; }));

    // Release and drain every submitted job even when an expectation failed.
    MutexLocker locker(state->mutex);
    state->release = true;
    state->condition.broadcast();
    while (state->finished != worker_count || !state->other_pool_ran)
        state->condition.wait();
}

}

TEST_CASE(name_resolution_runs_when_the_shared_pool_is_blocked)
{
    expect_pool_isolation(Threading::ThreadPool::the(), Threading::ThreadPool::for_name_resolution());
}

TEST_CASE(shared_work_runs_when_the_name_resolution_pool_is_blocked)
{
    expect_pool_isolation(Threading::ThreadPool::for_name_resolution(), Threading::ThreadPool::the());
}
