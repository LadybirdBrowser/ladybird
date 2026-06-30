/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <Compositor/CompositorWebContentClientEndpoint.h>
#include <Compositor/CompositorWebContentServerEndpoint.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibMedia/Forward.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/WebGL/Types.h>

namespace WebContent {

class CompositorConnection final
    : public IPC::ConnectionToServer<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>
    , public CompositorWebContentClientEndpoint {
    C_OBJECT_ABSTRACT(CompositorConnection)

public:
    explicit CompositorConnection(NonnullOwnPtr<IPC::Transport>);

    void set_parent_context(Web::Compositor::CompositorContextId, Optional<Web::Compositor::CompositorContextId>);
    void stop_presenting_to_client(Web::Compositor::CompositorContextId);
    void destroy_context(Web::Compositor::CompositorContextId);
    void update_display_list(Web::Compositor::CompositorContextId, NonnullRefPtr<Web::Painting::DisplayList> const&, Web::Painting::AccumulatedVisualContextTree const&, Web::Painting::DisplayListResourceTransaction, Web::Painting::ScrollStateSnapshot const&);
    void update_visual_context_tree(Web::Compositor::CompositorContextId, Web::Painting::AccumulatedVisualContextTree const&);
    void update_scroll_state(Web::Compositor::CompositorContextId, Web::Painting::ScrollStateSnapshot const&);
    void update_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId, NonnullRefPtr<Media::VideoFrame const> const&);
    void clear_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId);
    Optional<Web::Painting::CanvasId> create_canvas_2d_context(Gfx::IntSize, bool alpha);
    void update_canvas_2d_commands(Web::Painting::CanvasId, Gfx::CanvasCommandList const&, bool commit);
    void destroy_canvas_context(Web::Painting::CanvasId);
    Gfx::ShareableBitmap get_canvas_pixels(Web::Painting::CanvasId, Gfx::IntRect);
    void invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId, u64 generation);
    Web::Compositor::AsyncScrollEnqueueResult async_scroll_by(Web::Compositor::CompositorContextId, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking);
    bool should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId);
    Web::Compositor::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::Compositor::CompositorContextId);
    void viewport_size_updated(Web::Compositor::CompositorContextId, Gfx::IntSize, Web::Compositor::WindowResizingInProgress);
    void present_frame(Web::Compositor::CompositorContextId, Gfx::IntRect);
    void request_screenshot(Web::Compositor::CompositorContextId, NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&&);

    Optional<Web::Painting::CanvasId> create_webgl_context(Web::WebGL::WebGLVersion, Gfx::IntSize, bool depth, bool stencil, bool antialias, Vector<String>& out_supported_extensions);
    void send_webgl_commands(Web::Painting::CanvasId, ByteBuffer const&, Vector<Gfx::DecodedImageFrame> const& bitmaps);
    void present_webgl_canvas(Web::Painting::CanvasId, bool preserve_drawing_buffer);
    ByteBuffer webgl_sync_call(Web::Painting::CanvasId, ByteBuffer request);
    Web::WebGL::ReadPixelsResult read_webgl_pixels(Web::Painting::CanvasId, Web::WebGL::GLint x, Web::WebGL::GLint y, Web::WebGL::GLsizei width, Web::WebGL::GLsizei height, Web::WebGL::GLenum format, Web::WebGL::GLenum type, Web::WebGL::GLsizei buf_size, Core::AnonymousBuffer const& pixels);
    bool read_webgl_buffer_sub_data(Web::Painting::CanvasId, Web::WebGL::GLenum target, Web::WebGL::GLintptr offset, Web::WebGL::GLintptr size, Core::AnonymousBuffer const& data);

    Function<void(u64 page_id, Web::MouseEvent)> on_mouse_event;
    Function<void()> on_compositor_lost;

private:
    struct PendingScreenshot {
        NonnullRefPtr<Gfx::PaintingSurface> target_surface;
        NonnullRefPtr<Gfx::Bitmap> target_bitmap;

        Function<void()> callback;
    };

    virtual void die() override;

    virtual void mouse_event(u64 page_id, Web::MouseEvent) override;
    virtual void request_rendering_update() override;
    virtual void did_complete_screenshot(Web::Compositor::ScreenshotRequestId) override;
    virtual void did_fail_screenshot(Web::Compositor::ScreenshotRequestId) override;
    virtual void did_lose_compositor() override;

    bool can_send_message_to_compositor() const;
    Optional<PendingScreenshot> take_screenshot(Web::Compositor::ScreenshotRequestId);

    HashMap<Web::Compositor::ScreenshotRequestId, PendingScreenshot> m_screenshots;
    u64 m_next_screenshot_request_id { 1 };
    bool m_has_lost_compositor { false };
};

}
