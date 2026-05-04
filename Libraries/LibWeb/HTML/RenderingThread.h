/*
 * Copyright (c) 2025-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Queue.h>
#include <AK/Variant.h>
#include <LibGfx/SharedImage.h>
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
    struct PresentToUI {
    };
    struct PublishToExternalContent {
        NonnullRefPtr<Painting::ExternalContentSource> source;
    };
    using PresentationMode = Variant<PresentToUI, PublishToExternalContent>;

    explicit RenderingThread(PresentationCallback);
    ~RenderingThread();

    void start(DisplayListPlayerType);
    void set_skia_player(OwnPtr<Painting::DisplayListPlayerSkia>&& player);
    void set_presentation_mode(PresentationMode);

    void update_display_list(NonnullRefPtr<Painting::DisplayList>, Painting::ScrollStateSnapshot&&);
    void update_backing_stores(Gfx::IntSize, i32 front_id, i32 back_id, Function<void(i32, Gfx::SharedImage, i32, Gfx::SharedImage)>&& = {});
    u64 present_frame(Gfx::IntRect);
    void wait_for_frame(u64 frame_id);
    void request_screenshot(NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback);

    void ready_to_paint();

private:
    NonnullRefPtr<ThreadData> m_thread_data;
    RefPtr<Threading::Thread> m_thread;
};

}
