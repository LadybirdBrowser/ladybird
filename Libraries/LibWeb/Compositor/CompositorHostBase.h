/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/RefPtr.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/Export.h>

namespace Web::Compositor {

class WEB_API CompositorHostBase : public CompositorHost {
public:
    virtual RefPtr<Web::WebGL::RemoteWebGLTransport> create_webgl_transport() override;
    virtual RefPtr<Web::HTML::RemoteCanvas2DTransport> create_canvas_2d_transport() override;

    virtual Optional<Web::Compositor::PlaceholderCanvasLink> allocate_placeholder_canvas() override;
    virtual void release_placeholder_canvas(Compositing::CanvasId) override;
    virtual void commit_placeholder_canvas(Web::Compositor::PlaceholderCanvasLink, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize, bool origin_clean) override;
    virtual Web::Compositor::PlaceholderCanvasPixels read_placeholder_canvas_pixels(Compositing::CanvasId, Gfx::IntRect) override;

    virtual void destroy_context(Web::CompositorContextId) override;
    virtual void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId>) override;
    virtual void stop_presenting_to_client(Web::CompositorContextId) override;

    virtual void submit_frame(PresentationTurn, CompositorFrame&&) override;
    virtual void add_video_sink(Media::VideoSinkHandle) override;
    virtual void remove_video_sink(Media::VideoSinkHandle) override;
    virtual void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick) override;
    virtual void invalidate_wheel_event_listener_state(Web::CompositorContextId, u64 generation) override;
    virtual void invalidate_keyboard_scroll_state(Web::CompositorContextId, u64 generation) override;
    virtual Compositing::AsyncScrollEnqueueResult async_scroll_by(Web::CompositorContextId, Web::UniqueNodeID expected_document_id, Gfx::FloatPoint position,
        Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking) override;
    virtual Compositing::AsyncScrollEnqueueResult smooth_scroll_to(Web::CompositorContextId, Web::AsyncScrollNodeStableID, Gfx::FloatPoint offset_in_device_pixels, Gfx::FloatPoint main_thread_offset_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind, Compositing::SmoothScrollInitiator) override;
    virtual void cancel_smooth_scroll(Web::CompositorContextId, Web::AsyncScrollNodeStableID) override;
    virtual Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::CompositorContextId, Compositing::AsyncScrollUpdateFreshness) override;
    virtual void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize, Compositing::WindowResizingInProgress) override;
    virtual bool request_rendering_opportunity(Web::CompositorContextId, double maximum_frames_per_second) override;
    virtual void hurry_rendering_opportunity(Web::CompositorContextId) override;
    virtual void request_screenshot(Web::CompositorContextId, NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback) override;

protected:
    virtual void send_canvas_2d_stream(Compositing::Canvas2DCommandStream&) override;

    virtual CompositorConnection* compositor_connection() const = 0;
    virtual void context_was_destroyed(Web::CompositorContextId) { }
};

}
