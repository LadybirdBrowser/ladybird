/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/HTML/RenderingThread.h>

namespace Web::HTML {

RenderingThread::RenderingThread()
    : m_main_thread_event_loop(Core::EventLoop::current())
{
}

RenderingThread::~RenderingThread()
{
    m_exit = true;
    m_rendering_task_ready_wake_condition.signal();
    (void)m_thread->join();
}

void RenderingThread::start()
{
    VERIFY(m_skia_player);
    m_thread = Threading::Thread::construct([this] {
        rendering_thread_loop();
        return static_cast<intptr_t>(0);
    });
    m_thread->start();
}

void RenderingThread::rendering_thread_loop()
{
    while (true) {
        auto task = [this]() -> Optional<Task> {
            Threading::MutexLocker const locker { m_rendering_task_mutex };
            while (m_rendering_tasks.is_empty() && !m_exit) {
                m_rendering_task_ready_wake_condition.wait();
            }
            if (m_exit)
                return {};
            return m_rendering_tasks.dequeue();
        }();

        if (!task.has_value()) {
            VERIFY(m_exit);
            break;
        }

        m_skia_player->execute(*task->display_list, task->painting_surface);
        m_main_thread_event_loop.deferred_invoke([callback = move(task->callback)] {
            callback();
        });
    }
}

void RenderingThread::enqueue_rendering_task(NonnullRefPtr<Painting::DisplayList> display_list, NonnullRefPtr<Gfx::PaintingSurface> painting_surface, Function<void()>&& callback)
{
    Threading::MutexLocker const locker { m_rendering_task_mutex };
    m_rendering_tasks.enqueue(Task { move(display_list), move(painting_surface), move(callback) });
    m_rendering_task_ready_wake_condition.signal();
}

}
