/*
 * Copyright (c) 2024, Braydn Moore <braydn.moore@uwaterloo.ca>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IterationDecision.h>
#include <LibThreading/ThreadPool.h>

namespace Threading {
/**
 * Each work-stealing worker keeps a local work queue in an attempt to access
 * the global queue as infrequently as possible to improve performance.
 *
 * For a given ThreadPool with N threads, a thread accessing the global queue
 * will drain 1/Nth of the jobs in the global queue into it's local work
 * queue.
 *
 * In an attempt to share the work cooperatively, every
 * `SHARE_INTERVAL` jobs which are completed a worker will
 * donate 1/`GLOBAL_DONATE_RATIO` of it's local queue to the if:
 *   1. The local queue has more than `MIN_NUMBER_OF_LOCAL_JOBS`
 *   2. The global queue is empty
 *   3. The global queue lock is not currently held by another worker
 */

static size_t distribute_id = 0;
static constexpr size_t WORK_STEALING_SHARE_INTERVAL_DEFAULT = 128;
static constexpr size_t WORK_STEALING_GLOBAL_DONATE_RATIO_DEFAULT = 2;
static constexpr size_t WORK_STEALING_MIN_NUMBER_OF_LOCAL_JOBS_DEFAULT = 4;

template<size_t SHARE_INTERVAL = WORK_STEALING_SHARE_INTERVAL_DEFAULT,
    size_t GLOBAL_DONATE_RATIO = WORK_STEALING_GLOBAL_DONATE_RATIO_DEFAULT,
    size_t MIN_NUMBER_OF_LOCAL_JOBS = WORK_STEALING_MIN_NUMBER_OF_LOCAL_JOBS_DEFAULT>
struct WorkStealingLooper {

    template<typename Pool>
    class Looper {
    public:
        Looper()
            : id(distribute_id++)
            , m_local_queue()
            , m_jobs_since_last_share(0)
            , m_jobs_ran(0)
        {
        }

        AK::IterationDecision next(Pool& pool, bool wait)
        {
            // The only time a work-stealing looper should yield to the main thread
            // loop is if there is no local jobs (generally this should also be
            // after an attempt to replenish the local queue with jobs from the
            // global queue)
            VERIFY(m_local_queue.is_empty());
            while (true) {
                if (do_work(pool)) {
                    return IterationDecision::Continue;
                }

                if (pool.looper_should_exit())
                    return IterationDecision::Break;

                if (!wait)
                    return IterationDecision::Continue;

                pool.looper_wait();
            }
        }

    private:
        bool do_work(Pool& pool)
        {
            auto guard = pool.looper_enter_busy_section();
            // Attempt to replenish the local queue with 1/Nth of the global
            // queue if this worker has no work or exit if requested
            if (m_local_queue.is_empty()) {
                pool.looper_with_global_queue([this, num_workers = pool.looper_num_workers()](auto& queue) {
                    size_t num_jobs = max(1, queue.size() / num_workers);
                    for (size_t i = 0; i < num_jobs && !queue.is_empty(); ++i) {
                        m_local_queue.enqueue(queue.dequeue());
                    }
                });
            }

            // if there are still no jobs even after checking the global queue yield
            if (m_local_queue.is_empty()) {
                return false;
            }

            // Run handler on jobs in the local queue
            while (!m_local_queue.is_empty()) {
                pool.looper_run_handler([this](typename Pool::Work w) { m_local_queue.enqueue(w); }, m_local_queue.dequeue());
                ++m_jobs_since_last_share;
                ++m_jobs_ran;
                // Share jobs with the global queue if criteria is met
                if (m_jobs_since_last_share >= SHARE_INTERVAL && m_local_queue.size() >= MIN_NUMBER_OF_LOCAL_JOBS) {
                    m_jobs_since_last_share = 0;
                    pool.looper_try_with_global_queue([this, &pool](auto& queue) {
                        if (!queue.is_empty()) {
                            return;
                        }

                        for (size_t i = 0; i < m_local_queue.size() / GLOBAL_DONATE_RATIO; ++i) {
                            queue.enqueue(m_local_queue.dequeue());
                        }
                        pool.looper_signal_work_available();
                    });
                }
            }

            return true;
        }

        size_t id;
        Queue<typename Pool::Work> m_local_queue;
        size_t m_jobs_since_last_share;
        size_t m_jobs_ran;
    };
};

template<typename TWork>
using WorkStealingThreadPool = ThreadPool<TWork, WorkStealingLooper<>::Looper>;
}
