/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::Painting {

class CanvasSurfaceRegistry;

}

namespace Compositor {

class HostWebGLContext;

class CanvasHost {
public:
    struct CreateWebGLContextResult {
        bool success { false };
        Web::Painting::CanvasId canvas_id { 0 };
        Vector<String> supported_extensions;
    };

    CanvasHost(RefPtr<Gfx::SkiaBackendContext>, Web::Painting::CanvasSurfaceRegistry&);
    ~CanvasHost();

    Optional<Web::Painting::CanvasId> create_2d_context(Gfx::IntSize, bool alpha);
    CreateWebGLContextResult create_webgl_context(Web::WebGL::WebGLVersion, Gfx::IntSize, bool depth, bool stencil, bool antialias);
    void destroy_context(Web::Painting::CanvasId);
    bool has_context(Web::Painting::CanvasId) const;

    void execute_canvas_2d_commands(Web::Painting::CanvasId, Gfx::CanvasCommandList const&, bool commit);
    void execute_webgl_commands(Web::Painting::CanvasId, ReadonlyBytes, Vector<Gfx::DecodedImageFrame> const&);
    ErrorOr<ByteBuffer> execute_webgl_sync_call(Web::Painting::CanvasId, ByteBuffer request);
    Web::WebGL::ReadPixelsResult webgl_read_pixels_robust_angle(Web::Painting::CanvasId, Web::WebGL::GLint x, Web::WebGL::GLint y, Web::WebGL::GLsizei width, Web::WebGL::GLsizei height, Web::WebGL::GLenum format, Web::WebGL::GLenum type, Web::WebGL::GLsizei buf_size, Core::AnonymousBuffer pixels);
    void webgl_read_buffer_sub_data(Web::Painting::CanvasId, Web::WebGL::GLenum target, Web::WebGL::GLintptr offset, Web::WebGL::GLintptr size, Core::AnonymousBuffer data);

    void present_webgl_canvas(Web::Painting::CanvasId, bool preserve_drawing_buffer);
    Gfx::ShareableBitmap read_back_pixels(Web::Painting::CanvasId, Gfx::IntRect);

private:
    struct Canvas2DContext {
        NonnullOwnPtr<Gfx::CanvasCommandPlayer> command_player;
        NonnullRefPtr<Gfx::PaintingSurface> presented_surface;
        bool has_uncommitted_commands { false };
    };
    using WebGLContext = NonnullOwnPtr<HostWebGLContext>;
    using Context = Variant<Canvas2DContext, WebGLContext>;

    Context* context(Web::Painting::CanvasId);
    OwnPtr<Gfx::CanvasCommandPlayer> create_2d_command_player(Gfx::IntSize, bool alpha);
    static Canvas2DContext& as_2d(Context&);
    static HostWebGLContext& as_webgl(Context&);
    void present_canvas_2d_context(Web::Painting::CanvasId, Canvas2DContext&);

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    Web::Painting::CanvasSurfaceRegistry& m_canvas_surface_registry;
    HashMap<Web::Painting::CanvasId, Context> m_contexts;
};

}
