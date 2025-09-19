/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Queue.h>
#include <LibCore/Promise.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Forward.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

class RenderingThread {
    AK_MAKE_NONCOPYABLE(RenderingThread);
    AK_MAKE_NONMOVABLE(RenderingThread);

public:
    RenderingThread();
    ~RenderingThread();

    void start(DisplayListPlayerType);
    void set_skia_player(OwnPtr<Painting::DisplayListPlayerSkia>&& player);
    void enqueue_rendering_task(NonnullRefPtr<Painting::DisplayList>, Painting::ScrollStateSnapshotByDisplayList&&, NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback);

private:
    void rendering_thread_loop();

    Core::EventLoop& m_main_thread_event_loop;
    DisplayListPlayerType m_display_list_player_type;

    OwnPtr<Painting::DisplayListPlayerSkia> m_skia_player;

    RefPtr<Threading::Thread> m_thread;
    Atomic<bool> m_exit { false };
    NonnullRefPtr<Core::Promise<NonnullRefPtr<Core::EventReceiver>>> m_main_thread_exit_promise;

    struct Task {
        NonnullRefPtr<Painting::DisplayList> display_list;
        Painting::ScrollStateSnapshotByDisplayList scroll_state_snapshot_by_display_list;
        NonnullRefPtr<Gfx::PaintingSurface> painting_surface;
        Function<void()> callback;
    };
    // NOTE: Queue will only contain multiple items in case tasks were scheduled by screenshot requests.
    //       Otherwise, it will contain only one item at a time.
    Queue<Task> m_rendering_tasks;
    Threading::Mutex m_rendering_task_mutex;
    Threading::ConditionVariable m_rendering_task_ready_wake_condition { m_rendering_task_mutex };
};

}
