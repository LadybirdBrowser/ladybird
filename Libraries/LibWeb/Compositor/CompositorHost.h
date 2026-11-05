/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>
#include <LibCompositing/Types.h>
#include <LibCompositing/Video/VideoFrameHold.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibMedia/Forward.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Page/InputTrace.h>

namespace Web::Compositor {

class CompositorFrameSink;
class CompositorHost;
class NavigablePresenter;
struct CompositorFrame;

// Whether the compositor took a requested screenshot, so painted the bitmap the request handed it.
enum class ScreenshotResult : u8 {
    Taken,
    Failed,
};

struct PlaceholderCanvasLink {
    Compositing::CanvasId canvas_id;
    u64 secret { 0 };
};

struct PlaceholderCanvasPixels {
    RefPtr<Gfx::Bitmap> bitmap;
    bool origin_clean { true };
};

// What the compositor needs to animate an image: it asks the decoder's session for the frames.
struct AnimatedImageRegistration {
    i64 session_id { 0 };
    Gfx::IntSize size;
    Gfx::ColorSpace color_space;
    Vector<u32> frame_durations_in_milliseconds;
    u32 loop_count { 0 };
    // The decoder the session belongs to, which may be gone by the time a new compositor hears of the image.
    u64 image_decoder_client_generation { 0 };
    bool is_started { false };
};

struct AnimatedImageState {
    bool is_animating { false };
    u32 frame_index { 0 };
    u32 shown_frame_index { 0 };
    u32 loops_completed { 0 };
};

class WEB_API CompositorContextHandle {
    AK_MAKE_NONCOPYABLE(CompositorContextHandle);
    AK_MAKE_NONMOVABLE(CompositorContextHandle);

public:
    AK_ALLOC_WITH_KMALLOC;

    ~CompositorContextHandle();

    Web::CompositorContextId id() const { return m_context_id; }
    void set_parent_context(Optional<Web::CompositorContextId>);
    void stop_presenting_to_client();

    void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick);
    void invalidate_wheel_event_listener_state(u64 generation);
    void invalidate_keyboard_scroll_state(u64 generation);
    Compositing::WheelEventAnswer take_wheel_event_from_main_thread(Web::MouseEvent const&, Compositing::AsyncScrollOperationTracking, Compositing::WheelEventRouting);
    Compositing::WheelEventAnswer wheel_event_let_through(Compositing::WheelStepLetThrough const&, Compositing::AsyncScrollOperationTracking);
    void wheel_event_canceled(u64 wheel_gesture_id);
    String scroll_state_for_testing(Web::AsyncScrollNodeStableID stretched_scroller);
    void synchronize_for_testing();
    Optional<Compositing::FirstWheelStepRouteForTesting> route_first_wheel_step_for_testing(Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels);
    void begin_input_trace_for_testing();
    void clear_per_test_input_state_for_testing();
    Vector<Web::InputTraceRecord> take_input_trace_for_testing();
    Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Compositing::AsyncScrollUpdateFreshness);
    void viewport_size_updated(Gfx::IntSize, double device_pixel_ratio, Compositing::WindowResizingInProgress);
    bool request_rendering_opportunity(double maximum_frames_per_second);
    void hurry_rendering_opportunity();
    void request_screenshot(NonnullRefPtr<Gfx::Bitmap>, Function<void(ScreenshotResult)>&& callback);
    // Sends the canvas commands a frame may sample ahead of it, and answers whether the compositor can be reached.
    bool ready_for_frame();

private:
    friend class CompositorHost;

    CompositorContextHandle(CompositorHost&, Web::CompositorContextId);

    CompositorHost& m_host;
    Web::CompositorContextId m_context_id;
};

class WEB_API CompositorHost : public Weakable<CompositorHost> {
    AK_MAKE_NONCOPYABLE(CompositorHost);
    AK_MAKE_NONMOVABLE(CompositorHost);

public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~CompositorHost();

    OwnPtr<CompositorContextHandle> create_context(Web::CompositorContextId);

    Compositing::Canvas2DCommandStream& canvas_2d_stream() { return *m_canvas_2d_stream; }
    void flush_canvas_2d_stream();
    void discard_canvas_2d_stream();

    RefPtr<WebGL::RemoteWebGLTransport> create_webgl_transport();
    RefPtr<HTML::RemoteCanvas2DTransport> create_canvas_2d_transport();

    Optional<PlaceholderCanvasLink> allocate_placeholder_canvas();
    void release_placeholder_canvas(Compositing::CanvasId);
    void commit_placeholder_canvas(PlaceholderCanvasLink, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize, bool origin_clean);
    PlaceholderCanvasPixels read_placeholder_canvas_pixels(Compositing::CanvasId, Gfx::IntRect);

    // Has the compositor draw a display list that belongs to no compositor context into the target, which must be a
    // shareable BGRA8888 bitmap with premultiplied alpha. Returns false if nothing was drawn.
    bool rasterize_display_list(Compositing::DisplayListResource const&, Compositing::DisplayListResourceStorage const&, NonnullRefPtr<Gfx::Bitmap> target) const;

    void destroy_context(Web::CompositorContextId);
    void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId>);
    void stop_presenting_to_client(Web::CompositorContextId);

    // Has the compositor show the frames of an animated image under the image frame with the id.
    void add_animated_image(u64 frame_id, AnimatedImageRegistration);
    void remove_animated_image(u64 frame_id);
    void start_animated_image(u64 frame_id);
    void stop_animated_image(u64 frame_id);
    AnimatedImageState animated_image_state_for_testing(u64 frame_id) const;
    // A new compositor process has heard of none of the images.
    void register_animated_images_with_new_compositor();

    void add_video_sink(Media::VideoSinkHandle, NonnullRefPtr<Compositing::VideoFramePresentationPage>);
    void remove_video_sink(Media::VideoSinkHandle);
    void set_video_sink_ticking(Media::VideoSinkHandle, bool should_tick);
    Compositing::VideoFrameHold::ReleaseThroughCompositor video_frame_hold_releaser_for_current_connection() const;
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
    void request_screenshot(Web::CompositorContextId, NonnullRefPtr<Gfx::Bitmap>, Function<void(ScreenshotResult)>&& callback);

protected:
    CompositorHost();

    // The connection to the compositor process, while there is one.
    virtual CompositorConnection* compositor_connection() const = 0;
    virtual void context_was_destroyed(Web::CompositorContextId) { }

private:
    friend class CompositorContextHandle;

    HashMap<u64, AnimatedImageRegistration> m_animated_images;

    // Drains the stream, but only when the message can actually be delivered.
    void send_canvas_2d_stream(Compositing::Canvas2DCommandStream&);

    NonnullRefPtr<Compositing::Canvas2DCommandStream> m_canvas_2d_stream;
};

}
