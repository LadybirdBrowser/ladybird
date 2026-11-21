/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibWeb/Compositor/CompositorConnection.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/Compositor/RenderClock.h>

// The Paint stage keeps what the compositor keeps of the display lists sent for each context.
extern "C" void paint_stage_forget_destroyed_compositor_context(u64 context_id);

namespace Web::Compositor {

CompositorContextHandle::CompositorContextHandle(CompositorHost& host, Web::CompositorContextId context_id)
    : m_host(host)
    , m_context_id(context_id)
{
}

CompositorContextHandle::~CompositorContextHandle()
{
    m_host.destroy_context(m_context_id);
}

void CompositorContextHandle::set_parent_context(Optional<Web::CompositorContextId> parent_context_id)
{
    m_host.set_parent_context(m_context_id, parent_context_id);
}

void CompositorContextHandle::stop_presenting_to_client()
{
    m_host.stop_presenting_to_client(m_context_id);
}

bool CompositorContextHandle::ready_for_frame()
{
    m_host.flush_canvas_2d_stream();
    return m_host.compositor_connection();
}

void CompositorContextHandle::set_video_sink_ticking(Media::VideoSinkHandle video_sink_handle, bool should_tick)
{
    m_host.set_video_sink_ticking(video_sink_handle, should_tick);
}

void CompositorContextHandle::invalidate_keyboard_scroll_state(u64 generation)
{
    m_host.invalidate_keyboard_scroll_state(m_context_id, generation);
}

void CompositorContextHandle::invalidate_wheel_event_listener_state(u64 generation)
{
    m_host.invalidate_wheel_event_listener_state(m_context_id, generation);
}

Compositing::WheelEventAnswer CompositorContextHandle::take_wheel_event_from_main_thread(Web::MouseEvent const& event, Compositing::AsyncScrollOperationTracking operation_tracking, Compositing::WheelEventRouting routing)
{
    return m_host.take_wheel_event_from_main_thread(m_context_id, event, operation_tracking, routing);
}

Compositing::WheelEventAnswer CompositorContextHandle::wheel_event_let_through(Compositing::WheelStepLetThrough const& step, Compositing::AsyncScrollOperationTracking operation_tracking)
{
    return m_host.wheel_event_let_through(m_context_id, step, operation_tracking);
}

void CompositorContextHandle::wheel_event_canceled(u64 wheel_gesture_id)
{
    m_host.wheel_event_canceled(m_context_id, wheel_gesture_id);
}

String CompositorContextHandle::scroll_state_for_testing(Web::AsyncScrollNodeStableID stretched_scroller)
{
    return m_host.scroll_state_for_testing(m_context_id, stretched_scroller);
}

void CompositorContextHandle::synchronize_for_testing()
{
    m_host.synchronize_for_testing();
}

Optional<Compositing::FirstWheelStepRouteForTesting> CompositorContextHandle::route_first_wheel_step_for_testing(Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels)
{
    return m_host.route_first_wheel_step_for_testing(m_context_id, position, delta_in_css_pixels);
}

void CompositorContextHandle::begin_input_trace_for_testing()
{
    m_host.begin_input_trace_for_testing(m_context_id);
}

void CompositorContextHandle::clear_per_test_input_state_for_testing()
{
    m_host.clear_per_test_input_state_for_testing(m_context_id);
}

Vector<Web::InputTraceRecord> CompositorContextHandle::take_input_trace_for_testing()
{
    return m_host.take_input_trace_for_testing(m_context_id);
}

Compositing::PendingAsyncScrollUpdates CompositorContextHandle::take_pending_async_scroll_updates(Compositing::AsyncScrollUpdateFreshness freshness)
{
    return m_host.take_pending_async_scroll_updates(m_context_id, freshness);
}

void CompositorContextHandle::viewport_size_updated(Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    m_host.viewport_size_updated(m_context_id, viewport_size, device_pixel_ratio, window_resize_in_progress);
}

bool CompositorContextHandle::request_rendering_opportunity(double maximum_frames_per_second)
{
    return m_host.request_rendering_opportunity(m_context_id, maximum_frames_per_second);
}

void CompositorContextHandle::hurry_rendering_opportunity()
{
    m_host.hurry_rendering_opportunity(m_context_id);
}

void CompositorContextHandle::request_screenshot(NonnullRefPtr<Gfx::Bitmap> target_bitmap, Function<void(ScreenshotResult)>&& callback)
{
    m_host.flush_canvas_2d_stream();
    m_host.request_screenshot(m_context_id, move(target_bitmap), move(callback));
}

void CompositorContextHandle::prepare_remote_descendants_for_screenshot(Function<void()>&& callback)
{
    m_host.prepare_remote_descendants_for_screenshot(m_context_id, move(callback));
}

extern "C" {

void* web_canvas_2d_stream_create();
void web_canvas_2d_stream_destroy(void* stream);
void web_canvas_2d_stream_flush(void* stream, CompositorConnection* connection);
void web_canvas_2d_stream_discard(void* stream);
}

CompositorHost::CompositorHost()
    : m_canvas_2d_stream(web_canvas_2d_stream_create())
{
}

CompositorHost::~CompositorHost()
{
    web_canvas_2d_stream_destroy(m_canvas_2d_stream);
}

OwnPtr<CompositorContextHandle> CompositorHost::create_context(Web::CompositorContextId context_id)
{
    return adopt_own(*new CompositorContextHandle(*this, context_id));
}

Optional<CompositorHost::Canvas2DTransport> CompositorHost::create_canvas_2d_transport()
{
    auto* connection = compositor_connection();
    if (!connection)
        return {};
    connection->ref();
    return Canvas2DTransport { .stream = m_canvas_2d_stream, .connection = connection };
}

void CompositorHost::flush_canvas_2d_stream()
{
    if (auto* connection = compositor_connection())
        web_canvas_2d_stream_flush(m_canvas_2d_stream, connection);
}

void CompositorHost::discard_canvas_2d_stream()
{
    web_canvas_2d_stream_discard(m_canvas_2d_stream);
}

Optional<Web::Compositor::PlaceholderCanvasLink> CompositorHost::allocate_placeholder_canvas()
{
    if (auto* connection = compositor_connection())
        return connection->allocate_placeholder_canvas();
    return {};
}

void CompositorHost::release_placeholder_canvas(Compositing::CanvasId canvas_id)
{
    if (auto* connection = compositor_connection())
        connection->release_placeholder_canvas(canvas_id);
}

void CompositorHost::commit_placeholder_canvas(Web::Compositor::PlaceholderCanvasLink link, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize size, bool origin_clean)
{
    if (auto* connection = compositor_connection())
        connection->commit_placeholder_canvas(link, source_canvas_id, size, origin_clean);
}

Web::Compositor::PlaceholderCanvasPixels CompositorHost::read_placeholder_canvas_pixels(Compositing::CanvasId canvas_id, Gfx::IntRect rect)
{
    if (auto* connection = compositor_connection())
        return connection->get_placeholder_canvas_pixels(canvas_id, rect);
    return {};
}

bool CompositorHost::rasterize_image(Layout::RustFFI::FfiImageRasterization const& image, NonnullRefPtr<Gfx::Bitmap> target) const
{
    auto* connection = compositor_connection();
    if (!connection)
        return false;
    return connection->rasterize_image(image, move(target));
}

bool CompositorHost::rasterize_last_recording(Layout::RustFFI::DocumentHost const& document_host, Layout::BegunRead const& read, NonnullRefPtr<Gfx::Bitmap> target) const
{
    auto* connection = compositor_connection();
    if (!connection)
        return false;
    return connection->rasterize_last_recording(document_host, read, move(target));
}

void CompositorHost::destroy_context(Web::CompositorContextId context_id)
{
    paint_stage_forget_destroyed_compositor_context(context_id.value());
    if (auto* connection = compositor_connection()) {
        connection->destroy_context(context_id);
        // A connection attached the render clock, whose lanes of the context hear of it no more.
        RenderClock::the().forget_context(context_id);
    }
    context_was_destroyed(context_id);
}

void CompositorHost::set_parent_context(Web::CompositorContextId context_id, Optional<Web::CompositorContextId> parent_context_id)
{
    if (auto* connection = compositor_connection())
        connection->set_parent_context(context_id, parent_context_id);
}

void CompositorHost::stop_presenting_to_client(Web::CompositorContextId context_id)
{
    if (auto* connection = compositor_connection())
        connection->stop_presenting_to_client(context_id);
}

void CompositorHost::add_animated_image(u64 frame_id, AnimatedImageRegistration image)
{
    if (auto* connection = compositor_connection())
        connection->add_animated_image(frame_id, image);
    m_animated_images.set(frame_id, move(image));
}

void CompositorHost::remove_animated_image(u64 frame_id)
{
    m_animated_images.remove(frame_id);
    if (auto* connection = compositor_connection())
        connection->remove_animated_image(frame_id);
}

void CompositorHost::start_animated_image(u64 frame_id)
{
    m_animated_images.get(frame_id)->is_started = true;
    if (auto* connection = compositor_connection())
        connection->start_animated_image(frame_id);
}

void CompositorHost::stop_animated_image(u64 frame_id)
{
    m_animated_images.get(frame_id)->is_started = false;
    if (auto* connection = compositor_connection())
        connection->stop_animated_image(frame_id);
}

AnimatedImageState CompositorHost::animated_image_state_for_testing(u64 frame_id) const
{
    if (auto* connection = compositor_connection())
        return connection->animated_image_state_for_testing(frame_id);
    return {};
}

void CompositorHost::register_animated_images_with_new_compositor()
{
    auto* connection = compositor_connection();
    if (!connection)
        return;
    for (auto const& [frame_id, image] : m_animated_images) {
        connection->add_animated_image(frame_id, image);
        if (image.is_started)
            connection->start_animated_image(frame_id);
    }
}

bool CompositorHost::add_video_sink(Media::VideoSinkHandle video_sink_handle, NonnullRefPtr<Compositing::VideoFramePresentationPage> presentation_page)
{
    auto* connection = compositor_connection();
    if (!connection)
        return false;
    connection->add_video_sink(video_sink_handle, move(presentation_page));
    return true;
}

void CompositorHost::remove_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    if (auto* connection = compositor_connection())
        connection->remove_video_sink(video_sink_handle);
}

void CompositorHost::set_video_sink_ticking(Media::VideoSinkHandle video_sink_handle, bool should_tick)
{
    if (auto* connection = compositor_connection())
        connection->set_video_sink_ticking(video_sink_handle, should_tick);
}

Compositing::VideoFrameHold::ReleaseThroughCompositor CompositorHost::video_frame_hold_releaser_for_current_connection() const
{
    auto* connection = compositor_connection();
    if (!connection)
        return {};
    return [weak_connection = connection->make_weak_ptr<CompositorConnection>()](Media::VideoSinkHandle video_sink_handle, u64 generation) {
        if (auto connection = weak_connection.strong_ref())
            connection->release_video_frame_hold(video_sink_handle, generation);
    };
}

void CompositorHost::invalidate_keyboard_scroll_state(Web::CompositorContextId context_id, u64 generation)
{
    if (auto* connection = compositor_connection())
        connection->invalidate_keyboard_scroll_state(context_id, generation);
}

void CompositorHost::invalidate_wheel_event_listener_state(Web::CompositorContextId context_id, u64 generation)
{
    if (auto* connection = compositor_connection())
        connection->invalidate_wheel_event_listener_state(context_id, generation);
}

Compositing::WheelEventAnswer CompositorHost::take_wheel_event_from_main_thread(Web::CompositorContextId context_id, Web::MouseEvent const& event, Compositing::AsyncScrollOperationTracking operation_tracking, Compositing::WheelEventRouting routing)
{
    if (auto* connection = compositor_connection())
        return connection->take_wheel_event_from_main_thread(context_id, event, operation_tracking, routing);
    return {};
}

Compositing::WheelEventAnswer CompositorHost::wheel_event_let_through(Web::CompositorContextId context_id, Compositing::WheelStepLetThrough const& step, Compositing::AsyncScrollOperationTracking operation_tracking)
{
    if (auto* connection = compositor_connection())
        return connection->wheel_event_let_through(context_id, step, operation_tracking);
    return {};
}

void CompositorHost::wheel_event_canceled(Web::CompositorContextId context_id, u64 wheel_gesture_id)
{
    if (auto* connection = compositor_connection())
        connection->wheel_event_canceled(context_id, wheel_gesture_id);
}

String CompositorHost::scroll_state_for_testing(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stretched_scroller)
{
    if (auto* connection = compositor_connection())
        return connection->scroll_state_for_testing(context_id, stretched_scroller);
    return {};
}

void CompositorHost::synchronize_for_testing()
{
    if (auto* connection = compositor_connection())
        connection->synchronize_for_testing();
}

Optional<Compositing::FirstWheelStepRouteForTesting> CompositorHost::route_first_wheel_step_for_testing(Web::CompositorContextId context_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels)
{
    if (auto* connection = compositor_connection())
        return connection->route_first_wheel_step_for_testing(context_id, position, delta_in_css_pixels);
    return {};
}

void CompositorHost::begin_input_trace_for_testing(Web::CompositorContextId context_id)
{
    if (auto* connection = compositor_connection())
        connection->begin_input_trace_for_testing(context_id);
}

void CompositorHost::clear_per_test_input_state_for_testing(Web::CompositorContextId context_id)
{
    if (auto* connection = compositor_connection())
        connection->clear_per_test_input_state_for_testing(context_id);
}

Vector<Web::InputTraceRecord> CompositorHost::take_input_trace_for_testing(Web::CompositorContextId context_id)
{
    if (auto* connection = compositor_connection())
        return connection->take_input_trace_for_testing(context_id);
    return {};
}

Compositing::PendingAsyncScrollUpdates CompositorHost::take_pending_async_scroll_updates(Web::CompositorContextId context_id, Compositing::AsyncScrollUpdateFreshness freshness)
{
    if (auto* connection = compositor_connection())
        return connection->take_pending_async_scroll_updates(context_id, freshness);
    return {};
}

void CompositorHost::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    if (auto* connection = compositor_connection())
        connection->viewport_size_updated(context_id, viewport_size, device_pixel_ratio, window_resize_in_progress);
}

bool CompositorHost::request_rendering_opportunity(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    if (auto* connection = compositor_connection())
        return connection->request_rendering_opportunity(context_id, maximum_frames_per_second);
    return false;
}

void CompositorHost::hurry_rendering_opportunity(Web::CompositorContextId context_id)
{
    if (auto* connection = compositor_connection())
        connection->hurry_rendering_opportunity(context_id);
}

void CompositorHost::prepare_remote_descendants_for_screenshot(Web::CompositorContextId context_id, Function<void()>&& callback)
{
    if (auto* connection = compositor_connection()) {
        connection->prepare_remote_descendants_for_screenshot(context_id, move(callback));
        return;
    }
    callback();
}

void CompositorHost::request_screenshot(Web::CompositorContextId context_id, NonnullRefPtr<Gfx::Bitmap> target_bitmap, Function<void(ScreenshotResult)>&& callback)
{
    if (auto* connection = compositor_connection()) {
        connection->request_screenshot(context_id, move(target_bitmap), move(callback));
        return;
    }
    if (callback)
        callback(ScreenshotResult::Failed);
}

}
