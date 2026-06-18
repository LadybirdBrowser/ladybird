/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/PaintingSurface.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/Painting/DisplayList.h>

namespace Web::Compositor {

CompositorContextHandle::CompositorContextHandle(CompositorHost& host, CompositorContextId context_id)
    : m_host(host)
    , m_context_id(context_id)
{
}

CompositorContextHandle::~CompositorContextHandle()
{
    m_host.destroy_context(m_context_id);
}

void CompositorContextHandle::set_parent_context(Optional<CompositorContextId> parent_context_id)
{
    m_host.set_parent_context(m_context_id, parent_context_id);
}

void CompositorContextHandle::stop_presenting_to_client()
{
    m_host.stop_presenting_to_client(m_context_id);
}

void CompositorContextHandle::update_display_list(NonnullRefPtr<Painting::DisplayList> display_list, Painting::AccumulatedVisualContextTree visual_context_tree, Painting::DisplayListResourceTransaction&& resource_transaction, Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    m_host.update_display_list(m_context_id, move(display_list), move(visual_context_tree), move(resource_transaction), move(scroll_state_snapshot));
}

void CompositorContextHandle::update_visual_context_tree(Painting::AccumulatedVisualContextTree visual_context_tree)
{
    m_host.update_visual_context_tree(m_context_id, move(visual_context_tree));
}

void CompositorContextHandle::update_video_frame(Painting::VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> frame)
{
    m_host.update_video_frame(m_context_id, frame_id, move(frame));
}

void CompositorContextHandle::clear_video_frame(Painting::VideoFrameResourceId frame_id)
{
    m_host.clear_video_frame(m_context_id, frame_id);
}

void CompositorContextHandle::update_scroll_state(Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    m_host.update_scroll_state(m_context_id, move(scroll_state_snapshot));
}

void CompositorContextHandle::invalidate_wheel_event_listener_state(u64 generation)
{
    m_host.invalidate_wheel_event_listener_state(m_context_id, generation);
}

AsyncScrollEnqueueResult CompositorContextHandle::async_scroll_by(UniqueNodeID expected_document_id, Gfx::FloatPoint position,
    Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, AsyncScrollOperationTracking operation_tracking)
{
    return m_host.async_scroll_by(m_context_id, expected_document_id, position, delta_in_device_pixels, viewport_rect, operation_tracking);
}

bool CompositorContextHandle::should_defer_main_thread_present_for_async_scroll() const
{
    return m_host.should_defer_main_thread_present_for_async_scroll(m_context_id);
}

PendingAsyncScrollUpdates CompositorContextHandle::take_pending_async_scroll_updates()
{
    return m_host.take_pending_async_scroll_updates(m_context_id);
}

void CompositorContextHandle::viewport_size_updated(Gfx::IntSize viewport_size, WindowResizingInProgress window_resize_in_progress)
{
    m_host.viewport_size_updated(m_context_id, viewport_size, window_resize_in_progress);
}

void CompositorContextHandle::present_frame(Gfx::IntRect viewport_rect)
{
    m_host.present_frame(m_context_id, viewport_rect);
}

void CompositorContextHandle::request_screenshot(NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback)
{
    m_host.request_screenshot(m_context_id, move(target_surface), move(callback));
}

CompositorHost::~CompositorHost() = default;

OwnPtr<CompositorContextHandle> CompositorHost::create_context(CompositorContextId context_id)
{
    return adopt_own(*new CompositorContextHandle(*this, context_id));
}

}
