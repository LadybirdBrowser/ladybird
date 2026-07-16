/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibGfx/CanvasCommandList.h>
#include <LibGfx/PaintingSurface.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/HTML/Canvas/RemoteCanvas2DTransport.h>
#include <LibWeb/Painting/Canvas2DCommandStream.h>
#include <LibWeb/WebGL/RemoteWebGLTransport.h>
#include <LibWebView/CompositorConnection.h>
#include <LibWebView/CompositorHostBase.h>

namespace WebView {

class CompositorRemoteWebGLTransport final : public Web::WebGL::RemoteWebGLTransport {
public:
    explicit CompositorRemoteWebGLTransport(NonnullRefPtr<CompositorConnection> connection)
        : m_connection(move(connection))
    {
    }

private:
    virtual CreateResult create_context(Web::WebGL::WebGLVersion webgl_version, Gfx::IntSize initial_size, bool depth, bool stencil, bool antialias) override
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

    virtual Optional<Web::Painting::CanvasId> canvas_id() const override
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

    virtual ByteBuffer sync_call(ByteBuffer request) override
    {
        if (!m_canvas_id.has_value())
            return {};
        return m_connection->webgl_sync_call(*m_canvas_id, move(request));
    }

    virtual Web::WebGL::ReadPixelsResult read_pixels_robust_angle(Web::WebGL::GLint x, Web::WebGL::GLint y, Web::WebGL::GLsizei width, Web::WebGL::GLsizei height, Web::WebGL::GLenum format, Web::WebGL::GLenum type, Web::WebGL::GLsizei buf_size, Core::AnonymousBuffer pixels) override
    {
        if (!m_canvas_id.has_value())
            return {};
        return m_connection->read_webgl_pixels(*m_canvas_id, x, y, width, height, format, type, buf_size, pixels);
    }

    virtual bool read_buffer_sub_data(Web::WebGL::GLenum target, Web::WebGL::GLintptr offset, Web::WebGL::GLintptr size, Core::AnonymousBuffer data) override
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
    Optional<Web::Painting::CanvasId> m_canvas_id;
};

class CompositorRemoteCanvas2DTransport final : public Web::HTML::RemoteCanvas2DTransport {
public:
    CompositorRemoteCanvas2DTransport(NonnullRefPtr<CompositorConnection> connection, NonnullRefPtr<Web::Painting::Canvas2DCommandStream> stream)
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

    virtual Optional<Web::Painting::CanvasId> canvas_id() const override
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

    virtual Web::Painting::Canvas2DCommandStream& shared_stream() override
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
    NonnullRefPtr<Web::Painting::Canvas2DCommandStream> m_stream;
    Optional<Web::Painting::CanvasId> m_canvas_id;
};

RefPtr<Web::WebGL::RemoteWebGLTransport> CompositorHostBase::create_webgl_transport()
{
    if (auto* connection = compositor_connection())
        return adopt_ref(*new CompositorRemoteWebGLTransport(*connection));
    return nullptr;
}

RefPtr<Web::HTML::RemoteCanvas2DTransport> CompositorHostBase::create_canvas_2d_transport()
{
    if (auto* connection = compositor_connection())
        return adopt_ref(*new CompositorRemoteCanvas2DTransport(*connection, canvas_2d_stream()));
    return nullptr;
}

void CompositorHostBase::send_canvas_2d_stream(Web::Painting::Canvas2DCommandStream& stream)
{
    if (auto* connection = compositor_connection())
        connection->update_canvas_2d_stream(stream);
}

void CompositorHostBase::destroy_context(Web::Compositor::CompositorContextId context_id)
{
    if (auto* connection = compositor_connection())
        connection->destroy_context(context_id);
    context_was_destroyed(context_id);
}

void CompositorHostBase::set_parent_context(Web::Compositor::CompositorContextId context_id, Optional<Web::Compositor::CompositorContextId> parent_context_id)
{
    if (auto* connection = compositor_connection())
        connection->set_parent_context(context_id, parent_context_id);
}

void CompositorHostBase::stop_presenting_to_client(Web::Compositor::CompositorContextId context_id)
{
    if (auto* connection = compositor_connection())
        connection->stop_presenting_to_client(context_id);
}

void CompositorHostBase::update_display_list(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Web::Painting::DisplayList> display_list, Web::Painting::AccumulatedVisualContextTree visual_context_tree, Web::Painting::DisplayListResourceTransaction&& resource_transaction, Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    if (auto* connection = compositor_connection())
        connection->update_display_list(context_id, display_list, visual_context_tree, resource_transaction, scroll_state_snapshot);
}

void CompositorHostBase::update_visual_context_tree(Web::Compositor::CompositorContextId context_id, Web::Painting::AccumulatedVisualContextTree visual_context_tree)
{
    if (auto* connection = compositor_connection())
        connection->update_visual_context_tree(context_id, visual_context_tree);
}

void CompositorHostBase::update_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> frame)
{
    if (auto* connection = compositor_connection())
        connection->update_video_frame(context_id, frame_id, frame);
}

void CompositorHostBase::clear_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id)
{
    if (auto* connection = compositor_connection())
        connection->clear_video_frame(context_id, frame_id);
}

void CompositorHostBase::update_scroll_state(Web::Compositor::CompositorContextId context_id, Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    if (auto* connection = compositor_connection())
        connection->update_scroll_state(context_id, scroll_state_snapshot);
}

void CompositorHostBase::invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId context_id, u64 generation)
{
    if (auto* connection = compositor_connection())
        connection->invalidate_wheel_event_listener_state(context_id, generation);
}

Web::Compositor::AsyncScrollEnqueueResult CompositorHostBase::async_scroll_by(Web::Compositor::CompositorContextId context_id, Web::UniqueNodeID expected_document_id, Gfx::FloatPoint position,
    Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking operation_tracking)
{
    if (auto* connection = compositor_connection())
        return connection->async_scroll_by(context_id, expected_document_id, position, delta_in_device_pixels, viewport_rect, operation_tracking);
    return {};
}

Web::Compositor::PendingAsyncScrollUpdates CompositorHostBase::take_pending_async_scroll_updates(Web::Compositor::CompositorContextId context_id)
{
    if (auto* connection = compositor_connection())
        return connection->take_pending_async_scroll_updates(context_id);
    return {};
}

void CompositorHostBase::viewport_size_updated(Web::Compositor::CompositorContextId context_id, Gfx::IntSize viewport_size, Web::Compositor::WindowResizingInProgress window_resize_in_progress)
{
    if (auto* connection = compositor_connection())
        connection->viewport_size_updated(context_id, viewport_size, window_resize_in_progress);
}

void CompositorHostBase::present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect viewport_rect, Gfx::IntRect damage_rect)
{
    if (auto* connection = compositor_connection())
        connection->present_frame(context_id, viewport_rect, damage_rect);
}

void CompositorHostBase::request_screenshot(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback)
{
    if (auto* connection = compositor_connection()) {
        connection->request_screenshot(context_id, move(target_surface), move(callback));
        return;
    }
    if (callback)
        callback();
}

}
