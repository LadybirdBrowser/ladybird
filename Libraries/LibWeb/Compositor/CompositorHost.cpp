/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/Canvas2DCommandStream.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/Compositor/CompositorFrame.h>
#include <LibWeb/Compositor/CompositorHost.h>

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

void CompositorContextHandle::submit_frame(PresentationTurn turn, CompositorFrame&& frame)
{
    frame.context_id = m_context_id;
    // Pending canvas commands (and present markers) must reach the Compositor
    // before a display list that samples the presented canvas surfaces.
    if (frame.display_list_update.has_value() || frame.present_viewport_rect.has_value())
        m_host.flush_canvas_2d_stream();
    m_host.submit_frame(turn, move(frame));
}

RefPtr<CompositorFrameSink> CompositorContextHandle::frame_sink()
{
    m_host.flush_canvas_2d_stream();
    return m_host.frame_sink();
}

void CompositorContextHandle::add_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    m_host.add_video_sink(video_sink_handle);
}

void CompositorContextHandle::remove_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    m_host.remove_video_sink(video_sink_handle);
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

Compositing::AsyncScrollEnqueueResult CompositorContextHandle::async_scroll_by(UniqueNodeID expected_document_id, Gfx::FloatPoint position,
    Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase, u32 modifiers, Compositing::AsyncScrollOperationTracking operation_tracking)
{
    return m_host.async_scroll_by(m_context_id, expected_document_id, position, delta_in_device_pixels, viewport_rect, wheel_delta_precision, scroll_gesture_phase, modifiers, operation_tracking);
}

Compositing::AsyncScrollEnqueueResult CompositorContextHandle::smooth_scroll_to(Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint offset_in_device_pixels, Gfx::FloatPoint main_thread_offset_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind animation_kind, Compositing::SmoothScrollInitiator initiator)
{
    return m_host.smooth_scroll_to(m_context_id, stable_node_id, offset_in_device_pixels, main_thread_offset_in_device_pixels, viewport_rect, animation_kind, initiator);
}

void CompositorContextHandle::cancel_smooth_scroll(Web::AsyncScrollNodeStableID stable_node_id)
{
    m_host.cancel_smooth_scroll(m_context_id, stable_node_id);
}

Compositing::PendingAsyncScrollUpdates CompositorContextHandle::take_pending_async_scroll_updates(Compositing::AsyncScrollUpdateFreshness freshness)
{
    return m_host.take_pending_async_scroll_updates(m_context_id, freshness);
}

void CompositorContextHandle::viewport_size_updated(Gfx::IntSize viewport_size, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    m_host.viewport_size_updated(m_context_id, viewport_size, window_resize_in_progress);
}

bool CompositorContextHandle::request_rendering_opportunity(double maximum_frames_per_second)
{
    return m_host.request_rendering_opportunity(m_context_id, maximum_frames_per_second);
}

void CompositorContextHandle::hurry_rendering_opportunity()
{
    m_host.hurry_rendering_opportunity(m_context_id);
}

void CompositorContextHandle::request_screenshot(NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback)
{
    m_host.flush_canvas_2d_stream();
    m_host.request_screenshot(m_context_id, move(target_surface), move(callback));
}

CompositorHost::CompositorHost()
    : m_canvas_2d_stream(adopt_ref(*new Compositing::Canvas2DCommandStream()))
{
}

CompositorHost::~CompositorHost() = default;

OwnPtr<CompositorContextHandle> CompositorHost::create_context(Web::CompositorContextId context_id)
{
    return adopt_own(*new CompositorContextHandle(*this, context_id));
}

void CompositorHost::flush_canvas_2d_stream()
{
    if (m_canvas_2d_stream->is_empty())
        return;
    send_canvas_2d_stream(*m_canvas_2d_stream);
}

void CompositorHost::discard_canvas_2d_stream()
{
    (void)m_canvas_2d_stream->take_segments();
}

}
