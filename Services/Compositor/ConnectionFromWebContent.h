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
#include <LibImageDecoderClient/Client.h>
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

    virtual void connect_to_image_decoder(IPC::TransportHandle) override;
    virtual void add_animated_image(u64 frame_id, i64 session_id, Gfx::IntSize, Gfx::ColorSpace, Vector<u32> frame_durations_in_milliseconds, u32 loop_count) override;
    virtual void remove_animated_image(u64 frame_id) override;
    virtual void start_animated_image(u64 frame_id) override;
    virtual void stop_animated_image(u64 frame_id) override;
    virtual Messages::CompositorWebContentServer::AnimatedImageStateForTestingResponse animated_image_state_for_testing(u64 frame_id) override;
    virtual void add_video_sink(Media::VideoSinkHandle, NonnullRefPtr<Compositing::VideoFramePresentationPage>) override;
    virtual void remove_video_sink(Media::VideoSinkHandle) override;
    virtual void release_video_frame_hold(Media::VideoSinkHandle, u64 generation) override;
    virtual void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick) override;
    virtual void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId>) override;
    virtual void stop_presenting_to_client(Web::CompositorContextId) override;
    virtual void destroy_context(Web::CompositorContextId) override;
    virtual void update_display_list(Web::CompositorContextId, Core::AnonymousBuffer display_list_buffer, u64 tape_size, u64 run_count, Compositing::DisplayList::Properties, IPC::Encoded<Optional<Compositing::DisplayList::AsyncScrollingMetadata>>, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction, ByteBuffer scroll_state) override;
    virtual void update_visual_context_tree(Web::CompositorContextId, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction, ByteBuffer scroll_state, IPC::Encoded<Compositing::KeyboardScrollState>) override;
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
    virtual void synchronize_for_testing() override { }
    virtual void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress) override;
    virtual ErrorOr<OwnPtr<IPC::MessageBuffer>> handle_raw(IPC::RawMessage&) override;
    virtual void request_rendering_opportunity(Web::CompositorContextId, double maximum_frames_per_second) override;
    virtual void hurry_rendering_opportunity(Web::CompositorContextId) override;
    virtual void present_frame(Web::CompositorContextId, Gfx::IntRect viewport_rect) override;
    virtual void request_screenshot(Web::CompositorContextId, Compositing::ScreenshotRequestId request_id, Gfx::ShareableBitmap target_bitmap) override;

    virtual WebContentClientId web_content_client_id() const override { return m_web_content_client_id; }
    virtual void post_message_to_web_content(IPC::MessageBuffer&) override;
    virtual void post_message_to_render_clock(IPC::MessageBuffer&) override;
    virtual void rendering_opportunity(Web::CompositorContextId, i64 frame_time_nanoseconds, double frame_interval_milliseconds) override;
    virtual void create_video_edge(Media::VideoSinkHandle) override;
    virtual void release_video_edge(Media::VideoSinkHandle) override;
    virtual void request_animated_image_frames(AnimatedImage::FrameRequest const&) override;
    virtual void video_sink_presented_frame(Media::VideoSinkHandle, RefPtr<Media::VideoFrame>, Media::DisplayingVideoSinkFrameCounts) override;
    virtual void video_sink_counted_frames(Media::VideoSinkHandle, Media::DisplayingVideoSinkFrameCounts) override;
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool origin_clean) override;
    virtual Compositing::CanvasSurfaceOwnerId canvas_surface_owner_id() const override { return m_canvas_host.canvas_surface_owner_id(); }
    bool context_is_owned_by_this_connection(Web::CompositorContextId);

    NonnullRefPtr<CompositorState> m_compositor_state;
    WebContentClientId m_web_content_client_id;
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
    void drop_image_decoder();

    RefPtr<ImageDecoderClient::Client> m_image_decoder;
    RefPtr<RenderClockConnection> m_render_clock_connection;
};

}
