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
}

FetchExecutor::~FetchExecutor() = default;

void FetchExecutor::post(EngineCommand command)
{
    if (m_shut_down.load(AK::MemoryOrder::memory_order_acquire))
        return;

    // NB: A task runs every pending command, oldest first, and not just the one it was posted with. A task can run in a
    //     nested event loop, such as the one a sync XHR spins, while tasks posted before it still wait in the outer
    //     loop. The engine must still see the commands in posting order, and the nested loop must also run the commands
    //     it waits for.
    {
        MutexLocker locker { m_pending_commands_mutex };
        m_pending_commands.enqueue(move(command));
    }

    // The task keeps the executor, and so the engine, alive until it has run or been dropped.
    auto run_pending_commands = [self = NonnullRefPtr(*this)] { self->run_pending_commands(); };
    if (!post_task(move(run_pending_commands)))
        drop_pending_commands();
}

void FetchExecutor::run_pending_commands()
{
    for (;;) {
        Optional<EngineCommand> command;
        {
            MutexLocker locker { m_pending_commands_mutex };
            if (m_pending_commands.is_empty())
                return;
            command = m_pending_commands.dequeue();
        }
        if (m_shut_down.load(AK::MemoryOrder::memory_order_acquire))
            continue;
        m_engine->handle_command(command.release_value());
    }
}

void FetchExecutor::drop_pending_commands()
{
    // NB: Without a loop to run on, no command that is still pending can ever run. They are destroyed after the lock is
    //     released.
    Vector<EngineCommand> pending_commands;
    {
        MutexLocker locker { m_pending_commands_mutex };
        pending_commands.ensure_capacity(m_pending_commands.size());
        while (!m_pending_commands.is_empty())
            pending_commands.unchecked_append(m_pending_commands.dequeue());
    }
}

void FetchExecutor::shutdown()
{
    m_shut_down.store(true, AK::MemoryOrder::memory_order_release);
}

}
