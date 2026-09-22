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
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibMedia/Forward.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Compositor {

class CompositorHost;

class WEB_API CompositorContextHandle {
    AK_MAKE_NONCOPYABLE(CompositorContextHandle);
    AK_MAKE_NONMOVABLE(CompositorContextHandle);

public:
    AK_ALLOC_WITH_KMALLOC;

    ~CompositorContextHandle();

    Compositing::CompositorContextId id() const { return m_context_id; }
    void set_parent_context(Optional<Compositing::CompositorContextId>);
    void stop_presenting_to_client();

    void update_display_list(NonnullRefPtr<Compositing::DisplayList>, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&, Compositing::ScrollStateSnapshot&&);
    void update_visual_context_tree(Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&);
    void add_video_sink(Media::VideoSinkHandle);
    void remove_video_sink(Media::VideoSinkHandle);
    void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick);
    void update_scroll_state(Compositing::ScrollStateSnapshot&&, Compositing::KeyboardScrollState);
    void invalidate_wheel_event_listener_state(u64 generation);
    void invalidate_keyboard_scroll_state(u64 generation);
    Compositing::AsyncScrollEnqueueResult async_scroll_by(UniqueNodeID expected_document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels,
        Gfx::IntRect viewport_rect, Compositing::WheelDeltaPrecision, Compositing::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking = Compositing::AsyncScrollOperationTracking::No);
    Compositing::AsyncScrollEnqueueResult smooth_scroll_to(Compositing::AsyncScrollNodeStableID, Gfx::FloatPoint offset_in_device_pixels, Gfx::FloatPoint main_thread_offset_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind);
    void cancel_smooth_scroll(Compositing::AsyncScrollNodeStableID);
    Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Compositing::AsyncScrollUpdateFreshness);
    void viewport_size_updated(Gfx::IntSize, Compositing::WindowResizingInProgress);
    bool request_rendering_opportunity(double maximum_frames_per_second);
    void hurry_rendering_opportunity();
    void present_frame(Gfx::IntRect viewport_rect);
    void request_screenshot(NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback);

private:
    friend class CompositorHost;

    CompositorContextHandle(CompositorHost&, Compositing::CompositorContextId);

    CompositorHost& m_host;
    Compositing::CompositorContextId m_context_id;
};

class WEB_API CompositorHost {
    AK_MAKE_NONCOPYABLE(CompositorHost);
    AK_MAKE_NONMOVABLE(CompositorHost);

public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~CompositorHost();

    OwnPtr<CompositorContextHandle> create_context(Compositing::CompositorContextId);

    Compositing::Canvas2DCommandStream& canvas_2d_stream() { return *m_canvas_2d_stream; }
    void flush_canvas_2d_stream();
    void discard_canvas_2d_stream();

    virtual RefPtr<WebGL::RemoteWebGLTransport> create_webgl_transport() = 0;
    virtual RefPtr<HTML::RemoteCanvas2DTransport> create_canvas_2d_transport() = 0;

    virtual void destroy_context(Compositing::CompositorContextId) = 0;
    virtual void set_parent_context(Compositing::CompositorContextId, Optional<Compositing::CompositorContextId>) = 0;
    virtual void stop_presenting_to_client(Compositing::CompositorContextId) = 0;

    virtual void update_display_list(Compositing::CompositorContextId, NonnullRefPtr<Compositing::DisplayList>, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&, Compositing::ScrollStateSnapshot&&) = 0;
    virtual void update_visual_context_tree(Compositing::CompositorContextId, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&) = 0;
    virtual void add_video_sink(Media::VideoSinkHandle) = 0;
    virtual void remove_video_sink(Media::VideoSinkHandle) = 0;
    virtual void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick) = 0;
    virtual void update_scroll_state(Compositing::CompositorContextId, Compositing::ScrollStateSnapshot&&, Compositing::KeyboardScrollState) = 0;
    virtual void invalidate_wheel_event_listener_state(Compositing::CompositorContextId, u64 generation) = 0;
    virtual void invalidate_keyboard_scroll_state(Compositing::CompositorContextId, u64 generation) = 0;
    virtual Compositing::AsyncScrollEnqueueResult async_scroll_by(Compositing::CompositorContextId, UniqueNodeID expected_document_id, Gfx::FloatPoint position,
        Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::WheelDeltaPrecision, Compositing::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking)
        = 0;
    virtual Compositing::AsyncScrollEnqueueResult smooth_scroll_to(Compositing::CompositorContextId, Compositing::AsyncScrollNodeStableID, Gfx::FloatPoint offset_in_device_pixels, Gfx::FloatPoint main_thread_offset_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind) = 0;
    virtual void cancel_smooth_scroll(Compositing::CompositorContextId, Compositing::AsyncScrollNodeStableID) = 0;
    virtual Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Compositing::CompositorContextId, Compositing::AsyncScrollUpdateFreshness) = 0;
    virtual void viewport_size_updated(Compositing::CompositorContextId, Gfx::IntSize, Compositing::WindowResizingInProgress) = 0;
    virtual bool request_rendering_opportunity(Compositing::CompositorContextId, double maximum_frames_per_second) = 0;
    virtual void hurry_rendering_opportunity(Compositing::CompositorContextId) = 0;
    virtual void present_frame(Compositing::CompositorContextId, Gfx::IntRect viewport_rect) = 0;
    virtual void request_screenshot(Compositing::CompositorContextId, NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback) = 0;

protected:
    CompositorHost();

    // Drains the stream, but only when the message can actually be delivered.
    virtual void send_canvas_2d_stream(Compositing::Canvas2DCommandStream&) = 0;

private:
    NonnullRefPtr<Compositing::Canvas2DCommandStream> m_canvas_2d_stream;
};

}
