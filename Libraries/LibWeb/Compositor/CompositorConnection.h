/*
 * Copyright (c) 2026-present, the Ladybird developers.
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
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/Canvas2DCommandStream.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibCompositing/Types.h>
#include <LibCompositing/WebGL/Types.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibMedia/Forward.h>
#include <LibWeb/Compositor/CompositorFrame.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/Page/PageId.h>

namespace Web::Compositor {

class CompositorConnectionFrameSink;
// What a test of the transport, which has no Paint thread, reaches a connection's frame sink through. Only the test
// defines it.
struct TransportTestAccess;

class WEB_API CompositorConnection final
    : public IPC::ConnectionToServer<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>
    , public CompositorWebContentClientEndpoint {
    C_OBJECT_ABSTRACT(CompositorConnection)

public:
    explicit CompositorConnection(NonnullOwnPtr<IPC::Transport>);
    virtual ~CompositorConnection() override;

    void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId>);
    void stop_presenting_to_client(Web::CompositorContextId);
    void destroy_context(Web::CompositorContextId);
    // Has the Paint thread present frames through this connection from now on.
    void hand_frame_sink_to_paint_thread();
    void add_video_sink(Media::VideoSinkHandle);
    void remove_video_sink(Media::VideoSinkHandle);
    void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick);
    Optional<Compositing::CanvasId> create_canvas_2d_context(Gfx::IntSize, bool alpha);
    void update_canvas_2d_stream(Compositing::Canvas2DCommandStream&);
    void destroy_canvas_context(Compositing::CanvasId);
    Gfx::ShareableBitmap get_canvas_pixels(Compositing::CanvasId, Gfx::IntRect);
    Optional<Web::Compositor::PlaceholderCanvasLink> allocate_placeholder_canvas();
    void release_placeholder_canvas(Compositing::CanvasId);
    void commit_placeholder_canvas(Web::Compositor::PlaceholderCanvasLink, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize, bool origin_clean);
    Web::Compositor::PlaceholderCanvasPixels get_placeholder_canvas_pixels(Compositing::CanvasId, Gfx::IntRect);
    void invalidate_wheel_event_listener_state(Web::CompositorContextId, u64 generation);
    void invalidate_keyboard_scroll_state(Web::CompositorContextId, u64 generation);
    Compositing::AsyncScrollEnqueueResult async_scroll_by(Web::CompositorContextId, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking);
    Compositing::AsyncScrollEnqueueResult smooth_scroll_to(Web::CompositorContextId, Web::AsyncScrollNodeStableID, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind, Compositing::SmoothScrollInitiator);
    void cancel_smooth_scroll(Web::CompositorContextId, Web::AsyncScrollNodeStableID);
    Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::CompositorContextId, Compositing::AsyncScrollUpdateFreshness);
    void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize, Compositing::WindowResizingInProgress);
    bool request_rendering_opportunity(Web::CompositorContextId, double maximum_frames_per_second);
    void hurry_rendering_opportunity(Web::CompositorContextId);
    void request_screenshot(Web::CompositorContextId, NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&&);

    Optional<Compositing::CanvasId> create_webgl_context(Compositing::WebGL::WebGLVersion, Gfx::IntSize, bool depth, bool stencil, bool antialias, Vector<String>& out_supported_extensions);
    void set_webgl_command_buffer(Compositing::CanvasId, Core::AnonymousBuffer const&);
    void send_webgl_commands_from_shared_buffer(Compositing::CanvasId, u64 offset, u64 size_in_bytes, u64 flush_sequence_number, Vector<Gfx::DecodedImageFrame> const& bitmaps);
    bool drain_webgl_command_buffer(Compositing::CanvasId);
    void send_webgl_commands(Compositing::CanvasId, ByteBuffer const&, Vector<Gfx::DecodedImageFrame> const& bitmaps);
    void present_webgl_canvas(Compositing::CanvasId, bool preserve_drawing_buffer);
    void clear_webgl_drawing_buffer(Compositing::CanvasId);
    ByteBuffer webgl_sync_call(Compositing::CanvasId, ByteBuffer request);
    Compositing::WebGL::ReadPixelsResult read_webgl_pixels(Compositing::CanvasId, Compositing::WebGL::GLint x, Compositing::WebGL::GLint y, Compositing::WebGL::GLsizei width, Compositing::WebGL::GLsizei height, Compositing::WebGL::GLenum format, Compositing::WebGL::GLenum type, Compositing::WebGL::GLsizei buf_size, Core::AnonymousBuffer const& pixels);
    bool read_webgl_buffer_sub_data(Compositing::CanvasId, Compositing::WebGL::GLenum target, Compositing::WebGL::GLintptr offset, Compositing::WebGL::GLintptr size, Core::AnonymousBuffer const& data);

    void ensure_video_presentation_channel();
    // Offers the Compositor the process's render clock, which it delivers display ticks to from then on.
    void attach_render_clock();
    Function<void(Web::PageId page_id, Web::MouseEvent)> on_mouse_event;
    Function<void(Web::PageId page_id, Web::KeyEvent)> on_key_event;
    Function<void()> on_compositor_lost;

private:
    // Only a test of the transport hands a frame straight to the compositor.
    friend struct TransportTestAccess;
    void submit_frame_for_testing(CompositorFrame&&);

    struct PendingScreenshot {
        NonnullRefPtr<Gfx::PaintingSurface> target_surface;
        NonnullRefPtr<Gfx::Bitmap> target_bitmap;

        Function<void()> callback;
    };

    virtual void die() override;

    virtual void mouse_event(u64 page_id, Web::MouseEvent) override;
    virtual void key_event(u64 page_id, Web::KeyEvent) override;
    virtual void request_rendering_update() override;
    virtual void rendering_opportunity(Web::CompositorContextId, i64 frame_time_nanoseconds, double frame_interval_milliseconds) override;
    virtual void async_scroll_updates(Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates) override;
    virtual void did_complete_screenshot(Compositing::ScreenshotRequestId) override;
    virtual void did_fail_screenshot(Compositing::ScreenshotRequestId) override;
    virtual void did_lose_compositor() override;
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool origin_clean) override;

    bool can_send_message_to_compositor() const;
    void merge_async_scroll_updates(Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates);
    Optional<PendingScreenshot> take_screenshot(Compositing::ScreenshotRequestId);

    NonnullRefPtr<CompositorConnectionFrameSink> m_frame_sink;

    HashMap<Compositing::ScreenshotRequestId, PendingScreenshot> m_screenshots;
    // What the compositor process scrolled since the last rendering update adopted it, per context.
    HashMap<Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates> m_pending_async_scroll_updates;
    u64 m_next_screenshot_request_id { 1 };
    bool m_has_lost_compositor { false };
    // The media server whose presentation channel the Compositor holds; a replacement server needs a new channel.
    Optional<u64> m_video_presentation_channel_media_client_generation;
};

}
