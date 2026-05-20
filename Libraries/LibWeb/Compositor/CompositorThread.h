/*
 * Copyright (c) 2025-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>
#include <LibMedia/Forward.h>
#include <LibSync/ConditionVariable.h>
#include <LibThreading/Forward.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Page/Page.h>

namespace Web::Painting {

struct DisplayListResourceTransaction;

}

namespace Web::Compositor {

class CompositorMainThreadClient;
class CompositorUIPresentationClient;

class WEB_API CompositorThread {
    AK_MAKE_NONCOPYABLE(CompositorThread);
    AK_MAKE_NONMOVABLE(CompositorThread);

public:
    class ThreadData;

    explicit CompositorThread(NonnullRefPtr<CompositorMainThreadClient>);
    ~CompositorThread();

    void set_ui_presentation_client(NonnullRefPtr<CompositorUIPresentationClient>);
    void clear_ui_presentation_client();
    void presented_bitmap_ready_to_paint(u64 page_id, i32 bitmap_id);
    bool async_scroll_by(u64 page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels);
    bool handle_mouse_event(u64 page_id, MouseEvent const&);

    CompositorContextId create_context(Optional<u64> page_id, PagePresentationRegistration);
    void destroy_context(CompositorContextId);
    void stop_presenting_to_client(CompositorContextId);
    void set_presentation_mode(CompositorContextId, PresentationMode);

    void update_display_list(CompositorContextId, NonnullRefPtr<Painting::DisplayList>, Painting::DisplayListResourceTransaction&&, Painting::ScrollStateSnapshot&&);
    void update_video_frame(CompositorContextId, Painting::VideoFrameResourceId, NonnullRefPtr<Media::VideoFrame const>);
    void clear_video_frame(CompositorContextId, Painting::VideoFrameResourceId);
    void update_compositor_surface(CompositorContextId, Painting::CompositorSurfaceId, Gfx::SharedImage&&);
    void clear_compositor_surface(CompositorContextId, Painting::CompositorSurfaceId);
    void update_scroll_state(CompositorContextId, Painting::ScrollStateSnapshot&&);
    void invalidate_wheel_event_listener_state(CompositorContextId, u64 generation);
    AsyncScrollEnqueueResult async_scroll_by(CompositorContextId, UniqueNodeID expected_document_id, Gfx::FloatPoint position,
        Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, AsyncScrollOperationTracking = AsyncScrollOperationTracking::No);
    bool should_defer_async_scroll_offset_adoption(CompositorContextId) const;
    bool should_defer_main_thread_present_for_async_scroll(CompositorContextId) const;
    PendingAsyncScrollUpdates take_pending_async_scroll_updates(CompositorContextId);
    void viewport_size_updated(CompositorContextId, Gfx::IntSize, bool is_top_level_traversable, WindowResizingInProgress);
    void present_frame(CompositorContextId, Gfx::IntRect);
    void request_screenshot(CompositorContextId, NonnullRefPtr<Gfx::PaintingSurface>, ScreenshotRequestId);
    void start(DisplayListPlayerType);

private:
    NonnullRefPtr<ThreadData> m_thread_data;
    RefPtr<Threading::Thread> m_thread;
};

}
