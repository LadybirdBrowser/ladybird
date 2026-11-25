/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Queue.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/EngineCommandSink.h>
#include <LibWeb/Fetch/Engine/FetchExecutorServices.h>

namespace Web::Fetch::Engine {

class FetchEngineRecords;

// Commands reach the engine on its thread, in posting order, and never after shutdown.
class WEB_API FetchExecutor : public AtomicRefCounted<FetchExecutor> {
public:
    virtual ~FetchExecutor();

    void post(EngineCommand);

    // Runs work of the engine's own on the engine's thread, after the commands posted before it, unless the executor
    // shuts down first.
    void defer(Badge<FetchEngineRecords>, Function<void()>);

    // Drops every command and deferred work that has not started running; one that has started completes.
    void shutdown();

protected:
    FetchExecutor(FetchExecutorServices, NonnullOwnPtr<EngineCommandSink>);

    FetchExecutorServices const& services() const { return m_services; }

    // Answers false if the task can never run, as the loop it would run on is gone.
    virtual bool post_task(Function<void()>) = 0;

private:
    using PendingWork = Variant<EngineCommand, Function<void()>>;

    void enqueue(PendingWork);
    void run_pending_work();
    void drop_pending_work();

    FetchExecutorServices m_services;
    NonnullOwnPtr<EngineCommandSink> m_engine;
    Mutex m_pending_work_mutex;
    Queue<PendingWork> m_pending_work;
    u64 m_enqueued_count { 0 };
    u64 m_dequeued_count { 0 };
    Atomic<bool> m_shut_down { false };
};

}
