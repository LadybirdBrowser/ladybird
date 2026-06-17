/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <Compositor/OpenGLContext.h>
#include <Compositor/WebGLObjectMap.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL::Commands {

struct TexImage2DFromBitmap;
struct TexSubImage2DFromBitmap;

}

namespace Compositor {

class HostWebGLContext {
public:
    static OwnPtr<HostWebGLContext> create(NonnullRefPtr<Gfx::SkiaBackendContext>, OpenGLContext::WebGLVersion, OpenGLContext::DrawingBufferOptions, Gfx::IntSize initial_size);

    ErrorOr<void> execute_commands(ReadonlyBytes, Vector<Gfx::DecodedImageFrame> const& bitmaps);
    ErrorOr<ByteBuffer> execute_sync_call(ReadonlyBytes request);
    Gfx::ShareableBitmap read_back_drawing_buffer(Gfx::IntRect);
    Web::WebGL::ReadPixelsResult read_pixels_robust_angle(Web::WebGL::GLint x, Web::WebGL::GLint y, Web::WebGL::GLsizei width, Web::WebGL::GLsizei height, Web::WebGL::GLenum format, Web::WebGL::GLenum type, Web::WebGL::GLsizei buf_size, Core::AnonymousBuffer pixels);
    void read_buffer_sub_data(Web::WebGL::GLenum target, Web::WebGL::GLintptr offset, Web::WebGL::GLintptr size, Core::AnonymousBuffer data);
    ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> prepare_for_compositing(bool preserve_drawing_buffer);
    RefPtr<Gfx::PaintingSurface> surface();

    OpenGLContext& gl_context() { return *m_gl_context; }

private:
    explicit HostWebGLContext(NonnullOwnPtr<OpenGLContext>);

    ErrorOr<void> set_drawing_buffer_size(int width, int height);
    ErrorOr<void> tex_image2d_from_bitmap(Web::WebGL::Commands::TexImage2DFromBitmap const&, Vector<Gfx::DecodedImageFrame> const& bitmaps);
    ErrorOr<void> tex_sub_image2d_from_bitmap(Web::WebGL::Commands::TexSubImage2DFromBitmap const&, Vector<Gfx::DecodedImageFrame> const& bitmaps);

    NonnullOwnPtr<OpenGLContext> m_gl_context;
    WebGLObjectMap m_objects;
    bool m_needs_clear_before_next_frame { false };
};

}
