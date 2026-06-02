/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/Types.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>
#include <LibMedia/Forward.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>

namespace Web::Compositor {

class CompositorHost;

class WEB_API CompositorContextHandle {
    AK_MAKE_NONCOPYABLE(CompositorContextHandle);
    AK_MAKE_NONMOVABLE(CompositorContextHandle);

public:
    ~CompositorContextHandle();

    CompositorContextId id() const { return m_context_id; }
    void set_presentation_mode(PresentationMode);

    void update_display_list(NonnullRefPtr<Painting::DisplayList>, Painting::AccumulatedVisualContextTree, Painting::DisplayListResourceTransaction&&, Painting::ScrollStateSnapshot&&);
    void update_visual_context_tree(Painting::AccumulatedVisualContextTree);
    void update_video_frame(Painting::VideoFrameResourceId, NonnullRefPtr<Media::VideoFrame const>);
    void clear_video_frame(Painting::VideoFrameResourceId);
    void update_compositor_surface(Painting::CompositorSurfaceId, Gfx::SharedImage&&);
    void clear_compositor_surface(Painting::CompositorSurfaceId);
    void update_scroll_state(Painting::ScrollStateSnapshot&&);
    void invalidate_wheel_event_listener_state(u64 generation);
    AsyncScrollEnqueueResult async_scroll_by(UniqueNodeID expected_document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels,
        Gfx::IntRect viewport_rect, AsyncScrollOperationTracking = AsyncScrollOperationTracking::No);
    bool should_defer_main_thread_present_for_async_scroll() const;
    PendingAsyncScrollUpdates take_pending_async_scroll_updates();
    void viewport_size_updated(Gfx::IntSize, WindowResizingInProgress);
    void present_frame(Gfx::IntRect);
    void request_screenshot(NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback);

private:
    friend class CompositorHost;

    CompositorContextHandle(CompositorHost&, CompositorContextId);

    CompositorHost& m_host;
    CompositorContextId m_context_id;
};

class WEB_API CompositorHost {
    AK_MAKE_NONCOPYABLE(CompositorHost);
    AK_MAKE_NONMOVABLE(CompositorHost);

public:
    virtual ~CompositorHost();

    OwnPtr<CompositorContextHandle> create_context(CompositorContextId);

    virtual void destroy_context(CompositorContextId) = 0;
    virtual void set_presentation_mode(CompositorContextId, PresentationMode) = 0;

    virtual void update_display_list(CompositorContextId, NonnullRefPtr<Painting::DisplayList>, Painting::AccumulatedVisualContextTree, Painting::DisplayListResourceTransaction&&, Painting::ScrollStateSnapshot&&) = 0;
    virtual void update_visual_context_tree(CompositorContextId, Painting::AccumulatedVisualContextTree) = 0;
    virtual void update_video_frame(CompositorContextId, Painting::VideoFrameResourceId, NonnullRefPtr<Media::VideoFrame const>) = 0;
    virtual void clear_video_frame(CompositorContextId, Painting::VideoFrameResourceId) = 0;
    virtual void update_compositor_surface(CompositorContextId, Painting::CompositorSurfaceId, Gfx::SharedImage&&) = 0;
    virtual void clear_compositor_surface(CompositorContextId, Painting::CompositorSurfaceId) = 0;
    virtual void update_scroll_state(CompositorContextId, Painting::ScrollStateSnapshot&&) = 0;
    virtual void invalidate_wheel_event_listener_state(CompositorContextId, u64 generation) = 0;
    virtual AsyncScrollEnqueueResult async_scroll_by(CompositorContextId, UniqueNodeID expected_document_id, Gfx::FloatPoint position,
        Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, AsyncScrollOperationTracking)
        = 0;
    virtual bool should_defer_main_thread_present_for_async_scroll(CompositorContextId) const = 0;
    virtual PendingAsyncScrollUpdates take_pending_async_scroll_updates(CompositorContextId) = 0;
    virtual void viewport_size_updated(CompositorContextId, Gfx::IntSize, WindowResizingInProgress) = 0;
    virtual void present_frame(CompositorContextId, Gfx::IntRect) = 0;
    virtual void request_screenshot(CompositorContextId, NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback) = 0;

protected:
    CompositorHost() = default;
};

}
