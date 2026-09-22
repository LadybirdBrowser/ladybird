/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/Canvas2DCommandStream.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/Compositor/CompositorHost.h>

namespace Web::Compositor {

CompositorContextHandle::CompositorContextHandle(CompositorHost& host, Compositing::CompositorContextId context_id)
    : m_host(host)
    , m_context_id(context_id)
{
}

CompositorContextHandle::~CompositorContextHandle()
{
    m_host.destroy_context(m_context_id);
}

void CompositorContextHandle::set_parent_context(Optional<Compositing::CompositorContextId> parent_context_id)
{
    m_host.set_parent_context(m_context_id, parent_context_id);
}

void CompositorContextHandle::stop_presenting_to_client()
{
    m_host.stop_presenting_to_client(m_context_id);
}

void CompositorContextHandle::update_display_list(NonnullRefPtr<Compositing::DisplayList> display_list, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction&& resource_transaction, Compositing::ScrollStateSnapshot&& scroll_state_snapshot)
{
    // Pending canvas commands (and present markers) must reach the Compositor
    // before a display list that samples the presented canvas surfaces.
    m_host.flush_canvas_2d_stream();
    m_host.update_display_list(m_context_id, move(display_list), move(visual_context_tree), move(resource_transaction), move(scroll_state_snapshot));
}

void CompositorContextHandle::update_visual_context_tree(Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction&& resource_transaction)
{
    m_host.update_visual_context_tree(m_context_id, move(visual_context_tree), move(resource_transaction));
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

void CompositorContextHandle::update_scroll_state(Compositing::ScrollStateSnapshot&& scroll_state_snapshot, Compositing::KeyboardScrollState keyboard_scroll_state)
{
    m_host.update_scroll_state(m_context_id, move(scroll_state_snapshot), move(keyboard_scroll_state));
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
    Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::WheelDeltaPrecision wheel_delta_precision, Compositing::ScrollGesturePhase scroll_gesture_phase, u32 modifiers, Compositing::AsyncScrollOperationTracking operation_tracking)
{
    return m_host.async_scroll_by(m_context_id, expected_document_id, position, delta_in_device_pixels, viewport_rect, wheel_delta_precision, scroll_gesture_phase, modifiers, operation_tracking);
}

Compositing::AsyncScrollEnqueueResult CompositorContextHandle::smooth_scroll_to(Compositing::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint offset_in_device_pixels, Gfx::FloatPoint main_thread_offset_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind animation_kind)
{
    return m_host.smooth_scroll_to(m_context_id, stable_node_id, offset_in_device_pixels, main_thread_offset_in_device_pixels, viewport_rect, animation_kind);
}

void CompositorContextHandle::cancel_smooth_scroll(Compositing::AsyncScrollNodeStableID stable_node_id)
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

void CompositorContextHandle::present_frame(Gfx::IntRect viewport_rect)
{
    m_host.flush_canvas_2d_stream();
    m_host.present_frame(m_context_id, viewport_rect);
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

OwnPtr<CompositorContextHandle> CompositorHost::create_context(Compositing::CompositorContextId context_id)
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
