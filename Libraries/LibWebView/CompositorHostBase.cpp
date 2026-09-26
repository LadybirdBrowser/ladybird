/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibCompositing/DisplayList/Canvas2DCommandStream.h>
#include <LibGfx/CanvasCommandList.h>
#include <LibGfx/PaintingSurface.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/HTML/Canvas/RemoteCanvas2DTransport.h>
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

void CompositorHostBase::send_canvas_2d_stream(Compositing::Canvas2DCommandStream& stream)
{
    if (auto* connection = compositor_connection())
        connection->update_canvas_2d_stream(stream);
}

void CompositorHostBase::destroy_context(Compositing::CompositorContextId context_id)
{
    if (auto* connection = compositor_connection())
        connection->destroy_context(context_id);
    context_was_destroyed(context_id);
}

void CompositorHostBase::set_parent_context(Compositing::CompositorContextId context_id, Optional<Compositing::CompositorContextId> parent_context_id)
{
    if (auto* connection = compositor_connection())
        connection->set_parent_context(context_id, parent_context_id);
}

void CompositorHostBase::stop_presenting_to_client(Compositing::CompositorContextId context_id)
{
    if (auto* connection = compositor_connection())
        connection->stop_presenting_to_client(context_id);
}

void CompositorHostBase::update_display_list(Compositing::CompositorContextId context_id, NonnullRefPtr<Compositing::DisplayList> display_list, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction&& resource_transaction, Compositing::ScrollStateSnapshot&& scroll_state_snapshot)
{
    if (auto* connection = compositor_connection())
        connection->update_display_list(context_id, display_list, visual_context_tree, resource_transaction, scroll_state_snapshot);
}

void CompositorHostBase::update_visual_context_tree(Compositing::CompositorContextId context_id, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction&& resource_transaction)
{
    if (auto* connection = compositor_connection())
        connection->update_visual_context_tree(context_id, visual_context_tree, move(resource_transaction));
}

void CompositorHostBase::add_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    if (auto* connection = compositor_connection())
        connection->add_video_sink(video_sink_handle);
}

void CompositorHostBase::remove_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    if (auto* connection = compositor_connection())
        connection->remove_video_sink(video_sink_handle);
}

void CompositorHostBase::set_video_sink_ticking(Media::VideoSinkHandle video_sink_handle, bool should_tick)
{
    if (auto* connection = compositor_connection())
        connection->set_video_sink_ticking(video_sink_handle, should_tick);
}

void CompositorHostBase::update_scroll_state(Compositing::CompositorContextId context_id, Compositing::ScrollStateSnapshot&& scroll_state_snapshot, Compositing::KeyboardScrollState keyboard_scroll_state)
{
    if (auto* connection = compositor_connection())
        connection->update_scroll_state(context_id, scroll_state_snapshot, keyboard_scroll_state);
}

void CompositorHostBase::invalidate_keyboard_scroll_state(Compositing::CompositorContextId context_id, u64 generation)
{
    if (auto* connection = compositor_connection())
        connection->invalidate_keyboard_scroll_state(context_id, generation);
}

void CompositorHostBase::invalidate_wheel_event_listener_state(Compositing::CompositorContextId context_id, u64 generation)
{
    if (auto* connection = compositor_connection())
        connection->invalidate_wheel_event_listener_state(context_id, generation);
}

Compositing::AsyncScrollEnqueueResult CompositorHostBase::async_scroll_by(Compositing::CompositorContextId context_id, Compositing::UniqueNodeID expected_document_id, Gfx::FloatPoint position,
    Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::WheelDeltaPrecision wheel_delta_precision, Compositing::ScrollGesturePhase scroll_gesture_phase, u32 modifiers, Compositing::AsyncScrollOperationTracking operation_tracking)
{
    if (auto* connection = compositor_connection())
        return connection->async_scroll_by(context_id, expected_document_id, position, delta_in_device_pixels, viewport_rect, wheel_delta_precision, scroll_gesture_phase, modifiers, operation_tracking);
    return {};
}

Compositing::AsyncScrollEnqueueResult CompositorHostBase::smooth_scroll_to(Compositing::CompositorContextId context_id, Compositing::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint offset_in_device_pixels, Gfx::FloatPoint main_thread_offset_in_device_pixels, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind animation_kind, Compositing::SmoothScrollInitiator initiator)
{
    if (auto* connection = compositor_connection())
        return connection->smooth_scroll_to(context_id, stable_node_id, offset_in_device_pixels, main_thread_offset_in_device_pixels, viewport_rect, animation_kind, initiator);
    return {};
}

void CompositorHostBase::cancel_smooth_scroll(Compositing::CompositorContextId context_id, Compositing::AsyncScrollNodeStableID stable_node_id)
{
    if (auto* connection = compositor_connection())
        connection->cancel_smooth_scroll(context_id, stable_node_id);
}

Compositing::PendingAsyncScrollUpdates CompositorHostBase::take_pending_async_scroll_updates(Compositing::CompositorContextId context_id, Compositing::AsyncScrollUpdateFreshness freshness)
{
    if (auto* connection = compositor_connection())
        return connection->take_pending_async_scroll_updates(context_id, freshness);
    return {};
}

void CompositorHostBase::viewport_size_updated(Compositing::CompositorContextId context_id, Gfx::IntSize viewport_size, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    if (auto* connection = compositor_connection())
        connection->viewport_size_updated(context_id, viewport_size, window_resize_in_progress);
}

bool CompositorHostBase::request_rendering_opportunity(Compositing::CompositorContextId context_id, double maximum_frames_per_second)
{
    if (auto* connection = compositor_connection())
        return connection->request_rendering_opportunity(context_id, maximum_frames_per_second);
    return false;
}

void CompositorHostBase::hurry_rendering_opportunity(Compositing::CompositorContextId context_id)
{
    if (auto* connection = compositor_connection())
        connection->hurry_rendering_opportunity(context_id);
}

void CompositorHostBase::present_frame(Compositing::CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    if (auto* connection = compositor_connection())
        connection->present_frame(context_id, viewport_rect);
}

void CompositorHostBase::request_screenshot(Compositing::CompositorContextId context_id, NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback)
{
    if (auto* connection = compositor_connection()) {
        connection->request_screenshot(context_id, move(target_surface), move(callback));
        return;
    }
    if (callback)
        callback();
}

}
