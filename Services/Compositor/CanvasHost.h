/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCompositing/DisplayList/DisplayListResources.h>

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
#include <Compositor/BitmapExport.h>
#include <Compositor/DrawnResources.h>
#include <Compositor/Forward.h>
#include <Compositor/OpenGLContext.h>
#include <Compositor/PresentableSurface.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Types.h>
#include <LibCompositing/WebGL/Types.h>
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
    struct HeldVideoFrame {
        Media::VideoSinkHandle sink_handle;
        u64 generation { 0 };
    };
    using WebGLImageSource = Variant<Compositing::ImageFrameResourceId, HeldVideoFrame>;

    enum class WebGLPresentOutcome {
        Presented,
        ShowedPresentedFrame,
        NoDrawingBuffer,
    };

    enum class WebGLImageConversion {
        Converted,
        UnknownImageFrame,
        Failed,
    };

    CanvasHost(RefPtr<Gfx::SkiaBackendContext>, CanvasSurfaceRegistry&, VideoFrameHolds const&);
    ~CanvasHost();

    bool create_2d_context(Compositing::CanvasId, Gfx::IntSize, bool alpha, bool will_read_frequently, Gfx::ColorSpace const&);
    bool create_webgl_context(Compositing::CanvasId, OpenGLContext::WebGLVersion, OpenGLContext::DrawingBufferOptions, Gfx::IntSize);
    void destroy_2d_context(Compositing::CanvasId);
    void destroy_webgl_context(Compositing::CanvasId);

    [[nodiscard]] bool describe_canvas_2d_fonts(Vector<Compositing::DisplayListFontDescription> const&, Compositing::RegisteredTypefaces const&);
    Canvas2DCommandPlayer& begin_2d_painting(Compositing::CanvasId, Compositing::RegisteredImageFrames const&);
    bool end_2d_painting_reporting_whether_video_frames_were_sampled_on_gpu();
    bool try_present_2d_context(Compositing::CanvasId);
    void flush_2d_contexts(ReadonlySpan<Compositing::CanvasId>);

    Compositing::CanvasSurfaceOwnerId canvas_surface_owner_id() const { return m_canvas_surface_owner_id; }

    OpenGLContext& webgl_context(Compositing::CanvasId);
    WebGLPresentOutcome present_webgl_context(Compositing::CanvasId, bool preserve_drawing_buffer, bool show_presented_frame);
    Optional<Gfx::IntSize> read_back_webgl_drawing_buffer(Compositing::CanvasId, bool from_presented_frame, Gfx::IntRect, Bytes pixels);
    RefPtr<Media::VideoFrame> held_video_frame(Media::VideoSinkHandle, u64 generation) const;
    WebGLImageConversion convert_image_for_webgl_upload(WebGLImageSource, Compositing::RegisteredImageFrames const&, Gfx::ExportFormat, int export_flags, Optional<Gfx::IntSize> target_size, Gfx::BitmapExportResult& out_pixels) const;
    // The pixels come back in the color space.
    Optional<Gfx::IntSize> read_back_canvas_2d_pixels(Compositing::CanvasId, Gfx::IntRect, Gfx::AlphaType, Gfx::ColorSpace const&, Bytes pixels);
    RefPtr<Gfx::PaintingSurface> presented_surface(Compositing::CanvasId);

    void forget_video_sink(Media::VideoSinkHandle);

    static Gfx::ShareableBitmap read_back_surface(Gfx::PaintingSurface&, Gfx::IntRect, Gfx::AlphaType = Gfx::AlphaType::Premultiplied, Gfx::ColorSpace const& = {});
    static Optional<Gfx::IntSize> read_back_surface_into(Gfx::PaintingSurface&, Gfx::IntRect, Gfx::AlphaType, Gfx::ColorSpace const&, Bytes pixels);

private:
    struct Canvas2DContext {
        NonnullOwnPtr<Canvas2DCommandPlayer> command_player;
        PresentableSurfacePool presented_surfaces;
        NonnullRefPtr<Gfx::PaintingSurface> presented_surface;
    };
    using WebGLContext = NonnullOwnPtr<OpenGLContext>;
    using Context = Variant<Canvas2DContext, WebGLContext>;

    Context* context(Compositing::CanvasId);
    OwnPtr<Canvas2DCommandPlayer> create_2d_command_player(Gfx::IntSize, bool alpha, bool will_read_frequently, Gfx::ColorSpace const&);
    Canvas2DContext& canvas_2d_context(Compositing::CanvasId);
    sk_sp<SkImage> held_video_frame_image(Media::VideoSinkHandle, u64 generation, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context_of_canvas);

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    CanvasSurfaceRegistry& m_canvas_surface_registry;
    Compositing::CanvasSurfaceOwnerId m_canvas_surface_owner_id;
    HashMap<Compositing::CanvasId, Context> m_contexts;
    DrawnFonts m_described_canvas_2d_fonts;
    Compositing::RegisteredImageFrames const* m_image_frames_registered_while_painting { nullptr };

    VideoFrameHolds const& m_video_frame_holds;
    struct VideoFrameImage;
    HashMap<Media::VideoSinkHandle, NonnullOwnPtr<VideoFrameImage>> m_video_frame_images;
    bool m_painting_sampled_video_frame_on_gpu { false };
};

}
