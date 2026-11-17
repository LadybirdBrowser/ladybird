/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Queue.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/EngineCommandSink.h>
#include <LibWeb/Fetch/Engine/FetchExecutorServices.h>

namespace Web::Fetch::Engine {

// Commands reach the engine on its thread, in posting order, and never after shutdown.
class WEB_API FetchExecutor : public AtomicRefCounted<FetchExecutor> {
public:
    virtual ~FetchExecutor();

    void post(EngineCommand);

    // Drops every command that has not started running; one that has started completes.
    void shutdown();

protected:
    FetchExecutor(FetchExecutorServices, NonnullOwnPtr<EngineCommandSink>);

    FetchExecutorServices const& services() const { return m_services; }

    // Answers false if the task can never run, as the loop it would run on is gone.
    virtual bool post_task(Function<void()>) = 0;

private:
    void run_pending_commands();
    void drop_pending_commands();

    FetchExecutorServices m_services;
    NonnullOwnPtr<EngineCommandSink> m_engine;
    Mutex m_pending_commands_mutex;
    Queue<EngineCommand> m_pending_commands;
    Atomic<bool> m_shut_down { false };
};

}
