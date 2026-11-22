/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Engine/FetchExecutor.h>

namespace Web::Fetch::Engine {

FetchExecutor::FetchExecutor(FetchExecutorServices services, NonnullOwnPtr<EngineCommandSink> engine)
    : m_services(move(services))
    , m_engine(move(engine))
{
    m_engine->attach_to_executor(*this);
}

FetchExecutor::~FetchExecutor() = default;

void FetchExecutor::post(EngineCommand command)
{
    enqueue(move(command));
}

void FetchExecutor::defer(Badge<FetchEngineRecords>, Function<void()> work)
{
    enqueue(move(work));
}

void FetchExecutor::enqueue(PendingWork work)
{
    if (m_shut_down.load(AK::MemoryOrder::memory_order_acquire))
        return;

    // NB: A task runs every pending command and deferred work, oldest first, and not just the one it was posted with. A
    //     task can run in a nested event loop, such as the one a sync XHR spins, while tasks posted before it still wait
    //     in the outer loop. The engine must still see the commands in posting order, and the nested loop must also run
    //     the commands it waits for.
    {
        MutexLocker locker { m_pending_work_mutex };
        m_pending_work.enqueue(move(work));
    }

    // The task keeps the executor, and so the engine, alive until it has run or been dropped.
    auto run_pending_work = [self = NonnullRefPtr(*this)] { self->run_pending_work(); };
    if (!post_task(move(run_pending_work)))
        drop_pending_work();
}

void FetchExecutor::run_pending_work()
{
    for (;;) {
        Optional<PendingWork> work;
        {
            MutexLocker locker { m_pending_work_mutex };
            if (m_pending_work.is_empty())
                return;
            work = m_pending_work.dequeue();
        }
        if (m_shut_down.load(AK::MemoryOrder::memory_order_acquire))
            continue;
        auto next = work.release_value();
        next.visit(
            [&](EngineCommand& command) { m_engine->handle_command(move(command)); },
            [](Function<void()>& deferred) { deferred(); });
    }
}

void FetchExecutor::drop_pending_work()
{
    // NB: Without a loop to run on, no command or deferred work that is still pending can ever run. They are destroyed
    //     after the lock is released.
    Vector<PendingWork> pending_work;
    {
        MutexLocker locker { m_pending_work_mutex };
        pending_work.ensure_capacity(m_pending_work.size());
        while (!m_pending_work.is_empty())
            pending_work.unchecked_append(m_pending_work.dequeue());
    }
}

void FetchExecutor::shutdown()
{
    m_shut_down.store(true, AK::MemoryOrder::memory_order_release);
}

}
