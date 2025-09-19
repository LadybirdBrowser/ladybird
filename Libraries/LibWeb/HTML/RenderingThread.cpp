/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibThreading/Thread.h>
#include <LibWeb/HTML/RenderingThread.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>

namespace Web::HTML {

RenderingThread::RenderingThread()
    : m_main_thread_event_loop(Core::EventLoop::current())
    , m_main_thread_exit_promise(Core::Promise<NonnullRefPtr<Core::EventReceiver>>::construct())
{
    // FIXME: Come up with a better "event loop exited" notification mechanism.
    m_main_thread_exit_promise->on_rejection = [this](Error const&) -> void {
        Threading::MutexLocker const locker { m_rendering_task_mutex };
        m_exit = true;
        m_rendering_task_ready_wake_condition.signal();
    };
    m_main_thread_event_loop.add_job(m_main_thread_exit_promise);
}

RenderingThread::~RenderingThread()
{
    // Note: Promise rejection is expected to signal the thread to exit.
    m_main_thread_exit_promise->reject(Error::from_errno(ECANCELED));
    if (m_thread) {
        (void)m_thread->join();
    }
}

void RenderingThread::start(DisplayListPlayerType display_list_player_type)
{
    m_display_list_player_type = display_list_player_type;
    VERIFY(m_skia_player);
    m_thread = Threading::Thread::construct([this] {
        rendering_thread_loop();
        return static_cast<intptr_t>(0);
    });
    m_thread->start();
}

void RenderingThread::set_skia_player(OwnPtr<Painting::DisplayListPlayerSkia>&& player)
{
    m_skia_player = move(player);
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

        m_skia_player->execute(*task->display_list, move(task->scroll_state_snapshot_by_display_list), task->painting_surface);
        if (m_exit)
            break;
        m_main_thread_event_loop.deferred_invoke([callback = move(task->callback)] {
            callback();
        });
    }
}

void RenderingThread::enqueue_rendering_task(NonnullRefPtr<Painting::DisplayList> display_list, Painting::ScrollStateSnapshotByDisplayList&& scroll_state_snapshot_by_display_list, NonnullRefPtr<Gfx::PaintingSurface> painting_surface, Function<void()>&& callback)
{
    Threading::MutexLocker const locker { m_rendering_task_mutex };
    m_rendering_tasks.enqueue(Task { move(display_list), move(scroll_state_snapshot_by_display_list), move(painting_surface), move(callback) });
    m_rendering_task_ready_wake_condition.signal();
}

}
