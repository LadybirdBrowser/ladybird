/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <Compositor/CompositorWebContentClientEndpoint.h>
#include <Compositor/CompositorWebContentServerEndpoint.h>
#include <LibCompositing/DisplayList/DisplayListResources.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/ThreadSafeMessagePoster.h>
#include <LibMedia/Forward.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/Page/PageId.h>

namespace Web::Layout::RustFFI {

struct DocumentHost;
struct FfiImageRasterization;
enum class RasterizationOutcome : u8;

}

namespace Web::Compositor {

// Shared memory made for a message that Rust writes and posts.
struct FfiSharedBuffer {
    u8* data;
    size_t size;
    // A descriptor of the memory of its own, which the caller takes over.
    int descriptor;
    void* owner;
};

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
    void hand_poster_to_paint_thread();
    void add_animated_image(u64 frame_id, AnimatedImageRegistration const&);
    void remove_animated_image(u64 frame_id);
    void start_animated_image(u64 frame_id);
    void stop_animated_image(u64 frame_id);
    AnimatedImageState animated_image_state_for_testing(u64 frame_id);
    void add_video_sink(Media::VideoSinkHandle, NonnullRefPtr<Compositing::VideoFramePresentationPage>);
    void remove_video_sink(Media::VideoSinkHandle);
    void release_video_frame_hold(Media::VideoSinkHandle, u64 generation);
    void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick);
    RefPtr<IPC::ThreadSafeMessagePoster> poster_while_compositor_is_reachable();
    bool post_canvas_2d_stream_batch_fonts_and_image_frames(ReadonlySpan<Gfx::Font const*>, ReadonlySpan<Gfx::DecodedImageFrame const*>);
    void did_fail_to_post_canvas_2d_message();
    bool register_image_frames(ReadonlySpan<Gfx::DecodedImageFrame const*>);
    bool can_send_message_to_compositor() const;
    u64 serial() const { return m_serial; }
    Optional<Web::Compositor::PlaceholderCanvasLink> allocate_placeholder_canvas();
    void release_placeholder_canvas(Compositing::CanvasId);
    void commit_placeholder_canvas(Web::Compositor::PlaceholderCanvasLink, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize, bool origin_clean);
    Web::Compositor::PlaceholderCanvasPixels get_placeholder_canvas_pixels(Compositing::CanvasId, Gfx::IntRect);
    // Has the compositor rasterize `image` into `target`, and waits for it to.
    bool rasterize_image(Layout::RustFFI::FfiImageRasterization const&, NonnullRefPtr<Gfx::Bitmap> target);
    // Has the compositor rasterize the recording the document of the host took in last into `target`, and waits for
    // it to.
    bool rasterize_last_recording(Layout::RustFFI::DocumentHost const&, Layout::BegunRead const&, NonnullRefPtr<Gfx::Bitmap> target);
    void invalidate_wheel_event_listener_state(Web::CompositorContextId, u64 generation);
    void invalidate_keyboard_scroll_state(Web::CompositorContextId, u64 generation);
    Compositing::WheelEventAnswer take_wheel_event_from_main_thread(Web::CompositorContextId, Web::MouseEvent const&, Compositing::AsyncScrollOperationTracking, Compositing::WheelEventRouting);
    Compositing::WheelEventAnswer wheel_event_let_through(Web::CompositorContextId, Compositing::WheelStepLetThrough const&, Compositing::AsyncScrollOperationTracking);
    void wheel_event_canceled(Web::CompositorContextId, u64 wheel_gesture_id);
    String scroll_state_for_testing(Web::CompositorContextId, Web::AsyncScrollNodeStableID stretched_scroller);
    void synchronize_for_testing();
    Optional<Compositing::FirstWheelStepRouteForTesting> route_first_wheel_step_for_testing(Web::CompositorContextId, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels);
    void begin_input_trace_for_testing(Web::CompositorContextId);
    void clear_per_test_input_state_for_testing(Web::CompositorContextId);
    Vector<Web::InputTraceRecord> take_input_trace_for_testing(Web::CompositorContextId);
    Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::CompositorContextId, Compositing::AsyncScrollUpdateFreshness);
    void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize, double device_pixel_ratio, Compositing::WindowResizingInProgress);
    bool request_rendering_opportunity(Web::CompositorContextId, double maximum_frames_per_second);
    void hurry_rendering_opportunity(Web::CompositorContextId);
    void request_screenshot(Web::CompositorContextId, NonnullRefPtr<Gfx::Bitmap>, Function<void(ScreenshotResult)>&&);
    // Has the processes that own the context's descendants in other processes paint them, and calls `callback` once the
    // compositor has their frames, or once it is lost.
    void prepare_remote_descendants_for_screenshot(Web::CompositorContextId, Function<void()>&& callback);

    void ensure_video_presentation_channel();
    void ensure_image_decoder_channel();
    // Takes up the render clock channel the Browser made between this process and the Compositor, which delivers
    // display ticks over it from then on.
    void attach_render_clock(IPC::TransportHandle);
    Function<void(Web::PageId page_id, Web::MouseEvent)> on_mouse_event;
    Function<void(Web::PageId page_id, Web::KeyEvent)> on_key_event;
    Function<void(Web::PageId page_id, Web::PinchEvent)> on_pinch_event;
    Function<void()> on_compositor_lost;
    // Asks the Browser to connect this process's media server to the Compositor.
    Function<void()> on_request_video_presentation_channel;

private:
    struct PendingScreenshot {
        NonnullRefPtr<Gfx::Bitmap> target_bitmap;

        Function<void(ScreenshotResult)> callback;
    };

    virtual void die() override;

    virtual void mouse_event(u64 page_id, Web::MouseEvent) override;
    virtual void key_event(u64 page_id, Web::KeyEvent) override;
    virtual void pinch_event(u64 page_id, Web::PinchEvent) override;
    virtual void request_rendering_update() override;
    virtual void rendering_opportunity(Web::CompositorContextId, i64 frame_time_nanoseconds, double frame_interval_milliseconds) override;
    virtual void async_scroll_updates(Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates) override;
    virtual void animated_content_moved_under_pointer(Web::CompositorContextId, i64 frame_time_nanoseconds) override;
    bool did_rasterize(Layout::RustFFI::RasterizationOutcome);
    virtual void did_complete_screenshot(Compositing::ScreenshotRequestId) override;
    virtual void did_fail_screenshot(Compositing::ScreenshotRequestId) override;
    virtual void paint_contexts_for_screenshot(u64 preparation_id, Vector<Web::CompositorContextId>) override;
    virtual void remote_descendants_prepared_for_screenshot(u64 request_id) override;
    virtual void did_lose_compositor() override;
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool origin_clean) override;
    virtual void overlay_scrollbar_visibilities_changed(Vector<Web::OverlayScrollbarVisibility>) override;

    void merge_async_scroll_updates(Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates);
    Optional<PendingScreenshot> take_screenshot(Compositing::ScreenshotRequestId);

    // Posts to the transport from any thread: the Paint thread presents through it, and Rust posts what it encodes.
    NonnullRefPtr<IPC::ThreadSafeMessagePoster> m_poster;

    HashMap<Compositing::ScreenshotRequestId, PendingScreenshot> m_screenshots;
    HashMap<u64, Function<void()>> m_remote_descendant_preparations;
    u64 m_next_remote_descendant_preparation_id { 1 };
    // What the compositor process scrolled since the last rendering update adopted it, per context.
    HashMap<Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates> m_pending_async_scroll_updates;
    u64 m_next_screenshot_request_id { 1 };
    Atomic<u64> m_next_rasterization_token { 1 };
    u64 m_serial { 0 };
    bool m_has_lost_compositor { false };
    // The media server whose presentation channel the Compositor holds; a replacement server needs a new channel.
    Optional<u64> m_video_presentation_channel_media_client_generation;
    Optional<u64> m_image_decoder_channel_client_generation;
};

}
