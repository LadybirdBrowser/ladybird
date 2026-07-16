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

    virtual void destroy_context(Web::Compositor::CompositorContextId) override;
    virtual void set_parent_context(Web::Compositor::CompositorContextId, Optional<Web::Compositor::CompositorContextId>) override;
    virtual void stop_presenting_to_client(Web::Compositor::CompositorContextId) override;

    virtual void update_display_list(Web::Compositor::CompositorContextId, NonnullRefPtr<Web::Painting::DisplayList>, Web::Painting::AccumulatedVisualContextTree, Web::Painting::DisplayListResourceTransaction&&, Web::Painting::ScrollStateSnapshot&&) override;
    virtual void update_visual_context_tree(Web::Compositor::CompositorContextId, Web::Painting::AccumulatedVisualContextTree) override;
    virtual void update_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId, NonnullRefPtr<Media::VideoFrame const>) override;
    virtual void clear_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId) override;
    virtual void update_scroll_state(Web::Compositor::CompositorContextId, Web::Painting::ScrollStateSnapshot&&) override;
    virtual void invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId, u64 generation) override;
    virtual Web::Compositor::AsyncScrollEnqueueResult async_scroll_by(Web::Compositor::CompositorContextId, Web::UniqueNodeID expected_document_id, Gfx::FloatPoint position,
        Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking) override;
    virtual Web::Compositor::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::Compositor::CompositorContextId) override;
    virtual void viewport_size_updated(Web::Compositor::CompositorContextId, Gfx::IntSize, Web::Compositor::WindowResizingInProgress) override;
    virtual void present_frame(Web::Compositor::CompositorContextId, Gfx::IntRect viewport_rect, Gfx::IntRect damage_rect) override;
    virtual void request_screenshot(Web::Compositor::CompositorContextId, NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback) override;

protected:
    virtual void send_canvas_2d_stream(Web::Painting::Canvas2DCommandStream&) override;

    virtual CompositorConnection* compositor_connection() const = 0;
    virtual void context_was_destroyed(Web::Compositor::CompositorContextId) { }
};

}
