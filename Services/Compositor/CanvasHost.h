/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>

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
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Types.h>
#include <LibCompositing/WebGL/Types.h>
#include <LibCompositing/WebGL/WebGLSharedCommandBuffer.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ShareableBitmap.h>

namespace Compositing {

class CanvasSurfaceRegistry;

}

namespace Compositor {

class HostWebGLContext;

class CanvasHost {
public:
    struct CreateWebGLContextResult {
        bool success { false };
        Compositing::CanvasId canvas_id { 0 };
        Vector<String> supported_extensions;
    };

    CanvasHost(RefPtr<Gfx::SkiaBackendContext>, Compositing::CanvasSurfaceRegistry&);
    ~CanvasHost();

    Optional<Compositing::CanvasId> create_2d_context(Gfx::IntSize, bool alpha);
    CreateWebGLContextResult create_webgl_context(Compositing::WebGL::WebGLVersion, Gfx::IntSize, bool depth, bool stencil, bool antialias);
    void destroy_context(Compositing::CanvasId);
    bool has_context(Compositing::CanvasId) const;

    void execute_canvas_2d_stream(Vector<Compositing::Canvas2DCommandStreamSegment> const&, Vector<Compositing::DisplayListFontResource> const&);
    void execute_webgl_commands(Compositing::CanvasId, ReadonlyBytes, Vector<Gfx::DecodedImageFrame> const&);
    void set_webgl_shared_command_buffer(Compositing::CanvasId, Compositing::WebGL::WebGLSharedCommandBuffer);
    [[nodiscard]] bool execute_webgl_commands_from_shared_buffer(Compositing::CanvasId, u64 offset, u64 size_in_bytes, u64 flush_sequence_number, Vector<Gfx::DecodedImageFrame> const&);
    ErrorOr<ByteBuffer> execute_webgl_sync_call(Compositing::CanvasId, ByteBuffer request);
    Compositing::WebGL::ReadPixelsResult webgl_read_pixels_robust_angle(Compositing::CanvasId, Compositing::WebGL::GLint x, Compositing::WebGL::GLint y, Compositing::WebGL::GLsizei width, Compositing::WebGL::GLsizei height, Compositing::WebGL::GLenum format, Compositing::WebGL::GLenum type, Compositing::WebGL::GLsizei buf_size, Core::AnonymousBuffer pixels);
    bool webgl_read_buffer_sub_data(Compositing::CanvasId, Compositing::WebGL::GLenum target, Compositing::WebGL::GLintptr offset, Compositing::WebGL::GLintptr size, Core::AnonymousBuffer data);

    void present_webgl_canvas(Compositing::CanvasId, bool preserve_drawing_buffer);
    Gfx::ShareableBitmap read_back_pixels(Compositing::CanvasId, Gfx::IntRect);

private:
    struct Canvas2DContext {
        NonnullOwnPtr<Gfx::CanvasCommandPlayer> command_player;
        NonnullRefPtr<Gfx::PaintingSurface> presented_surface;
        bool has_uncommitted_commands { false };
    };
    using WebGLContext = NonnullOwnPtr<HostWebGLContext>;
    using Context = Variant<Canvas2DContext, WebGLContext>;

    Context* context(Compositing::CanvasId);
    OwnPtr<Gfx::CanvasCommandPlayer> create_2d_command_player(Gfx::IntSize, bool alpha);
    static HostWebGLContext& as_webgl(Context&);
    void present_canvas_2d_context(Compositing::CanvasId, Canvas2DContext&);

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    Compositing::CanvasSurfaceRegistry& m_canvas_surface_registry;
    HashMap<Compositing::CanvasId, Context> m_contexts;
    Compositing::DisplayListResourceStorage m_text_resources;
};

}
