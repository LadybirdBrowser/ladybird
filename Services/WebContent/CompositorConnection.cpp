/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <WebContent/CompositorConnection.h>

#include <LibCore/EventLoop.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <WebContent/WebContentCompositorHost.h>

namespace WebContent {

CompositorConnection::CompositorConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>(*this, move(transport))
{
}

void CompositorConnection::die()
{
    // The compositor process owns this WebContent's painting and presentation.
    // WebContent cannot continue without that connection, but the browser
    // process owns recovery and already treats an active renderer exit as a
    // crash. Avoid reporting an assertion failure during browser shutdown.
    Core::EventLoop::current().quit(0);
}

void CompositorConnection::set_presentation_mode(Web::Compositor::CompositorContextId context_id, Web::Compositor::PresentationMode const& presentation_mode)
{
    async_set_presentation_mode(context_id, presentation_mode);
}

void CompositorConnection::stop_presenting_to_client(Web::Compositor::CompositorContextId context_id)
{
    async_stop_presenting_to_client(context_id);
}

void CompositorConnection::destroy_context(Web::Compositor::CompositorContextId context_id)
{
    async_destroy_context(context_id);
}

void CompositorConnection::update_display_list(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Web::Painting::DisplayList> const& display_list, Web::Painting::DisplayListResourceTransaction const& resource_transaction, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot)
{
    auto encoded_message = MUST(Messages::CompositorWebContentServer::UpdateDisplayList::static_encode(context_id, display_list, resource_transaction, scroll_state_snapshot));
    MUST(post_message(encoded_message));
}

void CompositorConnection::update_scroll_state(Web::Compositor::CompositorContextId context_id, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot)
{
    async_update_scroll_state(context_id, scroll_state_snapshot);
}

void CompositorConnection::update_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> const& frame)
{
    auto encoded_message = MUST(Messages::CompositorWebContentServer::UpdateVideoFrame::static_encode(context_id, frame_id, frame));
    MUST(post_message(encoded_message));
}

void CompositorConnection::clear_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id)
{
    async_clear_video_frame(context_id, frame_id);
}

void CompositorConnection::update_compositor_surface(Web::Compositor::CompositorContextId context_id, Web::Painting::CompositorSurfaceId surface_id, Gfx::SharedImage const& shared_image)
{
    async_update_compositor_surface(context_id, surface_id, shared_image);
}

void CompositorConnection::clear_compositor_surface(Web::Compositor::CompositorContextId context_id, Web::Painting::CompositorSurfaceId surface_id)
{
    async_clear_compositor_surface(context_id, surface_id);
}

void CompositorConnection::invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId context_id, u64 generation)
{
    async_invalidate_wheel_event_listener_state(context_id, generation);
}

Web::Compositor::AsyncScrollEnqueueResult CompositorConnection::async_scroll_by(Web::Compositor::CompositorContextId context_id, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking operation_tracking)
{
    auto response = send_sync<Messages::CompositorWebContentServer::AsyncScrollBy>(context_id, document_id, position, delta, viewport_rect, operation_tracking);
    return response->take_result();
}

bool CompositorConnection::should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId context_id)
{
    auto response = send_sync<Messages::CompositorWebContentServer::ShouldDeferMainThreadPresentForAsyncScroll>(context_id);
    return response->should_defer();
}

Web::Compositor::PendingAsyncScrollUpdates CompositorConnection::take_pending_async_scroll_updates(Web::Compositor::CompositorContextId context_id)
{
    auto response = send_sync<Messages::CompositorWebContentServer::TakePendingAsyncScrollUpdates>(context_id);
    return response->take_updates();
}

void CompositorConnection::viewport_size_updated(Web::Compositor::CompositorContextId context_id, Gfx::IntSize viewport_size, bool is_top_level_traversable, Web::Compositor::WindowResizingInProgress window_resize_in_progress)
{
    async_viewport_size_updated(context_id, viewport_size, is_top_level_traversable, window_resize_in_progress);
}

void CompositorConnection::present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    async_present_frame(context_id, viewport_rect);
}

void CompositorConnection::request_screenshot(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback)
{
    auto target_bitmap = MUST(Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, target_surface->size()));
    auto shareable_bitmap = Gfx::ShareableBitmap { target_bitmap, Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
    auto request_id = Web::Compositor::ScreenshotRequestId { m_next_screenshot_request_id++ };
    m_screenshots.set(request_id, PendingScreenshot { move(target_surface), move(target_bitmap), move(callback) });
    async_request_screenshot(context_id, request_id, move(shareable_bitmap));
}

void CompositorConnection::mouse_event(u64 page_id, Web::MouseEvent event)
{
    if (on_mouse_event)
        on_mouse_event(page_id, move(event));
}

void CompositorConnection::request_rendering_update()
{
    Web::HTML::main_thread_event_loop().queue_task_to_update_the_rendering();
}

void CompositorConnection::did_complete_screenshot(Web::Compositor::ScreenshotRequestId request_id)
{
    auto pending_screenshot = take_screenshot(request_id);
    VERIFY(pending_screenshot.has_value());

    pending_screenshot->target_surface->write_from_bitmap(*pending_screenshot->target_bitmap);
    if (pending_screenshot->callback)
        pending_screenshot->callback();
}

void CompositorConnection::did_fail_screenshot(Web::Compositor::ScreenshotRequestId request_id)
{
    auto pending_screenshot = take_screenshot(request_id);
    VERIFY(pending_screenshot.has_value());

    if (pending_screenshot->callback)
        pending_screenshot->callback();
}

void CompositorConnection::did_lose_compositor()
{
    // The Compositor service sends this when its browser-process control
    // connection is gone. WebContent cannot present without that owner either,
    // so exit cleanly instead of reporting a renderer crash during shutdown.
    die();
}

Optional<CompositorConnection::PendingScreenshot> CompositorConnection::take_screenshot(Web::Compositor::ScreenshotRequestId request_id)
{
    return m_screenshots.take(request_id);
}

}
