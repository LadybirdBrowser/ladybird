/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>
#include <LibWeb/WebGL/RemoteWebGLTransport.h>
#include <LibWeb/WebGL/WebGLCommandList.h>

namespace Web::WebGL {

class WEB_API WebGLContextProxyBase {
    AK_MAKE_NONCOPYABLE(WebGLContextProxyBase);
    AK_MAKE_NONMOVABLE(WebGLContextProxyBase);

public:
    WebGLContextProxyBase(NonnullRefPtr<RemoteWebGLTransport>, WebGLVersion, Vector<String> supported_extensions);
    ~WebGLContextProxyBase();

    void flush_commands();
    void set_lost() { m_lost = true; }
    Optional<Painting::CanvasId> canvas_id() const { return m_transport->canvas_id(); }

    void restore(NonnullRefPtr<RemoteWebGLTransport>, Vector<String> supported_extensions);

    void make_current() { }
    void notify_content_will_change() { }
    u32 default_framebuffer() const { return 0; }
    u32 default_renderbuffer() const { return 0; }
    WebGLVersion webgl_version() const { return m_webgl_version; }
    Vector<String> const& get_supported_opengl_extensions() const { return m_supported_extensions; }
    void set_size(Gfx::IntSize const&);

    void present_canvas_for_compositing(bool preserve_drawing_buffer);

    RefPtr<Gfx::Bitmap> read_back_drawing_buffer(Gfx::IntRect const&);

    void read_pixels_into_pixel_pack_buffer(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, long long offset);
    bool read_buffer_sub_data(GLenum target, long long offset, Bytes destination);

    void tex_image2d_from_bitmap(GLenum target, GLint level, GLint internalformat, GLenum format, GLenum type, Gfx::DecodedImageFrame, Optional<Gfx::IntSize> destination_size, bool flip_y, bool premultiply_alpha);
    void tex_sub_image2d_from_bitmap(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLenum format, GLenum type, Gfx::DecodedImageFrame, Optional<Gfx::IntSize> destination_size, bool flip_y, bool premultiply_alpha);
    void tex_image3d_from_bitmap(GLenum target, GLint level, GLint internalformat, GLsizei depth, GLenum format, GLenum type, Gfx::DecodedImageFrame, Optional<Gfx::IntSize> destination_size, bool flip_y, bool premultiply_alpha);
    void tex_sub_image3d_from_bitmap(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei depth, GLenum format, GLenum type, Gfx::DecodedImageFrame, Optional<Gfx::IntSize> destination_size, bool flip_y, bool premultiply_alpha);

    GLenum take_pending_local_error()
    {
        auto error = m_pending_local_error;
        m_pending_local_error = 0;
        return error;
    }

protected:
    static constexpr size_t max_pending_command_bytes = 4 * MiB;

    WebGLObjectId allocate_object_id() { return m_next_object_id++; }

    void set_pending_local_error(GLenum error)
    {
        if (m_pending_local_error == 0)
            m_pending_local_error = error;
    }
    ReadPixelsResult read_pixels_robust_angle_into_shared_buffer(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei buf_size, Core::AnonymousBuffer const& pixels);

    template<typename Command>
    void record(Command const& command, ReadonlyBytes inline_data = {})
    {
        if (m_lost)
            return;
        m_commands.append(command, inline_data);
        if (m_commands.size_in_bytes() >= max_pending_command_bytes)
            flush_commands();
    }

    ByteBuffer send_sync_call(ByteBuffer request);
    bool is_lost() const { return m_lost; }

    HashMap<GLenum, NonnullOwnPtr<ByteBuffer>> m_string_cache;

private:
    u32 append_pending_bitmap(Gfx::DecodedImageFrame);

    NonnullRefPtr<RemoteWebGLTransport> m_transport;
    WebGLVersion m_webgl_version { WebGLVersion::WebGL1 };
    Vector<String> m_supported_extensions;
    WebGLCommandList m_commands;
    Vector<Gfx::DecodedImageFrame> m_pending_bitmaps;
    u32 m_next_object_id { 1 };
    bool m_lost { false };
    GLenum m_pending_local_error { 0 };
};

}
