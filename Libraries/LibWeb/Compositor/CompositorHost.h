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

class CompositorFrameSink;
class CompositorHost;
class NavigablePresenter;
struct CompositorFrame;
struct SealedFrame;

// A frame's turn to be presented. Only the event loop's presentation queue hands one out, so that the main thread hands
// the Paint thread its frames through the queue alone.
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
    // FIXME: Only the Paint thread should present frames. These still hand it frames the main thread built, or seal
    //        frames for it.
    friend class HTML::LocalNavigable;
    friend class HTML::PresentationQueue;

    CompositorContextHandle(CompositorHost&, Web::CompositorContextId);

    // Brings the context up to date with one frame, whose messages reach the compositor in order.
    void submit_frame(PresentationTurn, CompositorFrame&&);
    // Has the Paint thread build the frame the navigable sealed with its presenter, and present it after the frames
    // handed to it before, and waits for that.
    void present_sealed_frame(PresentationTurn, NavigablePresenter&, SealedFrame&&);
    // Sends the canvas commands a frame may sample ahead of it, and answers whether the compositor can be reached.
    bool ready_for_frame();

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

    RefPtr<WebGL::RemoteWebGLTransport> create_webgl_transport();
    RefPtr<HTML::RemoteCanvas2DTransport> create_canvas_2d_transport();

    Optional<PlaceholderCanvasLink> allocate_placeholder_canvas();
    void release_placeholder_canvas(Compositing::CanvasId);
    void commit_placeholder_canvas(PlaceholderCanvasLink, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize, bool origin_clean);
    PlaceholderCanvasPixels read_placeholder_canvas_pixels(Compositing::CanvasId, Gfx::IntRect);

    void destroy_context(Web::CompositorContextId);
    void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId>);
    void stop_presenting_to_client(Web::CompositorContextId);

    void add_video_sink(Media::VideoSinkHandle);
    void remove_video_sink(Media::VideoSinkHandle);
    void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick);
    void invalidate_wheel_event_listener_state(Web::CompositorContextId, u64 generation);
    void invalidate_keyboard_scroll_state(Web::CompositorContextId, u64 generation);
    Compositing::AsyncScrollEnqueueResult async_scroll_by(Web::CompositorContextId, UniqueNodeID expected_document_id, Gfx::FloatPoint position,
        Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking);
    Compositing::AsyncScrollEnqueueResult smooth_scroll_to(Web::CompositorContextId, Web::AsyncScrollNodeStableID, Gfx::FloatPoint offset_in_device_pixels, Gfx::FloatPoint main_thread_offset_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind, Compositing::SmoothScrollInitiator);
    void cancel_smooth_scroll(Web::CompositorContextId, Web::AsyncScrollNodeStableID);
    Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::CompositorContextId, Compositing::AsyncScrollUpdateFreshness);
    void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize, Compositing::WindowResizingInProgress);
    bool request_rendering_opportunity(Web::CompositorContextId, double maximum_frames_per_second);
    void hurry_rendering_opportunity(Web::CompositorContextId);
    void request_screenshot(Web::CompositorContextId, NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback);

protected:
    CompositorHost();

    // The connection to the compositor process, while there is one.
    virtual CompositorConnection* compositor_connection() const = 0;
    virtual void context_was_destroyed(Web::CompositorContextId) { }

private:
    friend class CompositorContextHandle;

    void submit_frame(PresentationTurn, CompositorFrame&&);

    // Drains the stream, but only when the message can actually be delivered.
    void send_canvas_2d_stream(Compositing::Canvas2DCommandStream&);

    NonnullRefPtr<Compositing::Canvas2DCommandStream> m_canvas_2d_stream;
};

}
