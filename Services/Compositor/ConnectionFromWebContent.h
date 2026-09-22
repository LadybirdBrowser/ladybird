/*
 * Copyright (c) 2026-present, the Ladybird developers.
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
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/WebGL/Types.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Size.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibMedia/VideoPresentation/VideoPresentationClientConnection.h>

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
    virtual void offer_video_presentation_channel(IPC::TransportHandle handle) override;
    virtual void add_video_sink(Media::VideoSinkHandle) override;
    virtual void remove_video_sink(Media::VideoSinkHandle) override;
    virtual void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick) override;
    virtual void set_parent_context(Compositing::CompositorContextId, Optional<Compositing::CompositorContextId>) override;
    virtual void stop_presenting_to_client(Compositing::CompositorContextId) override;
    virtual void destroy_context(Compositing::CompositorContextId) override;
    virtual void update_display_list(Compositing::CompositorContextId, NonnullRefPtr<Compositing::DisplayList>, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction, Compositing::ScrollStateSnapshot) override;
    virtual void update_visual_context_tree(Compositing::CompositorContextId, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction) override;
    virtual void update_scroll_state(Compositing::CompositorContextId, Compositing::ScrollStateSnapshot, Compositing::KeyboardScrollState) override;
    virtual void update_display_list_resources(Compositing::CompositorContextId, Compositing::DisplayListResourceTransaction) override;
    virtual Messages::CompositorWebContentServer::CreateCanvas2dContextResponse create_canvas_2d_context(Gfx::IntSize, bool) override;
    virtual void update_canvas_2d_stream(Vector<Compositing::Canvas2DCommandStreamSegment>, Vector<Compositing::DisplayListFontResource>) override;
    virtual void destroy_canvas_context(Compositing::CanvasId) override;
    virtual Messages::CompositorWebContentServer::GetCanvasPixelsResponse get_canvas_pixels(Compositing::CanvasId, Gfx::IntRect) override;

    virtual Messages::CompositorWebContentServer::CreateWebglContextResponse create_webgl_context(Compositing::WebGL::WebGLVersion webgl_version, Gfx::IntSize size, bool depth, bool stencil, bool antialias) override;
    virtual void webgl_set_command_buffer(Compositing::CanvasId canvas_id, Core::AnonymousBuffer command_buffer) override;
    virtual void webgl_commands_from_shared_buffer(Compositing::CanvasId canvas_id, u64 offset, u64 size_in_bytes, u64 flush_sequence_number, Vector<Gfx::DecodedImageFrame> bitmaps) override;
    virtual void webgl_drain_command_buffer(Compositing::CanvasId canvas_id) override;
    virtual void webgl_commands(Compositing::CanvasId canvas_id, Core::AnonymousBuffer commands, Vector<Gfx::DecodedImageFrame> bitmaps) override;
    virtual void webgl_present_canvas(Compositing::CanvasId canvas_id, bool preserve_drawing_buffer) override;
    virtual Messages::CompositorWebContentServer::WebglSyncCallResponse webgl_sync_call(Compositing::CanvasId canvas_id, ByteBuffer request) override;
    virtual Messages::CompositorWebContentServer::WebglReadPixelsResponse webgl_read_pixels(Compositing::CanvasId canvas_id, i32 x, i32 y, i32 width, i32 height, u32 format, u32 type, i32 buf_size, Core::AnonymousBuffer pixels) override;
    virtual Messages::CompositorWebContentServer::WebglReadBufferSubDataResponse webgl_read_buffer_sub_data(Compositing::CanvasId canvas_id, u32 target, i64 offset, i64 size, Core::AnonymousBuffer data) override;
    virtual void invalidate_wheel_event_listener_state(Compositing::CompositorContextId, u64 generation) override;
    virtual void invalidate_keyboard_scroll_state(Compositing::CompositorContextId, u64 generation) override;
    virtual Messages::CompositorWebContentServer::AsyncScrollByResponse async_scroll_by(Compositing::CompositorContextId, Compositing::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Compositing::WheelDeltaPrecision, Compositing::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking) override;
    virtual Messages::CompositorWebContentServer::SmoothScrollToResponse smooth_scroll_to(Compositing::CompositorContextId, Compositing::AsyncScrollNodeStableID, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind) override;
    virtual void cancel_smooth_scroll(Compositing::CompositorContextId, Compositing::AsyncScrollNodeStableID) override;
    virtual Messages::CompositorWebContentServer::TakePendingAsyncScrollUpdatesResponse take_pending_async_scroll_updates(Compositing::CompositorContextId) override;
    virtual void viewport_size_updated(Compositing::CompositorContextId, Gfx::IntSize viewport_size, Compositing::WindowResizingInProgress) override;
    virtual void request_rendering_opportunity(Compositing::CompositorContextId, double maximum_frames_per_second) override;
    virtual void hurry_rendering_opportunity(Compositing::CompositorContextId) override;
    virtual void present_frame(Compositing::CompositorContextId, Gfx::IntRect viewport_rect) override;
    virtual void request_screenshot(Compositing::CompositorContextId, Compositing::ScreenshotRequestId request_id, Gfx::ShareableBitmap target_bitmap) override;

    virtual void dispatch_mouse_event_to_web_content(u64 page_id, Compositing::MouseEvent const&) override;
    virtual void dispatch_key_event_to_web_content(u64 page_id, Compositing::KeyEvent const&) override;
    virtual void request_rendering_update() override;
    virtual void rendering_opportunity(Compositing::CompositorContextId, i64 frame_time_nanoseconds, double frame_interval_milliseconds) override;
    virtual void async_scroll_updates(Compositing::CompositorContextId, Compositing::PendingAsyncScrollUpdates const&) override;
    virtual void create_video_edge(Media::VideoSinkHandle) override;
    virtual void release_video_edge(Media::VideoSinkHandle) override;
    bool context_is_owned_by_this_connection(Compositing::CompositorContextId);

    NonnullRefPtr<CompositorState> m_compositor_state;
    CanvasHost m_canvas_host;
    Function<void(ConnectionFromWebContent&)> m_on_death;

    // The presentation client end of this WebContent's video presentation channel (connect-only for now).
    RefPtr<Media::VideoPresentationClientConnection> m_video_presentation_connection;
};

}
