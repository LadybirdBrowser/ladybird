/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ConditionVariable.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/Queue.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibThreading/Thread.h>

namespace Threading {

class ThreadPool {
public:
    AK_ALLOC_WITH_KMALLOC;

    static ThreadPool& the();

    void submit(Function<void()>);

private:
    ThreadPool();

    intptr_t worker_thread_func();

    Mutex m_mutex;
    ConditionVariable m_condition { m_mutex };
    Queue<Function<void()>> m_work_queue;
    Vector<NonnullRefPtr<Thread>> m_threads;
};

}
