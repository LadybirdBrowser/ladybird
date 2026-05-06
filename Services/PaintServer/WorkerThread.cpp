/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibPaintServer/Debug.h>
#include <PaintServer/WorkerThread.h>

namespace PaintServer {

WorkerThread::WorkerThread(StringView thread_name)
    : m_thread_name(thread_name)
    , m_state_condition(m_state_mutex)
{
}

WorkerThread::~WorkerThread()
{
    shutdown();
}

void WorkerThread::start()
{
    VERIFY(!m_thread);

    m_started = false;
    m_thread = Threading::Thread::construct(m_thread_name, [this] -> intptr_t {
        Core::EventLoop event_loop;
        {
            Threading::MutexLocker locker(m_state_mutex);
            m_event_loop = Core::EventLoop::current_weak();
            m_started = true;
            m_state_condition.broadcast();
        }

        int exit_code = event_loop.exec();

        if (is_logging_enabled())
            dbgln("WorkerThread: {} event loop exiting exit_code={}", m_thread_name, exit_code);

        {
            Threading::MutexLocker locker(m_state_mutex);
            m_event_loop = nullptr;
        }

        return exit_code;
    });
    m_thread->start();

    Threading::MutexLocker locker(m_state_mutex);
    m_state_condition.wait_while([this] { return !m_started; });

    if (is_logging_enabled())
        dbgln("WorkerThread: started {}", m_thread_name);
}

bool WorkerThread::post_task(Function<void()> task)
{
    RefPtr<Core::WeakEventLoopReference> event_loop;
    {
        Threading::MutexLocker locker(m_state_mutex);
        event_loop = m_event_loop;

        if (!event_loop) {
            if (is_logging_enabled())
                dbgln("WorkerThread: post_task failed {} has no event loop", m_thread_name);
            return false;
        }
    }

    auto strong_event_loop = event_loop->take();
    if (!strong_event_loop) {
        if (is_logging_enabled())
            dbgln("WorkerThread: post_task failed {} event loop is gone", m_thread_name);
        return false;
    }

    strong_event_loop->deferred_invoke(move(task));
    return true;
}

void WorkerThread::shutdown(Function<void()> on_shutdown)
{
    if (!m_thread)
        return;

    if (is_logging_enabled())
        dbgln("WorkerThread: shutting down {}", m_thread_name);

    auto shutdown_was_posted = post_task([on_shutdown = move(on_shutdown)]() mutable {
        if (on_shutdown)
            on_shutdown();
        Core::EventLoop::current().quit(0);
    });
    if (!shutdown_was_posted && on_shutdown)
        on_shutdown();

    MUST(m_thread->join());

    if (is_logging_enabled())
        dbgln("WorkerThread: shutdown complete {}", m_thread_name);

    m_thread = nullptr;

    Threading::MutexLocker locker(m_state_mutex);
    m_event_loop = nullptr;
    m_started = false;
}

bool WorkerThread::is_running() const
{
    return m_thread;
}

}
