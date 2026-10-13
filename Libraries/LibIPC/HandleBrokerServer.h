/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ConditionVariable.h>
#include <AK/Mutex.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibCore/Forward.h>
#include <LibIPC/Forward.h>
#include <LibIPC/HandleBroker.h>
#include <LibThreading/Forward.h>

namespace IPC {

class HandleBrokerChildConnection;

// Serves this process's broker to the processes it spawned, each over a channel of its own. The channels are answered
// on a thread that waits on nothing else, so no process is kept waiting for a handle while this process is busy, or
// while this process is itself waiting on that one.
class HandleBrokerServer {
    AK_MAKE_NONCOPYABLE(HandleBrokerServer);
    AK_MAKE_NONMOVABLE(HandleBrokerServer);

public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullOwnPtr<HandleBrokerServer> create();
    ~HandleBrokerServer();

    // The caller keeps the child's process alive until this returns, so its process ID cannot name another by then.
    void add_child(pid_t, int channel_handle);

private:
    friend class HandleBrokerChildConnection;

    explicit HandleBrokerServer(LocalHandleBroker&);
    intptr_t thread_main();
    void remove_child(HandleBrokerChildConnection&);

    LocalHandleBroker& m_broker;
    Mutex m_mutex;
    ConditionVariable m_condition { m_mutex };
    RefPtr<Core::WeakEventLoopReference> m_event_loop;
    NonnullRefPtr<Threading::Thread> m_thread;

    // Only the broker thread touches this.
    Vector<NonnullOwnPtr<HandleBrokerChildConnection>> m_children;
};

}
