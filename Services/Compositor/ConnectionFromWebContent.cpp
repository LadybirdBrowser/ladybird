/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ConnectionFromWebContent.h>
#include <LibCore/System.h>
#include <LibWeb/Page/InputEvent.h>

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

void ConnectionFromWebContent::notify_compositor_lost()
{
    async_did_lose_compositor();
}

Messages::CompositorWebContentServer::InitTransportResponse ConnectionFromWebContent::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#endif
    VERIFY_NOT_REACHED();
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

void ConnectionFromWebContent::set_parent_context(Web::Compositor::CompositorContextId context_id, Optional<Web::Compositor::CompositorContextId> parent_context_id)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->set_parent_context(context_id, parent_context_id);
}

void ConnectionFromWebContent::stop_presenting_to_client(Web::Compositor::CompositorContextId context_id)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->stop_presenting_to_client(context_id);
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

void ConnectionFromWebContent::update_image_frame_resources(Web::Compositor::CompositorContextId context_id, Vector<Web::Painting::DisplayListImageFrameResource> image_frames)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->update_image_frame_resources(context_id, move(image_frames));
}

void ConnectionFromWebContent::update_visual_context_tree(Web::Compositor::CompositorContextId context_id, Web::Painting::AccumulatedVisualContextTree visual_context_tree)
{
    verify_context_is_owned_by_this_connection(context_id);
    m_compositor_state->update_visual_context_tree(context_id, move(visual_context_tree));
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

Messages::CompositorWebContentServer::CreateCanvas2dContextResponse ConnectionFromWebContent::create_canvas_2d_context(Gfx::IntSize size, bool alpha)
{
    auto canvas_id = m_canvas_host.create_2d_context(size, alpha);
    if (!canvas_id.has_value())
        return { false, Web::Painting::CanvasId { 0 } };
    return { true, *canvas_id };
}

void ConnectionFromWebContent::update_canvas_2d_commands(Web::Painting::CanvasId canvas_id, Gfx::CanvasCommandList commands, bool commit)
{
    m_canvas_host.execute_canvas_2d_commands(canvas_id, commands, commit);
}

void ConnectionFromWebContent::destroy_canvas_context(Web::Painting::CanvasId canvas_id)
{
    m_canvas_host.destroy_context(canvas_id);
}

Messages::CompositorWebContentServer::GetCanvasPixelsResponse ConnectionFromWebContent::get_canvas_pixels(Web::Painting::CanvasId canvas_id, Gfx::IntRect rect)
{
    return m_canvas_host.read_back_pixels(canvas_id, rect);
}

Messages::CompositorWebContentServer::CreateWebglContextResponse ConnectionFromWebContent::create_webgl_context(Web::WebGL::WebGLVersion webgl_version, Gfx::IntSize size, bool depth, bool stencil, bool antialias)
{
    auto result = m_canvas_host.create_webgl_context(webgl_version, size, depth, stencil, antialias);
    return { result.success, result.canvas_id, move(result.supported_extensions) };
}

void ConnectionFromWebContent::webgl_commands(Web::Painting::CanvasId canvas_id, Core::AnonymousBuffer commands, Vector<Gfx::DecodedImageFrame> bitmaps)
{
    if (!commands.is_valid()) {
        did_misbehave("WebContent sent an invalid WebGL command buffer");
        return;
    }

    m_canvas_host.execute_webgl_commands(canvas_id, commands.bytes(), bitmaps);
}

void ConnectionFromWebContent::webgl_present_canvas(Web::Painting::CanvasId canvas_id, bool preserve_drawing_buffer)
{
    m_canvas_host.present_webgl_canvas(canvas_id, preserve_drawing_buffer);
}

Messages::CompositorWebContentServer::WebglSyncCallResponse ConnectionFromWebContent::webgl_sync_call(Web::Painting::CanvasId canvas_id, ByteBuffer request)
{
    return MUST(m_canvas_host.execute_webgl_sync_call(canvas_id, move(request)));
}

Messages::CompositorWebContentServer::WebglReadPixelsResponse ConnectionFromWebContent::webgl_read_pixels(Web::Painting::CanvasId canvas_id, i32 x, i32 y, i32 width, i32 height, u32 format, u32 type, i32 buf_size, Core::AnonymousBuffer pixels)
{
    if (buf_size < 0 || (buf_size > 0 && (!pixels.is_valid() || pixels.size() < static_cast<size_t>(buf_size)))) {
        did_misbehave("WebContent sent an invalid WebGL readPixels buffer");
        return { 0, 0, 0 };
    }

    auto result = m_canvas_host.webgl_read_pixels_robust_angle(canvas_id, x, y, width, height, format, type, buf_size, move(pixels));
    return { result.length, result.columns, result.rows };
}

Messages::CompositorWebContentServer::WebglReadBufferSubDataResponse ConnectionFromWebContent::webgl_read_buffer_sub_data(Web::Painting::CanvasId canvas_id, u32 target, i64 offset, i64 size, Core::AnonymousBuffer data)
{
    if (size < 0 || (size > 0 && (!data.is_valid() || data.size() < static_cast<size_t>(size)))) {
        did_misbehave("WebContent sent an invalid WebGL buffer readback target");
        return { false };
    }

    return { m_canvas_host.webgl_read_buffer_sub_data(canvas_id, target, offset, size, move(data)) };
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
