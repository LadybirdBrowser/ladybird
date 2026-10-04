/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Math.h>
#include <Compositor/ConnectionFromWebContent.h>
#include <LibCompositing/WebGL/WebGLSharedCommandBuffer.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibWebCommon/Page/InputEvent.h>

namespace Compositor {

ConnectionFromWebContent::ConnectionFromWebContent(NonnullOwnPtr<IPC::Transport> transport, NonnullRefPtr<CompositorState> compositor_state, int client_id)
    : IPC::ConnectionFromClient<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>(*this, move(transport), client_id)
    , m_compositor_state(move(compositor_state))
    , m_canvas_host(m_compositor_state->skia_backend_context(), m_compositor_state->canvas_surface_registry())
{
}

void ConnectionFromWebContent::die()
{
    auto protector = NonnullRefPtr { *this };
    m_compositor_state->destroy_contexts_for_web_content_client(*this);
    if (m_on_death)
        m_on_death(*this);
}

void ConnectionFromWebContent::offer_video_presentation_channel(IPC::TransportHandle handle)
{
    auto transport_or_error = handle.create_transport();
    if (transport_or_error.is_error()) {
        did_misbehave("WebContent sent an unusable video presentation transport handle");
        return;
    }
    m_video_presentation_connection = adopt_ref(*new Media::VideoPresentationClientConnection(transport_or_error.release_value()));

#ifdef AK_OS_WINDOWS
    m_video_presentation_connection->transport().set_peer_pid(transport().peer_pid());
#endif

    dbgln_if(VIDEO_PRESENTATION_CHANNEL_DEBUG, "Compositor: established video presentation channel for WebContent (client_id={})", client_id());
}

void ConnectionFromWebContent::offer_render_clock_channel(IPC::TransportHandle handle)
{
    auto transport_or_error = handle.create_transport();
    if (transport_or_error.is_error()) {
        did_misbehave("WebContent sent an unusable render clock transport handle");
        return;
    }
    // A new channel replaces the one before it, whose requests were for the contexts the process armed then.
    m_render_clock_connection = RenderClockConnection::construct(transport_or_error.release_value(), client_id());
#ifdef AK_OS_WINDOWS
    m_render_clock_connection->transport().set_peer_pid(transport().peer_pid());
#endif
    m_render_clock_connection->on_request_clock_tick = [this](Web::CompositorContextId context_id, double maximum_frames_per_second) {
        if (!context_is_owned_by_this_connection(context_id))
            return;
        if (!isfinite(maximum_frames_per_second) || maximum_frames_per_second <= 0) {
            did_misbehave("WebContent sent an invalid maximum clock tick rate");
            return;
        }
        m_compositor_state->request_clock_tick(context_id, maximum_frames_per_second);
    };
}

void ConnectionFromWebContent::clock_tick(Web::CompositorContextId context_id, i64 frame_time_nanoseconds, double frame_interval_milliseconds)
{
    if (m_render_clock_connection)
        m_render_clock_connection->async_clock_tick(context_id, frame_time_nanoseconds, frame_interval_milliseconds);
}

void ConnectionFromWebContent::add_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    m_compositor_state->add_video_sink(*this, video_sink_handle);
}

void ConnectionFromWebContent::remove_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    m_compositor_state->remove_video_sink(*this, video_sink_handle);
}

void ConnectionFromWebContent::set_video_sink_ticking(Media::VideoSinkHandle video_sink_handle, bool should_tick)
{
    m_compositor_state->set_video_sink_ticking(*this, video_sink_handle, should_tick);
}

void ConnectionFromWebContent::create_video_edge(Media::VideoSinkHandle video_sink_handle)
{
    if (!m_video_presentation_connection)
        return;
    m_video_presentation_connection->create_edge(video_sink_handle, [this, video_sink_handle](NonnullRefPtr<Media::DisplayingVideoSink> sink) {
        m_compositor_state->on_video_sink_ready(*this, video_sink_handle, move(sink));
    });
}

void ConnectionFromWebContent::release_video_edge(Media::VideoSinkHandle video_sink_handle)
{
    if (m_video_presentation_connection)
        m_video_presentation_connection->release_edge(video_sink_handle);
}

void ConnectionFromWebContent::notify_compositor_lost()
{
    async_did_lose_compositor();
}

Messages::CompositorWebContentServer::InitTransportResponse ConnectionFromWebContent::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#else
    did_misbehave("Unexpected Compositor transport initialization from WebContent");
    return 0;
#endif
}

void ConnectionFromWebContent::request_rendering_update()
{
    async_request_rendering_update();
}

void ConnectionFromWebContent::rendering_opportunity(Web::CompositorContextId context_id, i64 frame_time_nanoseconds, double frame_interval_milliseconds)
{
    async_rendering_opportunity(context_id, frame_time_nanoseconds, frame_interval_milliseconds);
}

void ConnectionFromWebContent::async_scroll_updates(Web::CompositorContextId context_id, Compositing::PendingAsyncScrollUpdates const& updates)
{
    async_async_scroll_updates(context_id, updates);
}

void ConnectionFromWebContent::dispatch_mouse_event_to_web_content(u64 page_id, Web::MouseEvent const& event)
{
    async_mouse_event(page_id, event);
}

void ConnectionFromWebContent::dispatch_key_event_to_web_content(u64 page_id, Web::KeyEvent const& event)
{
    async_key_event(page_id, event);
}

bool ConnectionFromWebContent::context_is_owned_by_this_connection(Web::CompositorContextId context_id)
{
    switch (m_compositor_state->check_context_owner(context_id, *this)) {
    case CompositorState::ContextOwnerCheckResult::OwnedByClient:
        return true;
    case CompositorState::ContextOwnerCheckResult::ContextUnavailable:
        return false;
    case CompositorState::ContextOwnerCheckResult::ConflictingOwner:
        did_misbehave("WebContent tried to use a compositor context owned by another connection");
        return false;
    }

    VERIFY_NOT_REACHED();
}

void ConnectionFromWebContent::request_rendering_opportunity(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    if (!isfinite(maximum_frames_per_second) || maximum_frames_per_second <= 0) {
        did_misbehave("WebContent sent an invalid maximum frame rate");
        return;
    }
    m_compositor_state->request_rendering_opportunity(context_id, maximum_frames_per_second);
}

void ConnectionFromWebContent::hurry_rendering_opportunity(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->hurry_rendering_opportunity(context_id);
}

void ConnectionFromWebContent::set_parent_context(Web::CompositorContextId context_id, Optional<Web::CompositorContextId> parent_context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->set_parent_context(context_id, parent_context_id);
}

void ConnectionFromWebContent::stop_presenting_to_client(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->stop_presenting_to_client(context_id);
}

void ConnectionFromWebContent::destroy_context(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->destroy_context(context_id);
}

static bool display_list_timing_enabled()
{
    static bool enabled = [] {
        auto value = Core::Environment::get("LADYBIRD_DISPLAY_LIST_TIMING"sv);
        return value.has_value() && !value->is_empty() && *value != "0"sv;
    }();
    return enabled;
}

void ConnectionFromWebContent::update_display_list(Web::CompositorContextId context_id, Core::AnonymousBuffer display_list_buffer, u64 tape_size, u64 run_count, Compositing::DisplayList::Properties display_list_properties, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction resource_transaction, Compositing::ScrollStateSnapshot scroll_state_snapshot)
{
    auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    auto display_list = Compositing::DisplayList::create_from_shared_buffer(move(display_list_properties), move(display_list_buffer), tape_size, run_count);
    if (display_list.is_error()) {
        dbgln("Compositor: Rejecting display list from WebContent: {}", display_list.error());
        did_misbehave("WebContent published a display list its shared buffer does not hold");
        return;
    }
    auto adopt_time = timer.elapsed_time();
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->update_display_list(context_id, display_list.release_value(), move(visual_context_tree), move(resource_transaction), move(scroll_state_snapshot));
    if (display_list_timing_enabled())
        dbgln("DISPLAY_LIST_RECEIVE bytes={} adopt={} µs install={} µs", tape_size, adopt_time.to_microseconds(), (timer.elapsed_time() - adopt_time).to_microseconds());
}

void ConnectionFromWebContent::update_display_list_resources(Web::CompositorContextId context_id, Compositing::DisplayListResourceTransaction resource_transaction)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->update_display_list_resources(context_id, move(resource_transaction));
}

void ConnectionFromWebContent::update_visual_context_tree(Web::CompositorContextId context_id, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction resource_transaction)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->update_visual_context_tree(context_id, move(visual_context_tree), move(resource_transaction));
}

void ConnectionFromWebContent::update_scroll_state(Web::CompositorContextId context_id, Compositing::ScrollStateSnapshot scroll_state_snapshot, Compositing::KeyboardScrollState keyboard_scroll_state)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->update_scroll_state(context_id, move(scroll_state_snapshot), move(keyboard_scroll_state));
}

Messages::CompositorWebContentServer::CreateCanvas2dContextResponse ConnectionFromWebContent::create_canvas_2d_context(Gfx::IntSize size, bool alpha)
{
    auto canvas_id = m_canvas_host.create_2d_context(size, alpha);
    if (!canvas_id.has_value())
        return { false, Compositing::CanvasId { 0 } };
    return { true, *canvas_id };
}

void ConnectionFromWebContent::update_canvas_2d_stream(Vector<Compositing::Canvas2DCommandStreamSegment> segments, Vector<Compositing::DisplayListFontResource> fonts)
{
    m_canvas_host.execute_canvas_2d_stream(segments, fonts);
}

void ConnectionFromWebContent::destroy_canvas_context(Compositing::CanvasId canvas_id)
{
    m_canvas_host.destroy_context(canvas_id);
}

Messages::CompositorWebContentServer::GetCanvasPixelsResponse ConnectionFromWebContent::get_canvas_pixels(Compositing::CanvasId canvas_id, Gfx::IntRect rect)
{
    return m_canvas_host.read_back_pixels(canvas_id, rect);
}

Messages::CompositorWebContentServer::AllocatePlaceholderCanvasResponse ConnectionFromWebContent::allocate_placeholder_canvas()
{
    auto allocation = m_compositor_state->allocate_placeholder_canvas(*this);
    return { allocation.canvas_id, allocation.secret };
}

void ConnectionFromWebContent::release_placeholder_canvas(Compositing::CanvasId canvas_id)
{
    m_compositor_state->release_placeholder_canvas(*this, canvas_id);
}

void ConnectionFromWebContent::commit_placeholder_canvas(Compositing::CanvasId canvas_id, u64 secret, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize size, bool origin_clean)
{
    RefPtr<Gfx::PaintingSurface> source_surface;
    if (source_canvas_id.has_value()) {
        source_surface = m_canvas_host.presented_surface(*source_canvas_id);
        if (!source_surface)
            return;
    }
    m_compositor_state->commit_placeholder_canvas(canvas_id, secret, move(source_surface), size, origin_clean);
}

Messages::CompositorWebContentServer::GetPlaceholderCanvasPixelsResponse ConnectionFromWebContent::get_placeholder_canvas_pixels(Compositing::CanvasId canvas_id, Gfx::IntRect rect)
{
    auto result = m_compositor_state->read_placeholder_canvas_pixels(*this, canvas_id, rect);
    return { move(result.pixels), result.origin_clean };
}

void ConnectionFromWebContent::placeholder_canvas_committed(Compositing::CanvasId canvas_id, Gfx::IntSize size, bool origin_clean)
{
    async_placeholder_canvas_committed(canvas_id, size, origin_clean);
}

Messages::CompositorWebContentServer::CreateWebglContextResponse ConnectionFromWebContent::create_webgl_context(Compositing::WebGL::WebGLVersion webgl_version, Gfx::IntSize size, bool depth, bool stencil, bool antialias)
{
    auto result = m_canvas_host.create_webgl_context(webgl_version, size, depth, stencil, antialias);
    return { result.success, result.canvas_id, move(result.supported_extensions) };
}

void ConnectionFromWebContent::webgl_set_command_buffer(Compositing::CanvasId canvas_id, Core::AnonymousBuffer command_buffer)
{
    auto shared_command_buffer = Compositing::WebGL::WebGLSharedCommandBuffer::adopt_received_buffer(move(command_buffer));
    if (!shared_command_buffer.has_value()) {
        did_misbehave("WebContent sent an invalid WebGL shared command buffer");
        return;
    }

    m_canvas_host.set_webgl_shared_command_buffer(canvas_id, shared_command_buffer.release_value());
}

void ConnectionFromWebContent::webgl_commands_from_shared_buffer(Compositing::CanvasId canvas_id, u64 offset, u64 size_in_bytes, u64 flush_sequence_number, Vector<Gfx::DecodedImageFrame> bitmaps)
{
    if (!m_canvas_host.execute_webgl_commands_from_shared_buffer(canvas_id, offset, size_in_bytes, flush_sequence_number, bitmaps))
        did_misbehave("WebContent published an invalid WebGL shared command buffer range");
}

void ConnectionFromWebContent::webgl_drain_command_buffer(Compositing::CanvasId)
{
    // The empty reply is the point: it proves every earlier message on this connection,
    // including all published command ranges, has already been processed.
}

void ConnectionFromWebContent::webgl_commands(Compositing::CanvasId canvas_id, Core::AnonymousBuffer commands, Vector<Gfx::DecodedImageFrame> bitmaps)
{
    if (!commands.is_valid()) {
        did_misbehave("WebContent sent an invalid WebGL command buffer");
        return;
    }

    m_canvas_host.execute_webgl_commands(canvas_id, commands.bytes(), bitmaps);
}

void ConnectionFromWebContent::webgl_present_canvas(Compositing::CanvasId canvas_id, bool preserve_drawing_buffer)
{
    m_canvas_host.present_webgl_canvas(canvas_id, preserve_drawing_buffer);
}

void ConnectionFromWebContent::webgl_clear_drawing_buffer(Compositing::CanvasId canvas_id)
{
    m_canvas_host.clear_webgl_drawing_buffer(canvas_id);
}

Messages::CompositorWebContentServer::WebglSyncCallResponse ConnectionFromWebContent::webgl_sync_call(Compositing::CanvasId canvas_id, ByteBuffer request)
{
    return MUST(m_canvas_host.execute_webgl_sync_call(canvas_id, move(request)));
}

Messages::CompositorWebContentServer::WebglReadPixelsResponse ConnectionFromWebContent::webgl_read_pixels(Compositing::CanvasId canvas_id, i32 x, i32 y, i32 width, i32 height, u32 format, u32 type, i32 buf_size, Core::AnonymousBuffer pixels)
{
    if (buf_size < 0 || (buf_size > 0 && (!pixels.is_valid() || pixels.size() < static_cast<size_t>(buf_size)))) {
        did_misbehave("WebContent sent an invalid WebGL readPixels buffer");
        return { 0, 0, 0 };
    }

    auto result = m_canvas_host.webgl_read_pixels_robust_angle(canvas_id, x, y, width, height, format, type, buf_size, move(pixels));
    return { result.length, result.columns, result.rows };
}

Messages::CompositorWebContentServer::WebglReadBufferSubDataResponse ConnectionFromWebContent::webgl_read_buffer_sub_data(Compositing::CanvasId canvas_id, u32 target, i64 offset, i64 size, Core::AnonymousBuffer data)
{
    if (size < 0 || (size > 0 && (!data.is_valid() || data.size() < static_cast<size_t>(size)))) {
        did_misbehave("WebContent sent an invalid WebGL buffer readback target");
        return { false };
    }

    return { m_canvas_host.webgl_read_buffer_sub_data(canvas_id, target, offset, size, move(data)) };
}

void ConnectionFromWebContent::invalidate_keyboard_scroll_state(Web::CompositorContextId context_id, u64 generation)
{
    if (context_is_owned_by_this_connection(context_id))
        m_compositor_state->invalidate_keyboard_scroll_state(context_id, generation);
}

void ConnectionFromWebContent::invalidate_wheel_event_listener_state(Web::CompositorContextId context_id, u64 generation)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->invalidate_wheel_event_listener_state(context_id, generation);
}

Messages::CompositorWebContentServer::AsyncScrollByResponse ConnectionFromWebContent::async_scroll_by(Web::CompositorContextId context_id, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase, u32 modifiers, Compositing::AsyncScrollOperationTracking operation_tracking)
{
    if (!context_is_owned_by_this_connection(context_id))
        return Compositing::AsyncScrollEnqueueResult {};
    auto result = m_compositor_state->async_scroll_by(context_id, document_id, position, delta, viewport_rect, wheel_delta_precision, scroll_gesture_phase, modifiers, operation_tracking);
    if (result.accepted)
        async_request_rendering_update();
    return result;
}

Messages::CompositorWebContentServer::SmoothScrollToResponse ConnectionFromWebContent::smooth_scroll_to(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind animation_kind, Compositing::SmoothScrollInitiator initiator)
{
    if (!context_is_owned_by_this_connection(context_id))
        return Compositing::AsyncScrollEnqueueResult {};
    auto result = m_compositor_state->smooth_scroll_to(context_id, stable_node_id, offset, main_thread_offset, viewport_rect, animation_kind, initiator);
    if (result.accepted)
        async_request_rendering_update();
    return result;
}

void ConnectionFromWebContent::cancel_smooth_scroll(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stable_node_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->cancel_smooth_scroll(context_id, stable_node_id);
}

Messages::CompositorWebContentServer::TakePendingAsyncScrollUpdatesResponse ConnectionFromWebContent::take_pending_async_scroll_updates(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return Compositing::PendingAsyncScrollUpdates {};
    return m_compositor_state->take_pending_async_scroll_updates(context_id);
}

void ConnectionFromWebContent::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->viewport_size_updated(context_id, viewport_size, window_resize_in_progress);
}

void ConnectionFromWebContent::present_frame(Web::CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->present_frame(context_id, viewport_rect);
}

void ConnectionFromWebContent::request_screenshot(Web::CompositorContextId context_id, Compositing::ScreenshotRequestId request_id, Gfx::ShareableBitmap target_bitmap)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    if (m_compositor_state->request_screenshot(context_id, target_bitmap))
        async_did_complete_screenshot(request_id);
    else
        async_did_fail_screenshot(request_id);
}

}
