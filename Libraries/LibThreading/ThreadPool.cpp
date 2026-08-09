/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <AK/kmalloc.h>
#include <LibThreading/ThreadPool.h>

static constexpr size_t THREAD_COUNT = 4;
static constexpr size_t THREAD_STACK_SIZE = 8 * MiB;
static constexpr auto THREAD_IDLE_MEMORY_COLLECTION_DELAY = AK::Duration::from_seconds(1);

namespace Threading {

ThreadPool& ThreadPool::the()
{
    static ThreadPool* instance = new ThreadPool;
    return *instance;
}

ThreadPool::ThreadPool()
{
    for (size_t i = 0; i < THREAD_COUNT; ++i) {
        auto name = ByteString::formatted("Pool/{}", i);
        auto thread = Thread::construct(name, [this]() -> intptr_t {
            return worker_thread_func();
        });
        thread->set_stack_size(THREAD_STACK_SIZE);
        thread->start();
        m_threads.append(move(thread));
    }
}

intptr_t ThreadPool::worker_thread_func()
{
    bool collected_since_last_work = false;

    while (true) {
        Function<void()> work;
        bool should_collect = false;
        {
            Sync::MutexLocker locker(m_mutex);

            while (m_work_queue.is_empty()) {
                if (collected_since_last_work) {
                    m_condition.wait();
                } else if (!m_condition.wait_for(THREAD_IDLE_MEMORY_COLLECTION_DELAY) && m_work_queue.is_empty()) {
                    should_collect = true;
                    break;
                }
            }

            if (!should_collect)
                work = m_work_queue.dequeue();
        }

        if (should_collect) {
            ak_kmalloc_collect();
            collected_since_last_work = true;
            continue;
        }

        collected_since_last_work = false;
        work();
    }
}

void ThreadPool::submit(Function<void()> work)
{
    Sync::MutexLocker locker(m_mutex);
    m_work_queue.enqueue(move(work));
    m_condition.signal();
}

}
