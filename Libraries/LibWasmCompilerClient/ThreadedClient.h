/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Forward.h>
#include <LibIPC/Forward.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Forward.h>

namespace WasmCompilerClient {

class Client;

class ThreadedClient final : public AtomicRefCounted<ThreadedClient> {
    AK_MAKE_NONCOPYABLE(ThreadedClient);
    AK_MAKE_NONMOVABLE(ThreadedClient);

public:
    static ErrorOr<NonnullRefPtr<ThreadedClient>> create(NonnullOwnPtr<IPC::Transport>);
    ~ThreadedClient();

    Core::AnonymousBuffer compile(Core::AnonymousBuffer const&);

private:
    explicit ThreadedClient(NonnullOwnPtr<IPC::Transport>);

    intptr_t thread_main(NonnullOwnPtr<IPC::Transport>);

    NonnullRefPtr<Threading::Thread> m_thread;

    Sync::Mutex m_mutex;
    Sync::ConditionVariable m_initialization_condition { m_mutex };
    Sync::ConditionVariable m_client_unused_condition { m_mutex };

    bool m_initialized { false };
    Optional<Error> m_initialization_error;

    RefPtr<Core::WeakEventLoopReference> m_event_loop;
    Client* m_client { nullptr };

    size_t m_active_compilations { 0 };
};

}
