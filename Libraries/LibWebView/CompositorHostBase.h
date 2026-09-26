/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/RefPtr.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API CompositorHostBase : public Web::Compositor::CompositorHost {
public:
    virtual RefPtr<Web::WebGL::RemoteWebGLTransport> create_webgl_transport() override;
    virtual RefPtr<Web::HTML::RemoteCanvas2DTransport> create_canvas_2d_transport() override;

    virtual void destroy_context(Compositing::CompositorContextId) override;
    virtual void set_parent_context(Compositing::CompositorContextId, Optional<Compositing::CompositorContextId>) override;
    virtual void stop_presenting_to_client(Compositing::CompositorContextId) override;

    virtual void update_display_list(Compositing::CompositorContextId, NonnullRefPtr<Compositing::DisplayList>, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&, Compositing::ScrollStateSnapshot&&) override;
    virtual void update_visual_context_tree(Compositing::CompositorContextId, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&) override;
    virtual void add_video_sink(Media::VideoSinkHandle) override;
    virtual void remove_video_sink(Media::VideoSinkHandle) override;
    virtual void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick) override;
    virtual void update_scroll_state(Compositing::CompositorContextId, Compositing::ScrollStateSnapshot&&, Compositing::KeyboardScrollState) override;
    virtual void invalidate_wheel_event_listener_state(Compositing::CompositorContextId, u64 generation) override;
    virtual void invalidate_keyboard_scroll_state(Compositing::CompositorContextId, u64 generation) override;
    virtual Compositing::AsyncScrollEnqueueResult async_scroll_by(Compositing::CompositorContextId, Compositing::UniqueNodeID expected_document_id, Gfx::FloatPoint position,
        Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::WheelDeltaPrecision, Compositing::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking) override;
    virtual Compositing::AsyncScrollEnqueueResult smooth_scroll_to(Compositing::CompositorContextId, Compositing::AsyncScrollNodeStableID, Gfx::FloatPoint offset_in_device_pixels, Gfx::FloatPoint main_thread_offset_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind, Compositing::SmoothScrollInitiator) override;
    virtual void cancel_smooth_scroll(Compositing::CompositorContextId, Compositing::AsyncScrollNodeStableID) override;
    virtual Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Compositing::CompositorContextId, Compositing::AsyncScrollUpdateFreshness) override;
    virtual void viewport_size_updated(Compositing::CompositorContextId, Gfx::IntSize, Compositing::WindowResizingInProgress) override;
    virtual bool request_rendering_opportunity(Compositing::CompositorContextId, double maximum_frames_per_second) override;
    virtual void hurry_rendering_opportunity(Compositing::CompositorContextId) override;
    virtual void present_frame(Compositing::CompositorContextId, Gfx::IntRect viewport_rect) override;
    virtual void request_screenshot(Compositing::CompositorContextId, NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback) override;

protected:
    virtual void send_canvas_2d_stream(Compositing::Canvas2DCommandStream&) override;

    virtual CompositorConnection* compositor_connection() const = 0;
    virtual void context_was_destroyed(Compositing::CompositorContextId) { }
};

}
