/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Types.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibMedia/Forward.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class PresentationQueue;

}

namespace Web::Compositor {

class CompositorHost;
struct CompositorFrame;

// A frame's turn to be presented. Only the event loop's presentation queue hands one out, to a frame no recording of
// its navigable's containers flies ahead of, so that a frame presented before the one it goes with does not compile.
class PresentationTurn {
    friend class HTML::PresentationQueue;
    PresentationTurn() = default;
};

struct PlaceholderCanvasLink {
    Compositing::CanvasId canvas_id;
    u64 secret { 0 };
};

struct PlaceholderCanvasPixels {
    RefPtr<Gfx::Bitmap> bitmap;
    bool origin_clean { true };
};

class WEB_API CompositorContextHandle {
    AK_MAKE_NONCOPYABLE(CompositorContextHandle);
    AK_MAKE_NONMOVABLE(CompositorContextHandle);

public:
    AK_ALLOC_WITH_KMALLOC;

    ~CompositorContextHandle();

    Web::CompositorContextId id() const { return m_context_id; }
    void set_parent_context(Optional<Web::CompositorContextId>);
    void stop_presenting_to_client();

    // Brings the context up to date with one frame, whose messages reach the compositor in order.
    void submit_frame(PresentationTurn, CompositorFrame&&);
    void add_video_sink(Media::VideoSinkHandle);
    void remove_video_sink(Media::VideoSinkHandle);
    void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick);
    void invalidate_wheel_event_listener_state(u64 generation);
    void invalidate_keyboard_scroll_state(u64 generation);
    Compositing::AsyncScrollEnqueueResult async_scroll_by(UniqueNodeID expected_document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels,
        Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking = Compositing::AsyncScrollOperationTracking::No);
    Compositing::AsyncScrollEnqueueResult smooth_scroll_to(Web::AsyncScrollNodeStableID, Gfx::FloatPoint offset_in_device_pixels, Gfx::FloatPoint main_thread_offset_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind, Compositing::SmoothScrollInitiator);
    void cancel_smooth_scroll(Web::AsyncScrollNodeStableID);
    Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Compositing::AsyncScrollUpdateFreshness);
    void viewport_size_updated(Gfx::IntSize, Compositing::WindowResizingInProgress);
    bool request_rendering_opportunity(double maximum_frames_per_second);
    void hurry_rendering_opportunity();
    void request_screenshot(NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback);

private:
    friend class CompositorHost;

    CompositorContextHandle(CompositorHost&, Web::CompositorContextId);

    CompositorHost& m_host;
    Web::CompositorContextId m_context_id;
};

class WEB_API CompositorHost {
    AK_MAKE_NONCOPYABLE(CompositorHost);
    AK_MAKE_NONMOVABLE(CompositorHost);

public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~CompositorHost();

    OwnPtr<CompositorContextHandle> create_context(Web::CompositorContextId);

    Compositing::Canvas2DCommandStream& canvas_2d_stream() { return *m_canvas_2d_stream; }
    void flush_canvas_2d_stream();
    void discard_canvas_2d_stream();

    virtual RefPtr<WebGL::RemoteWebGLTransport> create_webgl_transport() = 0;
    virtual RefPtr<HTML::RemoteCanvas2DTransport> create_canvas_2d_transport() = 0;

    virtual Optional<PlaceholderCanvasLink> allocate_placeholder_canvas() = 0;
    virtual void release_placeholder_canvas(Compositing::CanvasId) = 0;
    virtual void commit_placeholder_canvas(PlaceholderCanvasLink, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize, bool origin_clean) = 0;
    virtual PlaceholderCanvasPixels read_placeholder_canvas_pixels(Compositing::CanvasId, Gfx::IntRect) = 0;

    virtual void destroy_context(Web::CompositorContextId) = 0;
    virtual void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId>) = 0;
    virtual void stop_presenting_to_client(Web::CompositorContextId) = 0;

    virtual void submit_frame(PresentationTurn, CompositorFrame&&) = 0;
    virtual void add_video_sink(Media::VideoSinkHandle) = 0;
    virtual void remove_video_sink(Media::VideoSinkHandle) = 0;
    virtual void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick) = 0;
    virtual void invalidate_wheel_event_listener_state(Web::CompositorContextId, u64 generation) = 0;
    virtual void invalidate_keyboard_scroll_state(Web::CompositorContextId, u64 generation) = 0;
    virtual Compositing::AsyncScrollEnqueueResult async_scroll_by(Web::CompositorContextId, UniqueNodeID expected_document_id, Gfx::FloatPoint position,
        Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking)
        = 0;
    virtual Compositing::AsyncScrollEnqueueResult smooth_scroll_to(Web::CompositorContextId, Web::AsyncScrollNodeStableID, Gfx::FloatPoint offset_in_device_pixels, Gfx::FloatPoint main_thread_offset_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind, Compositing::SmoothScrollInitiator) = 0;
    virtual void cancel_smooth_scroll(Web::CompositorContextId, Web::AsyncScrollNodeStableID) = 0;
    virtual Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::CompositorContextId, Compositing::AsyncScrollUpdateFreshness) = 0;
    virtual void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize, Compositing::WindowResizingInProgress) = 0;
    virtual bool request_rendering_opportunity(Web::CompositorContextId, double maximum_frames_per_second) = 0;
    virtual void hurry_rendering_opportunity(Web::CompositorContextId) = 0;
    virtual void request_screenshot(Web::CompositorContextId, NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback) = 0;

protected:
    CompositorHost();

    // Drains the stream, but only when the message can actually be delivered.
    virtual void send_canvas_2d_stream(Compositing::Canvas2DCommandStream&) = 0;

private:
    NonnullRefPtr<Compositing::Canvas2DCommandStream> m_canvas_2d_stream;
};

}
