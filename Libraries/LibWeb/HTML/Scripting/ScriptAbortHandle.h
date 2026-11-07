/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/Forward.h>
#include <LibIPC/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

// What any thread can stop the script of an agent with, whatever that script is doing, including waiting in a nested
// event loop. It may outlive the agent, which it then has no effect on anymore.
class WEB_API ScriptAbortHandle final : public AtomicRefCounted<ScriptAbortHandle> {
public:
    // Only the agent's thread may create a handle, as it has to know the agent's Core::EventLoop.
    static NonnullRefPtr<ScriptAbortHandle> create(JS::VM&);

    ~ScriptAbortHandle();

    // Stops script on the agent's VM for good (see JS::VM::request_interrupt()), and fails the sync requests of its
    // thread, as script that waits for one is not running. Any thread may call this.
    void abort_script();

    // The agent's thread calls this before the VM goes away.
    void detach();

private:
    ScriptAbortHandle(JS::VM&, NonnullRefPtr<Core::WeakEventLoopReference>, NonnullRefPtr<IPC::SyncRequestCanceller>);

    Mutex m_mutex;
    JS::VM* m_vm { nullptr };
    NonnullRefPtr<Core::WeakEventLoopReference> m_event_loop;
    NonnullRefPtr<IPC::SyncRequestCanceller> m_sync_request_canceller;
};

}
