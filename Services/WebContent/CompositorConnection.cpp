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
    did_lose_compositor();
}

void CompositorConnection::set_parent_context(Web::Compositor::CompositorContextId context_id, Optional<Web::Compositor::CompositorContextId> parent_context_id)
{
    if (!can_send_message_to_compositor())
        return;
    async_set_parent_context(context_id, parent_context_id);
}

void CompositorConnection::stop_presenting_to_client(Web::Compositor::CompositorContextId context_id)
{
    if (!can_send_message_to_compositor())
        return;
    async_stop_presenting_to_client(context_id);
}

void CompositorConnection::destroy_context(Web::Compositor::CompositorContextId context_id)
{
    if (!can_send_message_to_compositor())
        return;
    async_destroy_context(context_id);
}

void CompositorConnection::update_display_list(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Web::Painting::DisplayList> const& display_list, Web::Painting::AccumulatedVisualContextTree const& visual_context_tree, Web::Painting::DisplayListResourceTransaction const& resource_transaction, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot)
{
    if (!can_send_message_to_compositor())
        return;

    auto encoded_message = MUST(Messages::CompositorWebContentServer::UpdateDisplayList::static_encode(context_id, display_list, visual_context_tree, resource_transaction, scroll_state_snapshot));
    if (post_message(encoded_message).is_error())
        did_lose_compositor();
}

void CompositorConnection::update_visual_context_tree(Web::Compositor::CompositorContextId context_id, Web::Painting::AccumulatedVisualContextTree const& visual_context_tree)
{
    if (!can_send_message_to_compositor())
        return;
    async_update_visual_context_tree(context_id, visual_context_tree);
}

void CompositorConnection::update_scroll_state(Web::Compositor::CompositorContextId context_id, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot)
{
    if (!can_send_message_to_compositor())
        return;
    async_update_scroll_state(context_id, scroll_state_snapshot);
}

void CompositorConnection::update_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> const& frame)
{
    if (!can_send_message_to_compositor())
        return;

    auto encoded_message = MUST(Messages::CompositorWebContentServer::UpdateVideoFrame::static_encode(context_id, frame_id, frame));
    if (post_message(encoded_message).is_error())
        did_lose_compositor();
}

void CompositorConnection::clear_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id)
{
    if (!can_send_message_to_compositor())
        return;
    async_clear_video_frame(context_id, frame_id);
}

Optional<Web::Painting::CanvasId> CompositorConnection::create_canvas_2d_context(Gfx::IntSize size, bool alpha)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync<Messages::CompositorWebContentServer::CreateCanvas2dContext>(size, alpha);
    if (!response->success())
        return {};
    return response->canvas_id();
}

void CompositorConnection::update_canvas_2d_commands(Web::Painting::CanvasId canvas_id, Gfx::CanvasCommandList const& commands, bool commit)
{
    if (!can_send_message_to_compositor())
        return;

    auto encoded_message = MUST(Messages::CompositorWebContentServer::UpdateCanvas2dCommands::static_encode(canvas_id, commands, commit));
    if (post_message(encoded_message).is_error())
        did_lose_compositor();
}

void CompositorConnection::destroy_canvas_context(Web::Painting::CanvasId canvas_id)
{
    if (!can_send_message_to_compositor())
        return;
    async_destroy_canvas_context(canvas_id);
}

Gfx::ShareableBitmap CompositorConnection::get_canvas_pixels(Web::Painting::CanvasId canvas_id, Gfx::IntRect rect)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync<Messages::CompositorWebContentServer::GetCanvasPixels>(canvas_id, rect);
    return response->take_pixels();
}

void CompositorConnection::invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId context_id, u64 generation)
{
    if (!can_send_message_to_compositor())
        return;
    async_invalidate_wheel_event_listener_state(context_id, generation);
}

Web::Compositor::AsyncScrollEnqueueResult CompositorConnection::async_scroll_by(Web::Compositor::CompositorContextId context_id, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking operation_tracking)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::AsyncScrollBy>(context_id, document_id, position, delta, viewport_rect, operation_tracking);
    if (!response) {
        did_lose_compositor();
        return {};
    }
    return response->take_result();
}

bool CompositorConnection::should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId context_id)
{
    if (!can_send_message_to_compositor())
        return false;

    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::ShouldDeferMainThreadPresentForAsyncScroll>(context_id);
    if (!response) {
        did_lose_compositor();
        return false;
    }
    return response->should_defer();
}

Web::Compositor::PendingAsyncScrollUpdates CompositorConnection::take_pending_async_scroll_updates(Web::Compositor::CompositorContextId context_id)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::TakePendingAsyncScrollUpdates>(context_id);
    if (!response) {
        did_lose_compositor();
        return {};
    }
    return response->take_updates();
}

void CompositorConnection::viewport_size_updated(Web::Compositor::CompositorContextId context_id, Gfx::IntSize viewport_size, Web::Compositor::WindowResizingInProgress window_resize_in_progress)
{
    if (!can_send_message_to_compositor())
        return;
    async_viewport_size_updated(context_id, viewport_size, window_resize_in_progress);
}

void CompositorConnection::present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    if (!can_send_message_to_compositor())
        return;
    async_present_frame(context_id, viewport_rect);
}

Optional<Web::Painting::CanvasId> CompositorConnection::create_webgl_context(Web::WebGL::WebGLVersion webgl_version, Gfx::IntSize size, bool depth, bool stencil, bool antialias, Vector<String>& out_supported_extensions)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync<Messages::CompositorWebContentServer::CreateWebglContext>(webgl_version, size, depth, stencil, antialias);
    out_supported_extensions = response->take_supported_extensions();
    if (!response->success())
        return {};
    return response->canvas_id();
}

void CompositorConnection::send_webgl_commands(Web::Painting::CanvasId canvas_id, ByteBuffer const& commands, Vector<Gfx::DecodedImageFrame> const& bitmaps)
{
    if (!can_send_message_to_compositor())
        return;

    auto encoded_message = MUST(Messages::CompositorWebContentServer::WebglCommands::static_encode(canvas_id, commands, bitmaps));
    if (post_message(encoded_message).is_error())
        did_lose_compositor();
}

void CompositorConnection::present_webgl_canvas(Web::Painting::CanvasId canvas_id, bool preserve_drawing_buffer)
{
    if (!can_send_message_to_compositor())
        return;

    async_webgl_present_canvas(canvas_id, preserve_drawing_buffer);
}

ByteBuffer CompositorConnection::webgl_sync_call(Web::Painting::CanvasId canvas_id, ByteBuffer request)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync<Messages::CompositorWebContentServer::WebglSyncCall>(canvas_id, move(request));
    return response->take_reply();
}

Web::WebGL::ReadPixelsResult CompositorConnection::read_webgl_pixels(Web::Painting::CanvasId canvas_id, Web::WebGL::GLint x, Web::WebGL::GLint y, Web::WebGL::GLsizei width, Web::WebGL::GLsizei height, Web::WebGL::GLenum format, Web::WebGL::GLenum type, Web::WebGL::GLsizei buf_size, Core::AnonymousBuffer const& pixels)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync<Messages::CompositorWebContentServer::WebglReadPixels>(canvas_id, x, y, width, height, format, type, buf_size, pixels);
    return {
        .length = response->length(),
        .columns = response->columns(),
        .rows = response->rows(),
    };
}

void CompositorConnection::read_webgl_buffer_sub_data(Web::Painting::CanvasId canvas_id, Web::WebGL::GLenum target, Web::WebGL::GLintptr offset, Web::WebGL::GLintptr size, Core::AnonymousBuffer const& data)
{
    if (!can_send_message_to_compositor())
        return;

    (void)send_sync<Messages::CompositorWebContentServer::WebglReadBufferSubData>(canvas_id, target, offset, size, data);
}

void CompositorConnection::request_screenshot(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback)
{
    if (!can_send_message_to_compositor()) {
        if (callback)
            callback();
        return;
    }

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
    if (!pending_screenshot.has_value())
        return;

    pending_screenshot->target_surface->write_from_bitmap(*pending_screenshot->target_bitmap);
    if (pending_screenshot->callback)
        pending_screenshot->callback();
}

void CompositorConnection::did_fail_screenshot(Web::Compositor::ScreenshotRequestId request_id)
{
    auto pending_screenshot = take_screenshot(request_id);
    if (!pending_screenshot.has_value())
        return;

    if (pending_screenshot->callback)
        pending_screenshot->callback();
}

void CompositorConnection::did_lose_compositor()
{
    if (m_has_lost_compositor)
        return;
    m_has_lost_compositor = true;

    for (auto& entry : m_screenshots) {
        if (entry.value.callback)
            entry.value.callback();
    }
    m_screenshots.clear();

    if (on_compositor_lost)
        on_compositor_lost();
}

bool CompositorConnection::can_send_message_to_compositor() const
{
    return !m_has_lost_compositor && is_open();
}

Optional<CompositorConnection::PendingScreenshot> CompositorConnection::take_screenshot(Web::Compositor::ScreenshotRequestId request_id)
{
    return m_screenshots.take(request_id);
}

}
