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

    // The task keeps the executor, and so the engine, alive until it has run or been dropped.
    auto run_command = [self = NonnullRefPtr(*this), command = move(command)]() mutable {
        if (self->m_shut_down.load(AK::MemoryOrder::memory_order_acquire))
            return;
        self->m_engine->handle_command(move(command));
    };
    post_task(move(run_command));
}

void FetchExecutor::shutdown()
{
    m_shut_down.store(true, AK::MemoryOrder::memory_order_release);
}

}
