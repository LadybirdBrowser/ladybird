/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>

namespace PaintServer {

class WorkerThread {
    AK_MAKE_NONCOPYABLE(WorkerThread);
    AK_MAKE_NONMOVABLE(WorkerThread);

public:
    explicit WorkerThread(StringView thread_name);
    ~WorkerThread();

    void start();
    bool post_task(Function<void()> task);
    void shutdown(Function<void()> on_shutdown = {});
    bool is_running() const;

private:
    ByteString m_thread_name;
    RefPtr<Threading::Thread> m_thread;
    Threading::Mutex m_state_mutex;
    Threading::ConditionVariable m_state_condition;
    RefPtr<Core::WeakEventLoopReference> m_event_loop;
    bool m_started { false };
};

}
