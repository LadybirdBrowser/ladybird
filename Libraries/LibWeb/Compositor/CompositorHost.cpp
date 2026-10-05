/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibCompositing/DisplayList/Canvas2DCommandStream.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibGfx/CanvasCommandList.h>
#include <LibGfx/PaintingSurface.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/Compositor/CompositorConnection.h>
#include <LibWeb/Compositor/CompositorFrame.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/HTML/Canvas/RemoteCanvas2DTransport.h>
#include <LibWeb/WebGL/RemoteWebGLTransport.h>

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

class CompositorRemoteWebGLTransport final : public Web::WebGL::RemoteWebGLTransport {
public:
    explicit CompositorRemoteWebGLTransport(NonnullRefPtr<CompositorConnection> connection)
        : m_connection(move(connection))
    {
    }

private:
    virtual CreateResult create_context(Compositing::WebGL::WebGLVersion webgl_version, Gfx::IntSize initial_size, bool depth, bool stencil, bool antialias) override
    {
        VERIFY(!m_canvas_id.has_value());
        CreateResult result;
        auto canvas_id = m_connection->create_webgl_context(webgl_version, initial_size, depth, stencil, antialias, result.supported_extensions);
        if (canvas_id.has_value()) {
            result.success = true;
            m_canvas_id = *canvas_id;
        }
        return result;
    }

    virtual Optional<Compositing::CanvasId> canvas_id() const override
    {
        return m_canvas_id;
    }

    virtual void destroy_context() override
    {
        if (!m_canvas_id.has_value())
            return;
        m_connection->destroy_canvas_context(*m_canvas_id);
        m_canvas_id.clear();
    }

    virtual void set_shared_command_buffer(Core::AnonymousBuffer const& command_buffer) override
    {
        if (!m_canvas_id.has_value())
            return;
        m_connection->set_webgl_command_buffer(*m_canvas_id, command_buffer);
    }

    virtual void send_commands_from_shared_buffer(u64 offset, u64 size_in_bytes, u64 flush_sequence_number, Vector<Gfx::DecodedImageFrame> const& bitmaps) override
    {
        if (!m_canvas_id.has_value())
            return;
        m_connection->send_webgl_commands_from_shared_buffer(*m_canvas_id, offset, size_in_bytes, flush_sequence_number, bitmaps);
    }

    virtual bool wait_until_published_commands_executed() override
    {
        if (!m_canvas_id.has_value())
            return false;
        return m_connection->drain_webgl_command_buffer(*m_canvas_id);
    }

    virtual void send_commands(ByteBuffer const& commands, Vector<Gfx::DecodedImageFrame> const& bitmaps) override
    {
        if (!m_canvas_id.has_value())
            return;
        m_connection->send_webgl_commands(*m_canvas_id, commands, bitmaps);
    }

    virtual void present_canvas(bool preserve_drawing_buffer) override
    {
        if (!m_canvas_id.has_value())
            return;
        m_connection->present_webgl_canvas(*m_canvas_id, preserve_drawing_buffer);
    }

    virtual void clear_drawing_buffer() override
    {
        if (!m_canvas_id.has_value())
            return;
        m_connection->clear_webgl_drawing_buffer(*m_canvas_id);
    }

    virtual ByteBuffer sync_call(ByteBuffer request) override
    {
        if (!m_canvas_id.has_value())
            return {};
        return m_connection->webgl_sync_call(*m_canvas_id, move(request));
    }

    virtual Compositing::WebGL::ReadPixelsResult read_pixels_robust_angle(Compositing::WebGL::GLint x, Compositing::WebGL::GLint y, Compositing::WebGL::GLsizei width, Compositing::WebGL::GLsizei height, Compositing::WebGL::GLenum format, Compositing::WebGL::GLenum type, Compositing::WebGL::GLsizei buf_size, Core::AnonymousBuffer pixels) override
    {
        if (!m_canvas_id.has_value())
            return {};
        return m_connection->read_webgl_pixels(*m_canvas_id, x, y, width, height, format, type, buf_size, pixels);
    }

    virtual bool read_buffer_sub_data(Compositing::WebGL::GLenum target, Compositing::WebGL::GLintptr offset, Compositing::WebGL::GLintptr size, Core::AnonymousBuffer data) override
    {
        if (!m_canvas_id.has_value())
            return false;
        return m_connection->read_webgl_buffer_sub_data(*m_canvas_id, target, offset, size, data);
    }

    virtual Gfx::ShareableBitmap read_back_drawing_buffer(Gfx::IntRect const& rect) override
    {
        if (!m_canvas_id.has_value())
            return {};
        return m_connection->get_canvas_pixels(*m_canvas_id, rect);
    }

    NonnullRefPtr<CompositorConnection> m_connection;
    Optional<Compositing::CanvasId> m_canvas_id;
};

class CompositorRemoteCanvas2DTransport final : public Web::HTML::RemoteCanvas2DTransport {
public:
    CompositorRemoteCanvas2DTransport(NonnullRefPtr<CompositorConnection> connection, NonnullRefPtr<Compositing::Canvas2DCommandStream> stream)
        : m_connection(move(connection))
        , m_stream(move(stream))
    {
    }

private:
    virtual bool create_context(Gfx::IntSize size, bool alpha) override
    {
        VERIFY(!m_canvas_id.has_value());
        auto canvas_id = m_connection->create_canvas_2d_context(size, alpha);
        if (!canvas_id.has_value())
            return false;
        m_canvas_id = *canvas_id;
        return true;
    }

    virtual Optional<Compositing::CanvasId> canvas_id() const override
    {
        return m_canvas_id;
    }

    virtual void destroy_context() override
    {
        if (!m_canvas_id.has_value())
            return;
        m_connection->destroy_canvas_context(*m_canvas_id);
        m_canvas_id.clear();
    }

    virtual Compositing::Canvas2DCommandStream& shared_stream() override
    {
        return *m_stream;
    }

    virtual void flush_shared_stream() override
    {
        m_connection->update_canvas_2d_stream(*m_stream);
    }

    virtual RefPtr<Gfx::Bitmap> read_back_pixels(Gfx::IntRect const& rect) override
    {
        if (!m_canvas_id.has_value())
            return nullptr;
        auto shareable_bitmap = m_connection->get_canvas_pixels(*m_canvas_id, rect);
        if (!shareable_bitmap.is_valid())
            return nullptr;
        return shareable_bitmap.bitmap();
    }

    NonnullRefPtr<CompositorConnection> m_connection;
    NonnullRefPtr<Compositing::Canvas2DCommandStream> m_stream;
    Optional<Compositing::CanvasId> m_canvas_id;
};

RefPtr<Web::WebGL::RemoteWebGLTransport> CompositorHost::create_webgl_transport()
{
    if (auto* connection = compositor_connection())
        return adopt_ref(*new CompositorRemoteWebGLTransport(*connection));
    return nullptr;
}

RefPtr<Web::HTML::RemoteCanvas2DTransport> CompositorHost::create_canvas_2d_transport()
{
    if (auto* connection = compositor_connection())
        return adopt_ref(*new CompositorRemoteCanvas2DTransport(*connection, canvas_2d_stream()));
    return nullptr;
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

void CompositorHost::send_canvas_2d_stream(Compositing::Canvas2DCommandStream& stream)
{
    if (auto* connection = compositor_connection())
        connection->update_canvas_2d_stream(stream);
}

void CompositorHost::destroy_context(Web::CompositorContextId context_id)
{
    if (auto* connection = compositor_connection())
        connection->destroy_context(context_id);
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

void CompositorHost::submit_frame(PresentationTurn, CompositorFrame&& frame)
{
    if (auto* connection = compositor_connection())
        connection->submit_frame(move(frame));
}

RefPtr<CompositorFrameSink> CompositorHost::frame_sink()
{
    if (auto* connection = compositor_connection())
        return connection->frame_sink();
    return nullptr;
}

void CompositorHost::add_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    if (auto* connection = compositor_connection())
        connection->add_video_sink(video_sink_handle);
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

Compositing::AsyncScrollEnqueueResult CompositorHost::async_scroll_by(Web::CompositorContextId context_id, Web::UniqueNodeID expected_document_id, Gfx::FloatPoint position,
    Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase, u32 modifiers, Compositing::AsyncScrollOperationTracking operation_tracking)
{
    if (auto* connection = compositor_connection())
        return connection->async_scroll_by(context_id, expected_document_id, position, delta_in_device_pixels, viewport_rect, wheel_delta_precision, scroll_gesture_phase, modifiers, operation_tracking);
    return {};
}

Compositing::AsyncScrollEnqueueResult CompositorHost::smooth_scroll_to(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint offset_in_device_pixels, Gfx::FloatPoint main_thread_offset_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind animation_kind, Compositing::SmoothScrollInitiator initiator)
{
    if (auto* connection = compositor_connection())
        return connection->smooth_scroll_to(context_id, stable_node_id, offset_in_device_pixels, main_thread_offset_in_device_pixels, viewport_rect, animation_kind, initiator);
    return {};
}

void CompositorHost::cancel_smooth_scroll(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stable_node_id)
{
    if (auto* connection = compositor_connection())
        connection->cancel_smooth_scroll(context_id, stable_node_id);
}

Compositing::PendingAsyncScrollUpdates CompositorHost::take_pending_async_scroll_updates(Web::CompositorContextId context_id, Compositing::AsyncScrollUpdateFreshness freshness)
{
    if (auto* connection = compositor_connection())
        return connection->take_pending_async_scroll_updates(context_id, freshness);
    return {};
}

void CompositorHost::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    if (auto* connection = compositor_connection())
        connection->viewport_size_updated(context_id, viewport_size, window_resize_in_progress);
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

void CompositorHost::request_screenshot(Web::CompositorContextId context_id, NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback)
{
    if (auto* connection = compositor_connection()) {
        connection->request_screenshot(context_id, move(target_surface), move(callback));
        return;
    }
    if (callback)
        callback();
}

}
