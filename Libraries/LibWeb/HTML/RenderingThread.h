/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Queue.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>

namespace Web::HTML {

class RenderingThread {
    AK_MAKE_NONCOPYABLE(RenderingThread);
    AK_MAKE_NONMOVABLE(RenderingThread);

public:
    RenderingThread();
    ~RenderingThread();

    void start(DisplayListPlayerType);
    void set_skia_player(OwnPtr<Painting::DisplayListPlayerSkia>&& player) { m_skia_player = move(player); }
    void set_skia_backend_context(RefPtr<Gfx::SkiaBackendContext> context) { m_skia_backend_context = move(context); }
    void enqueue_rendering_task(NonnullRefPtr<Painting::DisplayList>, NonnullRefPtr<Painting::BackingStore>, Function<void()>&& callback);
    void clear_bitmap_to_surface_cache();

private:
    void rendering_thread_loop();
    NonnullRefPtr<Gfx::PaintingSurface> painting_surface_for_backing_store(Painting::BackingStore& backing_store);

    Core::EventLoop& m_main_thread_event_loop;
    DisplayListPlayerType m_display_list_player_type;

    OwnPtr<Painting::DisplayListPlayerSkia> m_skia_player;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;

    RefPtr<Threading::Thread> m_thread;
    Atomic<bool> m_exit { false };

    struct Task {
        NonnullRefPtr<Painting::DisplayList> display_list;
        NonnullRefPtr<Painting::BackingStore> backing_store;
        Function<void()> callback;
    };
    // NOTE: Queue will only contain multiple items in case tasks were scheduled by screenshot requests.
    //       Otherwise, it will contain only one item at a time.
    Queue<Task> m_rendering_tasks;
    Threading::Mutex m_rendering_task_mutex;
    Threading::ConditionVariable m_rendering_task_ready_wake_condition { m_rendering_task_mutex };

    HashMap<Gfx::Bitmap*, NonnullRefPtr<Gfx::PaintingSurface>> m_bitmap_to_surface;
    bool m_needs_to_clear_bitmap_to_surface_cache { false };
};

}
