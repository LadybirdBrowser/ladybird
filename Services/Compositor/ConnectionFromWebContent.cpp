/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Math.h>
#include <Compositor/ConnectionFromWebContent.h>
#include <Compositor/RasterizeDisplayList.h>
#include <LibCompositing/DisplayList/Canvas2DTape.h>
#include <LibCompositing/WebGL/WebGLSharedCommandBuffer.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Environment.h>
#include <LibGfx/ColorSpace.h>
#include <LibWebCommon/Page/InputEvent.h>

namespace Compositor {

ConnectionFromWebContent::ConnectionFromWebContent(NonnullOwnPtr<IPC::Transport> transport, NonnullRefPtr<CompositorState> compositor_state, int client_id)
    : IPC::ConnectionFromClient<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>(*this, move(transport), client_id)
    , m_compositor_state(move(compositor_state))
    , m_canvas_host(m_compositor_state->skia_backend_context(), m_compositor_state->canvas_surface_registry(), m_video_frame_holds)
{
}

void ConnectionFromWebContent::die()
{
    auto protector = NonnullRefPtr { *this };
    m_compositor_state->destroy_contexts_for_web_content_client(*this);
    if (m_on_death)
        m_on_death(*this);
}

void ConnectionFromWebContent::install_video_presentation_channel(IPC::TransportHandle handle)
{
    auto transport_or_error = handle.create_transport();
    if (transport_or_error.is_error()) {
        dbgln("Compositor: Could not take up a video presentation channel: {}", transport_or_error.error());
        return;
    }
    m_video_presentation_connection = adopt_ref(*new Media::VideoPresentationClientConnection(transport_or_error.release_value()));
    m_compositor_state->create_video_edges(*this);

    dbgln_if(VIDEO_PRESENTATION_CHANNEL_DEBUG, "Compositor: established video presentation channel for WebContent (client_id={})", client_id());
}

void ConnectionFromWebContent::install_render_clock_channel(IPC::TransportHandle handle)
{
    auto transport_or_error = handle.create_transport();
    if (transport_or_error.is_error()) {
        dbgln("Compositor: Could not take up a render clock channel: {}", transport_or_error.error());
        return;
    }
    // A new channel replaces the one before it, whose requests were for the contexts the process armed then.
    m_render_clock_connection = RenderClockConnection::construct(transport_or_error.release_value(), client_id());
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

void ConnectionFromWebContent::clock_tick(Web::CompositorContextId context_id, i64 frame_time_nanoseconds, double frame_interval_milliseconds, Vector<Web::CompositorScrollOffset> const& scroll_offsets)
{
    if (m_render_clock_connection)
        m_render_clock_connection->async_clock_tick(context_id, frame_time_nanoseconds, frame_interval_milliseconds, scroll_offsets);
}

void ConnectionFromWebContent::pointer_moved(Web::CompositorContextId context_id, Web::DevicePixelPoint position, u32 buttons, bool scrolled_since_frame, u64 input_event_id)
{
    if (m_render_clock_connection)
        m_render_clock_connection->async_pointer_moved(context_id, position, buttons, scrolled_since_frame, input_event_id);
}

void ConnectionFromWebContent::pointer_left(Web::CompositorContextId context_id, u64 input_event_id)
{
    if (m_render_clock_connection)
        m_render_clock_connection->async_pointer_left(context_id, input_event_id);
}

void ConnectionFromWebContent::add_video_sink(Media::VideoSinkHandle video_sink_handle, NonnullRefPtr<Compositing::VideoFramePresentationPage> presentation_page)
{
    m_video_frame_holds.add_sink(video_sink_handle, move(presentation_page));
    m_compositor_state->add_video_sink(*this, video_sink_handle);
}

void ConnectionFromWebContent::remove_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    m_video_frame_holds.remove_sink(video_sink_handle);
    m_canvas_host.forget_video_sink(video_sink_handle);
    m_compositor_state->remove_video_sink(*this, video_sink_handle);
}

void ConnectionFromWebContent::release_video_frame_hold(Media::VideoSinkHandle video_sink_handle, u64 generation)
{
    if (m_video_frame_holds.release_hold(video_sink_handle, generation).is_error())
        did_misbehave("WebContent released a video frame hold that it did not take");
}

void ConnectionFromWebContent::video_sink_presented_frame(Media::VideoSinkHandle video_sink_handle, RefPtr<Media::VideoFrame> frame, Media::DisplayingVideoSinkFrameCounts new_frame_counts)
{
    m_video_frame_holds.present_frame(video_sink_handle, move(frame), new_frame_counts);
}

void ConnectionFromWebContent::video_sink_counted_frames(Media::VideoSinkHandle video_sink_handle, Media::DisplayingVideoSinkFrameCounts new_frame_counts)
{
    m_video_frame_holds.add_frame_counts(video_sink_handle, new_frame_counts);
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

void ConnectionFromWebContent::overlay_scrollbar_visibilities_changed(Vector<Web::OverlayScrollbarVisibility> const& visibilities)
{
    async_overlay_scrollbar_visibilities_changed(visibilities);
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

Messages::CompositorWebContentServer::ReserveCanvasIdsResponse ConnectionFromWebContent::reserve_canvas_ids()
{
    static constexpr u64 reserved_canvas_id_count = 1 << 20;
    if (!m_reserved_canvas_ids.is_used_up()) {
        did_misbehave("WebContent reserved canvas IDs before using up its range");
        return { Compositing::CanvasId { 0 }, 0 };
    }
    auto first_canvas_id = m_compositor_state->canvas_surface_registry().reserve_canvas_ids(reserved_canvas_id_count);
    m_reserved_canvas_ids = { first_canvas_id.value(), first_canvas_id.value() + reserved_canvas_id_count };
    return { first_canvas_id, reserved_canvas_id_count };
}

// NB: The pixels of a 2D context are in one of the predefined color spaces of a canvas, which WebContent describes by
//     coding-independent code points, or as sRGB without color space data. Nothing else can name one.
//     https://html.spec.whatwg.org/multipage/canvas.html#predefinedcolorspace
static bool is_predefined_canvas_color_space(Gfx::ColorSpace const& color_space)
{
    return color_space.data().visit(
        [](Empty) { return true; },
        [](Gfx::ColorSpace::CICP const& cicp) {
            auto has_valid_primaries = cicp.color_primaries == Media::ColorPrimaries::BT709 || cicp.color_primaries == Media::ColorPrimaries::SMPTE432;
            auto has_valid_transfer_characteristics = cicp.transfer_characteristics == Media::TransferCharacteristics::SRGB || cicp.transfer_characteristics == Media::TransferCharacteristics::Linear;
            return has_valid_primaries && has_valid_transfer_characteristics;
        },
        [](NonnullRefPtr<Gfx::ColorSpace::ICCProfile const> const&) { return false; });
}

void ConnectionFromWebContent::create_canvas_2d_context(Compositing::CanvasId canvas_id, Gfx::IntSize size, bool alpha, bool will_read_frequently, Gfx::ColorSpace color_space)
{
    // NB: WebContent names its contexts with the IDs of its range in order, so each ID names one context only.
    if (m_reserved_canvas_ids.is_used_up() || canvas_id.value() != m_reserved_canvas_ids.next) {
        did_misbehave("WebContent named a 2D canvas context with an ID other than the next one it reserved");
        return;
    }
    if (!is_predefined_canvas_color_space(color_space)) {
        did_misbehave("WebContent asked for a 2D canvas context in a color space that is not a predefined one");
        return;
    }
    ++m_reserved_canvas_ids.next;
    // NB: A context that cannot be created is not a protocol violation: commands recorded for it are dropped.
    (void)m_canvas_host.create_2d_context(canvas_id, size, alpha, will_read_frequently, color_space);
}

void ConnectionFromWebContent::update_canvas_2d_stream(Vector<Compositing::Canvas2DCommandStreamSegment> segments, Vector<Compositing::FontResourceId> kept_font_ids, Vector<Compositing::DisplayListFontResource> added_fonts, Vector<Compositing::DisplayListImageFrameResource> image_frames)
{
    for (auto const& segment : segments) {
        if (Compositing::validate_canvas_2d_tape(segment.tape).is_error()) {
            did_misbehave("WebContent sent a canvas 2D stream that does not validate");
            return;
        }
    }
    m_canvas_host.execute_canvas_2d_stream(segments, kept_font_ids, added_fonts, image_frames);
}

void ConnectionFromWebContent::destroy_canvas_context(Compositing::CanvasId canvas_id)
{
    m_canvas_host.destroy_context(canvas_id);
}

Messages::CompositorWebContentServer::GetCanvasPixelsResponse ConnectionFromWebContent::get_canvas_pixels(Compositing::CanvasId canvas_id, Gfx::IntRect rect, Gfx::AlphaType alpha_type, Gfx::ColorSpace color_space)
{
    if (!Gfx::is_valid_alpha_type(to_underlying(alpha_type))) {
        did_misbehave("WebContent asked for canvas pixels with an invalid alpha type");
        return Gfx::ShareableBitmap {};
    }
    if (!is_predefined_canvas_color_space(color_space)) {
        did_misbehave("WebContent asked for canvas pixels in a color space that is not a predefined one");
        return Gfx::ShareableBitmap {};
    }
    return m_canvas_host.read_back_pixels(canvas_id, rect, alpha_type, color_space);
}

Messages::CompositorWebContentServer::RasterizeDisplayListResponse ConnectionFromWebContent::rasterize_display_list(Core::AnonymousBuffer display_list_buffer, u64 tape_size, u64 run_count, Compositing::DisplayList::Properties display_list_properties, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction resource_transaction, Gfx::ShareableBitmap target_bitmap)
{
    auto display_list = Compositing::DisplayList::create_from_shared_buffer(move(display_list_properties), move(display_list_buffer), tape_size, run_count);
    if (display_list.is_error()) {
        dbgln("Compositor: Rejecting display list to rasterize from WebContent: {}", display_list.error());
        did_misbehave("WebContent asked to rasterize a display list its shared buffer does not hold");
        return false;
    }
    if (!target_bitmap.is_valid())
        return false;
    if (auto result = Compositor::rasterize_display_list(*display_list.value(), visual_context_tree, move(resource_transaction), *target_bitmap.bitmap()); result.is_error()) {
        dbgln("Compositor: Not rasterizing display list from WebContent: {}", result.error());
        return false;
    }
    return true;
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

void ConnectionFromWebContent::webgl_commands_from_shared_buffer(Compositing::CanvasId canvas_id, u64 offset, u64 size_in_bytes, u64 flush_sequence_number, Vector<Compositing::WebGL::ImageSourceFrame> frames)
{
    if (!m_canvas_host.execute_webgl_commands_from_shared_buffer(canvas_id, offset, size_in_bytes, flush_sequence_number, frames))
        did_misbehave("WebContent published an invalid WebGL shared command buffer range");
}

void ConnectionFromWebContent::webgl_drain_command_buffer(Compositing::CanvasId)
{
    // The empty reply is the point: it proves every earlier message on this connection,
    // including all published command ranges, has already been processed.
}

void ConnectionFromWebContent::webgl_commands(Compositing::CanvasId canvas_id, Core::AnonymousBuffer commands, Vector<Compositing::WebGL::ImageSourceFrame> frames)
{
    if (!commands.is_valid()) {
        did_misbehave("WebContent sent an invalid WebGL command buffer");
        return;
    }

    m_canvas_host.execute_webgl_commands(canvas_id, commands.bytes(), frames);
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

Messages::CompositorWebContentServer::AsyncScrollByResponse ConnectionFromWebContent::async_scroll_by(Web::CompositorContextId context_id, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase, u32 modifiers, Compositing::AsyncScrollOperationTracking operation_tracking, Compositing::WheelStepDispatch wheel_step_dispatch, u64 input_event_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return Compositing::AsyncScrollEnqueueResult {};
    auto result = m_compositor_state->async_scroll_by(context_id, document_id, position, delta, viewport_rect, wheel_delta_precision, scroll_gesture_phase, modifiers, operation_tracking, wheel_step_dispatch, input_event_id);
    if (result.accepted)
        async_request_rendering_update();
    return result;
}

Messages::CompositorWebContentServer::SmoothScrollToResponse ConnectionFromWebContent::smooth_scroll_to(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind animation_kind, Compositing::SmoothScrollInitiator initiator, Optional<Compositing::AsyncScrollOperationID> operation_to_continue)
{
    if (!context_is_owned_by_this_connection(context_id))
        return Compositing::AsyncScrollEnqueueResult {};
    auto result = m_compositor_state->smooth_scroll_to(context_id, stable_node_id, offset, main_thread_offset, viewport_rect, animation_kind, initiator, operation_to_continue);
    if (result.accepted)
        async_request_rendering_update();
    return result;
}

Messages::CompositorWebContentServer::ScrollStateForTestingResponse ConnectionFromWebContent::scroll_state_for_testing(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stretched_scroller)
{
    if (!context_is_owned_by_this_connection(context_id))
        return String {};
    return m_compositor_state->scroll_state_for_testing(context_id, stretched_scroller);
}

Messages::CompositorWebContentServer::RouteFirstWheelStepForTestingResponse ConnectionFromWebContent::route_first_wheel_step_for_testing(Web::CompositorContextId context_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels)
{
    if (!context_is_owned_by_this_connection(context_id))
        return { {}, {}, {} };
    auto result = m_compositor_state->route_first_wheel_step_for_testing(context_id, position, delta_in_css_pixels);
    if (!result.has_value())
        return { {}, {}, {} };
    return { result->route.decision, result->route.scroller, result->document_id };
}

void ConnectionFromWebContent::begin_input_trace_for_testing(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->begin_input_trace_for_testing(context_id);
}

Messages::CompositorWebContentServer::TakeInputTraceForTestingResponse ConnectionFromWebContent::take_input_trace_for_testing(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return Vector<Web::InputTraceRecord> {};
    return m_compositor_state->take_input_trace_for_testing(context_id);
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

void ConnectionFromWebContent::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    if (!isfinite(device_pixel_ratio) || device_pixel_ratio <= 0) {
        did_misbehave("WebContent sent an invalid device pixel ratio");
        return;
    }
    m_compositor_state->viewport_size_updated(context_id, viewport_size, device_pixel_ratio, window_resize_in_progress);
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
