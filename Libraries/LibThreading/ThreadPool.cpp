/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibThreading/ThreadPool.h>

static constexpr size_t THREAD_COUNT = 4;
static constexpr size_t THREAD_STACK_SIZE = 8 * MiB;

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
    while (true) {
        Function<void()> work;

        {
            Sync::MutexLocker locker(m_mutex);
            m_condition.wait_while([this] { return m_work_queue.is_empty(); });
            work = m_work_queue.dequeue();
        }

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
