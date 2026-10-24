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
#include <Compositor/RenderClockConnection.h>
#include <Compositor/VideoFrameHolds.h>
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

    // The Browser makes these channels and hands each end to the process it belongs to.
    void install_video_presentation_channel(IPC::TransportHandle);
    void install_render_clock_channel(IPC::TransportHandle);

private:
    explicit ConnectionFromWebContent(NonnullOwnPtr<IPC::Transport>, NonnullRefPtr<CompositorState>, int client_id);

    virtual void die() override;

    virtual void add_video_sink(Media::VideoSinkHandle, NonnullRefPtr<Compositing::VideoFramePresentationPage>) override;
    virtual void remove_video_sink(Media::VideoSinkHandle) override;
    virtual void release_video_frame_hold(Media::VideoSinkHandle, u64 generation) override;
    virtual void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick) override;
    virtual void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId>) override;
    virtual void stop_presenting_to_client(Web::CompositorContextId) override;
    virtual void destroy_context(Web::CompositorContextId) override;
    virtual void update_display_list(Web::CompositorContextId, Core::AnonymousBuffer display_list_buffer, u64 tape_size, u64 run_count, Compositing::DisplayList::Properties, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction, Compositing::ScrollStateSnapshot) override;
    virtual void update_visual_context_tree(Web::CompositorContextId, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction) override;
    virtual void update_scroll_state(Web::CompositorContextId, Compositing::ScrollStateSnapshot, Compositing::KeyboardScrollState) override;
    virtual void update_display_list_resources(Web::CompositorContextId, Compositing::DisplayListResourceTransaction) override;
    virtual Messages::CompositorWebContentServer::ReserveCanvasIdsResponse reserve_canvas_ids() override;
    virtual void create_canvas_2d_context(Compositing::CanvasId, Gfx::IntSize, bool alpha, bool will_read_frequently, Gfx::ColorSpace) override;
    virtual void update_canvas_2d_stream(Vector<Compositing::Canvas2DCommandStreamSegment>, Vector<Compositing::FontResourceId>, Vector<Compositing::DisplayListFontResource>, Vector<Compositing::DisplayListImageFrameResource>) override;
    virtual void destroy_canvas_context(Compositing::CanvasId) override;
    virtual Messages::CompositorWebContentServer::GetCanvasPixelsResponse get_canvas_pixels(Compositing::CanvasId, Gfx::IntRect, Gfx::AlphaType, Gfx::ColorSpace) override;
    virtual Messages::CompositorWebContentServer::AllocatePlaceholderCanvasResponse allocate_placeholder_canvas() override;
    virtual void release_placeholder_canvas(Compositing::CanvasId) override;
    virtual void commit_placeholder_canvas(Compositing::CanvasId, u64 secret, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize, bool origin_clean) override;
    virtual Messages::CompositorWebContentServer::GetPlaceholderCanvasPixelsResponse get_placeholder_canvas_pixels(Compositing::CanvasId, Gfx::IntRect) override;
    virtual Messages::CompositorWebContentServer::RasterizeDisplayListResponse rasterize_display_list(Core::AnonymousBuffer display_list_buffer, u64 tape_size, u64 run_count, Compositing::DisplayList::Properties, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction, Gfx::ShareableBitmap target_bitmap) override;

    virtual Messages::CompositorWebContentServer::CreateWebglContextResponse create_webgl_context(Compositing::WebGL::WebGLVersion webgl_version, Gfx::IntSize size, bool depth, bool stencil, bool antialias) override;
    virtual void webgl_set_command_buffer(Compositing::CanvasId canvas_id, Core::AnonymousBuffer command_buffer) override;
    virtual void webgl_commands_from_shared_buffer(Compositing::CanvasId canvas_id, u64 offset, u64 size_in_bytes, u64 flush_sequence_number, Vector<Compositing::WebGL::ImageSourceFrame> frames) override;
    virtual void webgl_drain_command_buffer(Compositing::CanvasId canvas_id) override;
    virtual void webgl_commands(Compositing::CanvasId canvas_id, Core::AnonymousBuffer commands, Vector<Compositing::WebGL::ImageSourceFrame> frames) override;
    virtual void webgl_present_canvas(Compositing::CanvasId canvas_id, bool preserve_drawing_buffer) override;
    virtual void webgl_clear_drawing_buffer(Compositing::CanvasId canvas_id) override;
    virtual Messages::CompositorWebContentServer::WebglSyncCallResponse webgl_sync_call(Compositing::CanvasId canvas_id, ByteBuffer request) override;
    virtual Messages::CompositorWebContentServer::WebglReadPixelsResponse webgl_read_pixels(Compositing::CanvasId canvas_id, i32 x, i32 y, i32 width, i32 height, u32 format, u32 type, i32 buf_size, Core::AnonymousBuffer pixels) override;
    virtual Messages::CompositorWebContentServer::WebglReadBufferSubDataResponse webgl_read_buffer_sub_data(Compositing::CanvasId canvas_id, u32 target, i64 offset, i64 size, Core::AnonymousBuffer data) override;
    virtual void invalidate_wheel_event_listener_state(Web::CompositorContextId, u64 generation) override;
    virtual void invalidate_keyboard_scroll_state(Web::CompositorContextId, u64 generation) override;
    virtual Messages::CompositorWebContentServer::AsyncScrollByResponse async_scroll_by(Web::CompositorContextId, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking, Compositing::WheelStepDispatch, u64 input_event_id) override;
    virtual Messages::CompositorWebContentServer::SmoothScrollToResponse smooth_scroll_to(Web::CompositorContextId, Web::AsyncScrollNodeStableID, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind, Compositing::SmoothScrollInitiator, Optional<Compositing::AsyncScrollOperationID> operation_to_continue) override;
    virtual Messages::CompositorWebContentServer::ScrollStateForTestingResponse scroll_state_for_testing(Web::CompositorContextId, Web::AsyncScrollNodeStableID) override;
    virtual void synchronize_for_testing() override { }
    virtual Messages::CompositorWebContentServer::RouteFirstWheelStepForTestingResponse route_first_wheel_step_for_testing(Web::CompositorContextId, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels) override;
    virtual void begin_input_trace_for_testing(Web::CompositorContextId) override;
    virtual Messages::CompositorWebContentServer::TakeInputTraceForTestingResponse take_input_trace_for_testing(Web::CompositorContextId) override;
    virtual void cancel_smooth_scroll(Web::CompositorContextId, Web::AsyncScrollNodeStableID) override;
    virtual Messages::CompositorWebContentServer::TakePendingAsyncScrollUpdatesResponse take_pending_async_scroll_updates(Web::CompositorContextId) override;
    virtual void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress) override;
    virtual void request_rendering_opportunity(Web::CompositorContextId, double maximum_frames_per_second) override;
    virtual void hurry_rendering_opportunity(Web::CompositorContextId) override;
    virtual void present_frame(Web::CompositorContextId, Gfx::IntRect viewport_rect) override;
    virtual void request_screenshot(Web::CompositorContextId, Compositing::ScreenshotRequestId request_id, Gfx::ShareableBitmap target_bitmap) override;

    virtual void dispatch_mouse_event_to_web_content(u64 page_id, Web::MouseEvent const&) override;
    virtual void dispatch_key_event_to_web_content(u64 page_id, Web::KeyEvent const&) override;
    virtual void request_rendering_update() override;
    virtual void rendering_opportunity(Web::CompositorContextId, i64 frame_time_nanoseconds, double frame_interval_milliseconds) override;
    virtual void clock_tick(Web::CompositorContextId, i64 frame_time_nanoseconds, double frame_interval_milliseconds, Vector<Web::CompositorScrollOffset> const& scroll_offsets) override;
    virtual void pointer_moved(Web::CompositorContextId, Web::DevicePixelPoint, u32 buttons, bool scrolled_since_frame, u64 input_event_id) override;
    virtual void pointer_left(Web::CompositorContextId, u64 input_event_id) override;
    virtual void async_scroll_updates(Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates const&) override;
    virtual void overlay_scrollbar_visibilities_changed(Vector<Web::OverlayScrollbarVisibility> const&) override;
    virtual void create_video_edge(Media::VideoSinkHandle) override;
    virtual void release_video_edge(Media::VideoSinkHandle) override;
    virtual void video_sink_presented_frame(Media::VideoSinkHandle, RefPtr<Media::VideoFrame>, Media::DisplayingVideoSinkFrameCounts) override;
    virtual void video_sink_counted_frames(Media::VideoSinkHandle, Media::DisplayingVideoSinkFrameCounts) override;
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool origin_clean) override;
    virtual Compositing::CanvasSurfaceOwnerId canvas_surface_owner_id() const override { return m_canvas_host.canvas_surface_owner_id(); }
    bool context_is_owned_by_this_connection(Web::CompositorContextId);

    NonnullRefPtr<CompositorState> m_compositor_state;
    VideoFrameHolds m_video_frame_holds;
    CanvasHost m_canvas_host;

    // The canvas IDs this WebContent has reserved and not yet named a 2D context with.
    struct ReservedCanvasIds {
        u64 next { 0 };
        u64 end { 0 };

        bool is_used_up() const { return next == end; }
    };
    ReservedCanvasIds m_reserved_canvas_ids;
    Function<void(ConnectionFromWebContent&)> m_on_death;

    // The presentation client end of this WebContent's video presentation channel (connect-only for now).
    RefPtr<Media::VideoPresentationClientConnection> m_video_presentation_connection;
    RefPtr<RenderClockConnection> m_render_clock_connection;
};

}
