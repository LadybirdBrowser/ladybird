/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <Compositor/CanvasHost.h>
#include <Compositor/CompositorState.h>
#include <Compositor/CompositorWebContentClientEndpoint.h>
#include <Compositor/CompositorWebContentServerEndpoint.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Size.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/WebGL/Types.h>

namespace Compositor {

class ConnectionFromWebContent final
    : public IPC::ConnectionFromClient<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>
    , public CompositorStateWebContentClient {
    C_OBJECT(ConnectionFromWebContent);

public:
    virtual ~ConnectionFromWebContent() override = default;
    void notify_compositor_lost();
    void set_on_death(Function<void(ConnectionFromWebContent&)> handler) { m_on_death = move(handler); }

private:
    explicit ConnectionFromWebContent(NonnullOwnPtr<IPC::Transport>, NonnullRefPtr<CompositorState>, int client_id);

    virtual void die() override;

    virtual Messages::CompositorWebContentServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual void set_parent_context(Web::Compositor::CompositorContextId, Optional<Web::Compositor::CompositorContextId>) override;
    virtual void stop_presenting_to_client(Web::Compositor::CompositorContextId) override;
    virtual void destroy_context(Web::Compositor::CompositorContextId) override;
    virtual void update_display_list(Web::Compositor::CompositorContextId, NonnullRefPtr<Web::Painting::DisplayList>, Web::Painting::AccumulatedVisualContextTree, Web::Painting::DisplayListResourceTransaction, Web::Painting::ScrollStateSnapshot) override;
    virtual void update_visual_context_tree(Web::Compositor::CompositorContextId, Web::Painting::AccumulatedVisualContextTree) override;
    virtual void update_scroll_state(Web::Compositor::CompositorContextId, Web::Painting::ScrollStateSnapshot) override;
    virtual void update_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId, NonnullRefPtr<Media::VideoFrame const>) override;
    virtual void clear_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId) override;
    virtual Messages::CompositorWebContentServer::CreateCanvas2dContextResponse create_canvas_2d_context(Gfx::IntSize, bool) override;
    virtual void update_canvas_2d_commands(Web::Painting::CanvasId, Gfx::CanvasCommandList, bool commit) override;
    virtual void destroy_canvas_context(Web::Painting::CanvasId) override;
    virtual Messages::CompositorWebContentServer::GetCanvasPixelsResponse get_canvas_pixels(Web::Painting::CanvasId, Gfx::IntRect) override;

    virtual Messages::CompositorWebContentServer::CreateWebglContextResponse create_webgl_context(Web::WebGL::WebGLVersion webgl_version, Gfx::IntSize size, bool depth, bool stencil, bool antialias) override;
    virtual void webgl_commands(Web::Painting::CanvasId canvas_id, ByteBuffer commands, Vector<Gfx::DecodedImageFrame> bitmaps) override;
    virtual void webgl_present_canvas(Web::Painting::CanvasId canvas_id, bool preserve_drawing_buffer) override;
    virtual Messages::CompositorWebContentServer::WebglSyncCallResponse webgl_sync_call(Web::Painting::CanvasId canvas_id, ByteBuffer request) override;
    virtual Messages::CompositorWebContentServer::WebglReadPixelsResponse webgl_read_pixels(Web::Painting::CanvasId canvas_id, i32 x, i32 y, i32 width, i32 height, u32 format, u32 type, i32 buf_size, Core::AnonymousBuffer pixels) override;
    virtual void webgl_read_buffer_sub_data(Web::Painting::CanvasId canvas_id, u32 target, i64 offset, i64 size, Core::AnonymousBuffer data) override;
    virtual void invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId, u64 generation) override;
    virtual Messages::CompositorWebContentServer::AsyncScrollByResponse async_scroll_by(Web::Compositor::CompositorContextId, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking) override;
    virtual Messages::CompositorWebContentServer::ShouldDeferMainThreadPresentForAsyncScrollResponse should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId) override;
    virtual Messages::CompositorWebContentServer::TakePendingAsyncScrollUpdatesResponse take_pending_async_scroll_updates(Web::Compositor::CompositorContextId) override;
    virtual void viewport_size_updated(Web::Compositor::CompositorContextId, Gfx::IntSize viewport_size, Web::Compositor::WindowResizingInProgress) override;
    virtual void present_frame(Web::Compositor::CompositorContextId, Gfx::IntRect viewport_rect) override;
    virtual void request_screenshot(Web::Compositor::CompositorContextId, Web::Compositor::ScreenshotRequestId request_id, Gfx::ShareableBitmap target_bitmap) override;

    virtual void dispatch_mouse_event_to_web_content(u64 page_id, Web::MouseEvent const&) override;
    virtual void request_rendering_update() override;
    void verify_context_is_owned_by_this_connection(Web::Compositor::CompositorContextId);

    NonnullRefPtr<CompositorState> m_compositor_state;
    CanvasHost m_canvas_host;
    Function<void(ConnectionFromWebContent&)> m_on_death;
};

}
