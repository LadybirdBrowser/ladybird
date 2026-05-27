/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ConnectionFromWebContent.h>
#include <LibWeb/Page/InputEvent.h>

namespace Compositor {

ConnectionFromWebContent::ConnectionFromWebContent(NonnullOwnPtr<IPC::Transport> transport, NonnullRefPtr<CompositorState> compositor_state, int client_id)
    : IPC::ConnectionFromClient<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>(*this, move(transport), client_id)
    , m_compositor_state(move(compositor_state))
{
}

void ConnectionFromWebContent::die()
{
    auto protector = NonnullRefPtr { *this };
    m_compositor_state->destroy_contexts_for_web_content_client(*this);
    if (m_on_death)
        m_on_death(*this);
}

void ConnectionFromWebContent::notify_compositor_lost()
{
    async_did_lose_compositor();
}

void ConnectionFromWebContent::request_rendering_update()
{
    async_request_rendering_update();
}

void ConnectionFromWebContent::dispatch_mouse_event_to_web_content(u64 page_id, Web::MouseEvent const& event)
{
    async_mouse_event(page_id, event);
}

void ConnectionFromWebContent::verify_context_is_owned_by_this_connection(Web::Compositor::CompositorContextId context_id)
{
    switch (m_compositor_state->check_context_owner(context_id, *this)) {
    case CompositorState::ContextOwnerCheckResult::OwnedByClient:
        return;
    case CompositorState::ContextOwnerCheckResult::ContextUnavailable:
        VERIFY_NOT_REACHED();
    case CompositorState::ContextOwnerCheckResult::ConflictingOwner:
        did_misbehave("WebContent tried to use a compositor context owned by another connection");
        VERIFY_NOT_REACHED();
    }

    VERIFY_NOT_REACHED();
}

void ConnectionFromWebContent::set_presentation_mode(Web::Compositor::CompositorContextId context_id, Web::Compositor::PresentationMode presentation_mode)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->set_presentation_mode(context_id, move(presentation_mode));
}

void ConnectionFromWebContent::destroy_context(Web::Compositor::CompositorContextId context_id)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->destroy_context(context_id);
}

void ConnectionFromWebContent::update_display_list(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Web::Painting::DisplayList> display_list, Web::Painting::AccumulatedVisualContextTree visual_context_tree, Web::Painting::DisplayListResourceTransaction resource_transaction, Web::Painting::ScrollStateSnapshot scroll_state_snapshot)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->update_display_list(context_id, move(display_list), move(visual_context_tree), move(resource_transaction), move(scroll_state_snapshot));
}

void ConnectionFromWebContent::update_scroll_state(Web::Compositor::CompositorContextId context_id, Web::Painting::ScrollStateSnapshot scroll_state_snapshot)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->update_scroll_state(context_id, move(scroll_state_snapshot));
}

void ConnectionFromWebContent::update_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> frame)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->update_video_frame(context_id, frame_id, move(frame));
}

void ConnectionFromWebContent::clear_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->clear_video_frame(context_id, frame_id);
}

void ConnectionFromWebContent::update_compositor_surface(Web::Compositor::CompositorContextId context_id, Web::Painting::CompositorSurfaceId surface_id, Gfx::SharedImage shared_image)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->update_compositor_surface(context_id, surface_id, move(shared_image));
}

void ConnectionFromWebContent::clear_compositor_surface(Web::Compositor::CompositorContextId context_id, Web::Painting::CompositorSurfaceId surface_id)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->clear_compositor_surface(context_id, surface_id);
}

void ConnectionFromWebContent::invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId context_id, u64 generation)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->invalidate_wheel_event_listener_state(context_id, generation);
}

Messages::CompositorWebContentServer::AsyncScrollByResponse ConnectionFromWebContent::async_scroll_by(Web::Compositor::CompositorContextId context_id, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking operation_tracking)
{
    verify_context_is_owned_by_this_connection(context_id);
    auto result = m_compositor_state->async_scroll_by(context_id, document_id, position, delta, viewport_rect, operation_tracking);
    if (result.accepted)
        async_request_rendering_update();
    return result;
}

Messages::CompositorWebContentServer::ShouldDeferMainThreadPresentForAsyncScrollResponse ConnectionFromWebContent::should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId context_id)
{
    verify_context_is_owned_by_this_connection(context_id);
    return m_compositor_state->should_defer_main_thread_present_for_async_scroll(context_id);
}

Messages::CompositorWebContentServer::TakePendingAsyncScrollUpdatesResponse ConnectionFromWebContent::take_pending_async_scroll_updates(Web::Compositor::CompositorContextId context_id)
{
    verify_context_is_owned_by_this_connection(context_id);
    return m_compositor_state->take_pending_async_scroll_updates(context_id);
}

void ConnectionFromWebContent::viewport_size_updated(Web::Compositor::CompositorContextId context_id, Gfx::IntSize viewport_size, Web::Compositor::WindowResizingInProgress window_resize_in_progress)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->viewport_size_updated(context_id, viewport_size, window_resize_in_progress);
}

void ConnectionFromWebContent::present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->present_frame(context_id, viewport_rect);
}

void ConnectionFromWebContent::request_screenshot(Web::Compositor::CompositorContextId context_id, Web::Compositor::ScreenshotRequestId request_id, Gfx::ShareableBitmap target_bitmap)
{
    verify_context_is_owned_by_this_connection(context_id);
    if (m_compositor_state->request_screenshot(context_id, target_bitmap))
        async_did_complete_screenshot(request_id);
    else
        async_did_fail_screenshot(request_id);
}

}
