/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Queue.h>
#include <AK/Vector.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Thread.h>

namespace Threading {

class ThreadPool {
public:
    static ThreadPool& the();

    void submit(Function<void()>);

private:
    ThreadPool();

    intptr_t worker_thread_func();

    Sync::Mutex m_mutex;
    Sync::ConditionVariable m_condition { m_mutex };
    Queue<Function<void()>> m_work_queue;
    Vector<NonnullRefPtr<Thread>> m_threads;
};

}
