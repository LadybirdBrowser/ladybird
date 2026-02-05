/*
 * Copyright (c) 2025-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
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

    class ThreadData;

public:
    using PresentationCallback = Function<void(Gfx::IntRect const&, i32)>;

    explicit RenderingThread(PresentationCallback);
    ~RenderingThread();

    void start(DisplayListPlayerType);
    void set_skia_player(OwnPtr<Painting::DisplayListPlayerSkia>&& player);

    void update_display_list(NonnullRefPtr<Painting::DisplayList>, Painting::ScrollStateSnapshotByDisplayList&&);
    void update_backing_stores(RefPtr<Gfx::PaintingSurface> front, RefPtr<Gfx::PaintingSurface> back, i32 front_id, i32 back_id);
    void present_frame(Gfx::IntRect);
    void request_screenshot(NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback);

    void ready_to_paint();

private:
    NonnullRefPtr<ThreadData> m_thread_data;
    RefPtr<Threading::Thread> m_thread;
    NonnullRefPtr<Core::Promise<NonnullRefPtr<Core::EventReceiver>>> m_main_thread_exit_promise;
};

}
