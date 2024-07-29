/*
 * Copyright (c) 2024, Braydn Moore <braydn.moore@uwaterloo.ca>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/Random.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibThreading/MutexProtected.h>
#include <LibThreading/WorkStealingThreadPool.h>
#include <pthread.h>

struct ThreadUtilizationTracker {
    AK_MAKE_NONCOPYABLE(ThreadUtilizationTracker);
    AK_MAKE_NONMOVABLE(ThreadUtilizationTracker);

public:
    ThreadUtilizationTracker(size_t num_threads)
        : m_num_thread(num_threads)
    {
    }

    void track_job()
    {
        m_total_jobs_done += 1;
        m_total_jobs_done_per_thread.with_locked([](auto& map) {
            auto tid = pthread_self();
            auto count = map.get(tid).value_or(0);
            map.set(tid, count + 1);
        });
    }

    void ensure_even_utilization(double acceptable_utilization_range)
    {
        double completely_fair_distribution = 1.0f / m_num_thread;
        m_total_jobs_done_per_thread.with_locked([this, completely_fair_distribution, acceptable_utilization_range](auto& counts) {
            EXPECT_EQ(counts.size(), m_num_thread);
            for (auto const& entry : counts) {
                double amount_of_work_done = double(entry.value) / double(m_total_jobs_done.load());
                EXPECT_APPROXIMATE_WITH_ERROR(amount_of_work_done, completely_fair_distribution, acceptable_utilization_range);
            }
        });
    }

private:
    size_t m_num_thread;
    AK::Atomic<u64> m_total_jobs_done;
    Threading::MutexProtected<AK::HashMap<pthread_t, u64>> m_total_jobs_done_per_thread;
};

template<typename Callback = Function<void()>>
static void run_threaded_summation(u64 min, u64 max, Optional<size_t> num_threads = {}, Optional<Callback> cb = {})
{
    for (u64 max_value = min; max_value <= max; max_value <<= 1) {
        u64 expected_value = (max_value * (max_value + 1)) / 2;
        Atomic<u64> sum;
        auto thread_pool = Threading::WorkStealingThreadPool<u64> {
            [&sum, &cb, max_value](Function<void(u64)> submit, u64 current_val) {
                sum += current_val;
                if (cb.has_value()) {
                    (*cb)();
                }
                for (u64 i = current_val * 4 + 1; i <= max_value && i <= current_val * 4 + 4; ++i) {
                    submit(i);
                }
            },
            num_threads
        };

        thread_pool.submit(0);
        thread_pool.wait_for_all();
        EXPECT(sum == expected_value);
    }
}

TEST_CASE(work_stealing_sum)
{
    run_threaded_summation(1 << 10, 1 << 20);
}

RANDOMIZED_TEST_CASE(work_stealing_sum_race_condition)
{
    run_threaded_summation(1 << 6, 1 << 12);
}

RANDOMIZED_TEST_CASE(work_stealing_thread_utilization_even_job_distribution)
{
    static constexpr size_t NUM_THREADS = 8;
    static constexpr double UTILIZATION_RANGE = 0.05;

    ThreadUtilizationTracker tracker(NUM_THREADS);
    auto utilization_func = [&tracker]() {
        tracker.track_job();
    };

    run_threaded_summation(1 << 15, 1 << 15, NUM_THREADS, AK::Optional<decltype(utilization_func)>(utilization_func));
    tracker.ensure_even_utilization(UTILIZATION_RANGE);
}

RANDOMIZED_TEST_CASE(work_stealing_thread_utilization_uneven_job_distribution)
{
    static constexpr size_t NUM_ITEMS = 1 << 15;
    static constexpr double UTILIZATION_RANGE = 0.1;
    static constexpr size_t MIN_ITEMS_EXPLORED = 1;
    static constexpr size_t MAX_ITEMS_EXPLORED = 8;
    static constexpr size_t NUM_THREADS = 8;

    AK::Vector<AK::NonnullOwnPtr<Atomic<bool>>> work;
    work.ensure_capacity(NUM_ITEMS);
    for (size_t i = 0; i < NUM_ITEMS; ++i) {
        work.unchecked_append(make<Atomic<bool>>(false));
    }

    ThreadUtilizationTracker tracker(NUM_THREADS);
    {
        auto thread_pool = Threading::WorkStealingThreadPool<size_t> {
            [&work, &tracker](Function<void(u64)> submit, size_t current_val) {
                tracker.track_job();
                if (work[current_val]->load()) {
                    return;
                }

                work[current_val]->store(true);
                size_t num_to_explore = MIN_ITEMS_EXPLORED + AK::get_random_uniform(MAX_ITEMS_EXPLORED - MIN_ITEMS_EXPLORED + 1);

                for (size_t item = current_val + 1; item <= current_val + num_to_explore && item < NUM_ITEMS; ++item) {
                    submit(item);
                }
            },
            NUM_THREADS
        };

        thread_pool.submit(0);
        thread_pool.wait_for_all();
        for (auto const& item : work) {
            EXPECT(item->load());
        }

        tracker.ensure_even_utilization(UTILIZATION_RANGE);
    }
}
