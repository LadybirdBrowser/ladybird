/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HTML/WorkerAgentForward.h>
#include <LibWebCommon/HTML/WorkerTypes.h>

namespace WebWorker {

// Starts a thread that hosts a worker agent, which talks to the UI process over the given transport. Nothing ever waits
// for the thread: it ends once its worker has closed, and otherwise with the process. A process that exits takes running
// workers with it, so it has to leave by Core::Process::terminate_immediately().
ErrorOr<void> start_worker_thread(IPC::TransportHandle, Web::HTML::AgentType);

struct StartedDedicatedWorkerThread {
    NonnullRefPtr<Web::HTML::DedicatedWorkerThreadLink> link;
    IPC::TransportHandle browser_end_of_worker_connection;
};
ErrorOr<StartedDedicatedWorkerThread> start_dedicated_worker_thread(Web::HTML::WorkerAgentOwnerToken);

// Whether the calling thread is one start_worker_thread() started. This can be asked from a signal handler.
bool is_worker_thread();

// Runs the function on each worker thread of this process, from its event loop. The thread may be stuck in script for a
// while before it gets to this.
void post_to_worker_threads(void (*)());

}
