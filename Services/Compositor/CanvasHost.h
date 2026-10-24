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
#include <Compositor/Forward.h>
#include <Compositor/HostWebGLContext.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Types.h>
#include <LibCompositing/WebGL/ImageSourceFrame.h>
#include <LibCompositing/WebGL/Types.h>
#include <LibCompositing/WebGL/WebGLSharedCommandBuffer.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibMediaCodecs/Forward.h>

class SkImage;

template<typename T>
class sk_sp;

namespace Compositing {

class CanvasSurfaceRegistry;

}

namespace Compositor {

class Canvas2DCommandPlayer;
class VideoFrameHolds;

class CanvasHost {
public:
    struct CreateWebGLContextResult {
        bool success { false };
        Compositing::CanvasId canvas_id { 0 };
        Vector<String> supported_extensions;
    };

    CanvasHost(RefPtr<Gfx::SkiaBackendContext>, CanvasSurfaceRegistry&, VideoFrameHolds const&);
    ~CanvasHost();

    bool create_2d_context(Compositing::CanvasId, Gfx::IntSize, bool alpha, bool will_read_frequently);
    CreateWebGLContextResult create_webgl_context(Compositing::WebGL::WebGLVersion, Gfx::IntSize, bool depth, bool stencil, bool antialias);
    void destroy_context(Compositing::CanvasId);

    void execute_canvas_2d_stream(Vector<Compositing::Canvas2DCommandStreamSegment> const&, Vector<Compositing::FontResourceId> const& kept_font_ids, Vector<Compositing::DisplayListFontResource> const& added_fonts, Vector<Compositing::DisplayListImageFrameResource> const& image_frames);
    void execute_webgl_commands(Compositing::CanvasId, ReadonlyBytes, Vector<Compositing::WebGL::ImageSourceFrame> const&);
    void set_webgl_shared_command_buffer(Compositing::CanvasId, Compositing::WebGL::WebGLSharedCommandBuffer);
    [[nodiscard]] bool execute_webgl_commands_from_shared_buffer(Compositing::CanvasId, u64 offset, u64 size_in_bytes, u64 flush_sequence_number, Vector<Compositing::WebGL::ImageSourceFrame> const&);
    ErrorOr<ByteBuffer> execute_webgl_sync_call(Compositing::CanvasId, ByteBuffer request);
    Compositing::WebGL::ReadPixelsResult webgl_read_pixels_robust_angle(Compositing::CanvasId, Compositing::WebGL::GLint x, Compositing::WebGL::GLint y, Compositing::WebGL::GLsizei width, Compositing::WebGL::GLsizei height, Compositing::WebGL::GLenum format, Compositing::WebGL::GLenum type, Compositing::WebGL::GLsizei buf_size, Core::AnonymousBuffer pixels);
    bool webgl_read_buffer_sub_data(Compositing::CanvasId, Compositing::WebGL::GLenum target, Compositing::WebGL::GLintptr offset, Compositing::WebGL::GLintptr size, Core::AnonymousBuffer data);

    Compositing::CanvasSurfaceOwnerId canvas_surface_owner_id() const { return m_canvas_surface_owner_id; }

    void present_webgl_canvas(Compositing::CanvasId, bool preserve_drawing_buffer);
    void clear_webgl_drawing_buffer(Compositing::CanvasId);
    Gfx::ShareableBitmap read_back_pixels(Compositing::CanvasId, Gfx::IntRect, Gfx::AlphaType);
    RefPtr<Gfx::PaintingSurface> presented_surface(Compositing::CanvasId);

    void forget_video_sink(Media::VideoSinkHandle);

    static Gfx::ShareableBitmap read_back_surface(Gfx::PaintingSurface&, Gfx::IntRect, Gfx::AlphaType = Gfx::AlphaType::Premultiplied);

private:
    struct Canvas2DContext {
        NonnullOwnPtr<Canvas2DCommandPlayer> command_player;
        NonnullRefPtr<Gfx::PaintingSurface> presented_surface;
        bool has_uncommitted_commands { false };
    };
    using WebGLContext = NonnullOwnPtr<HostWebGLContext>;
    using Context = Variant<Canvas2DContext, WebGLContext>;

    Context* context(Compositing::CanvasId);
    OwnPtr<Canvas2DCommandPlayer> create_2d_command_player(Gfx::IntSize, bool alpha, bool will_read_frequently);
    static HostWebGLContext& as_webgl(Context&);
    void present_canvas_2d_context(Compositing::CanvasId, Canvas2DContext&);
    sk_sp<SkImage> held_video_frame_image(Media::VideoSinkHandle, u64 generation, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context_of_canvas);
    Vector<HostWebGLContext::UploadSource> resolve_webgl_upload_sources(Vector<Compositing::WebGL::ImageSourceFrame> const&) const;

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    CanvasSurfaceRegistry& m_canvas_surface_registry;
    Compositing::CanvasSurfaceOwnerId m_canvas_surface_owner_id;
    HashMap<Compositing::CanvasId, Context> m_contexts;
    Compositing::DisplayListResourceStorage m_text_resources;
    // The image frames of the stream batch that plays.
    HashMap<u64, Gfx::DecodedImageFrame> m_image_frames_of_batch;

    VideoFrameHolds const& m_video_frame_holds;
    struct VideoFrameImage;
    HashMap<Media::VideoSinkHandle, NonnullOwnPtr<VideoFrameImage>> m_video_frame_images;
    bool m_drew_video_frame_in_segment { false };
};

}
