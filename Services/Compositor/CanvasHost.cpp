/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/BitmapExport.h>
#include <Compositor/Canvas2DCommandPlayer.h>
#include <Compositor/CanvasHost.h>
#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/OpenGLContext.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/VideoFrameHolds.h>
#include <Compositor/VideoFrameSkia.h>
#include <LibCompositing/CanvasSize.h>
#include <LibCompositing/DisplayList/RegisteredImageFrames.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <core/SkImage.h>

namespace Compositor {

struct CanvasHost::VideoFrameImage {
    AK_ALLOC_WITH_KMALLOC;

    u64 generation { 0 };
    // Kept until replaced, since the GPU samples the image after the commands are played.
    RefPtr<Media::VideoFrame> frame;
    sk_sp<SkImage> image_on_gpu;
    // NB: A canvas that draws in memory does not read back image_on_gpu instead: Skia caches readbacks by image ID,
    //     and the image of a video surface keeps its ID while the surface shows new frames.
    sk_sp<SkImage> image_in_memory;
};

CanvasHost::CanvasHost(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, CanvasSurfaceRegistry& canvas_surface_registry, VideoFrameHolds const& video_frame_holds)
    : m_skia_backend_context(move(skia_backend_context))
    , m_canvas_surface_registry(canvas_surface_registry)
    , m_canvas_surface_owner_id(canvas_surface_registry.allocate_owner_id())
    , m_video_frame_holds(video_frame_holds)
{
}

CanvasHost::~CanvasHost()
{
    for (auto canvas_id : m_contexts.keys())
        m_canvas_surface_registry.remove_canvas_surface(canvas_id);
}

OwnPtr<Canvas2DCommandPlayer> CanvasHost::create_2d_command_player(Gfx::IntSize size, bool alpha, bool will_read_frequently, Gfx::ColorSpace const& color_space)
{
    if (!Compositing::is_valid_canvas_2d_size(size))
        return nullptr;

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-will-read-frequently
    // "When a CanvasSettings object's will read frequently is true, the user agent may optimize the canvas for
    //  readback operations."
    // NB: The canvas then draws in memory, where a readback copies its pixels instead of waiting for the GPU.
    auto skia_backend_context_of_canvas = will_read_frequently ? nullptr : m_skia_backend_context;

    auto format = alpha ? Gfx::BitmapFormat::BGRA8888 : Gfx::BitmapFormat::BGRx8888;
    auto canvas_surface_resolver = [this](u64 canvas_id) -> Gfx::PaintingSurface const* {
        // A 2D source resolves to its live draw surface: the shared command
        // stream replays in recording order, so at this point the surface holds
        // exactly the commands recorded before the referencing DrawCanvas.
        auto* context = this->context(Compositing::CanvasId { canvas_id });
        if (!context)
            return nullptr;
        if (auto* canvas_context = context->get_pointer<Canvas2DContext>())
            return canvas_context->command_player->surface().ptr();
        // WebGL sources are presented separately and resolve via the registry.
        return m_canvas_surface_registry.canvas_surface(m_canvas_surface_owner_id, Compositing::CanvasId { canvas_id });
    };
    auto font_resolver = [this](u64 font_id) -> Gfx::Font const* {
        if (!m_described_canvas_2d_fonts.contains(Compositing::FontResourceId { font_id }))
            return nullptr;
        return &m_described_canvas_2d_fonts.font(Compositing::FontResourceId { font_id });
    };
    auto image_frame_resolver = [this](u64 frame_id) -> Gfx::DecodedImageFrame const* {
        if (!m_image_frames_registered_while_painting)
            return nullptr;
        auto image_frame = m_image_frames_registered_while_painting->image_frame(Compositing::ImageFrameResourceId { frame_id });
        if (!image_frame)
            return nullptr;
        return &image_frame->frame();
    };
    auto video_frame_image_resolver = [this, skia_backend_context_of_canvas](u64 video_sink_handle, u64 generation) {
        return held_video_frame_image(Media::VideoSinkHandle { video_sink_handle }, generation, skia_backend_context_of_canvas);
    };
    auto player_or_error = Canvas2DCommandPlayer::create(skia_backend_context_of_canvas, size, format, Gfx::AlphaType::Premultiplied, color_space,
        Canvas2DCommandPlayer::Resolvers {
            .canvas_surface = move(canvas_surface_resolver),
            .font = move(font_resolver),
            .image_frame = move(image_frame_resolver),
            .video_frame_image = move(video_frame_image_resolver),
        });
    // NB: Memory for a canvas of a size that passed the check above can still run out.
    if (player_or_error.is_error()) {
        dbgln("Unable to create a {}x{} 2D canvas context: {}", size.width(), size.height(), player_or_error.error());
        return nullptr;
    }
    auto player = player_or_error.release_value();

    // https://html.spec.whatwg.org/multipage/canvas.html#the-canvas-settings:concept-canvas-alpha
    // "Thus, the bitmap of such a context starts off as opaque black instead of transparent black"
    // AD-HOC: Skia hands out a fully transparent surface by default; only clear when alpha is disabled.
    if (!alpha)
        player->clear(Gfx::Color::Black);

    return player;
}

static void copy_surface_contents(Gfx::PaintingSurface& source, Gfx::PaintingSurface& destination)
{
    destination.copy_from_surface(source);
}

// The pool keeps a surface that the window server shows, or that a frame takes to it, as it is.
static ErrorOr<NonnullRefPtr<PresentableSurface>> present_into_surface_of_pool(PresentableSurfacePool& pool, Gfx::PaintingSurface& source, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto surface = TRY(pool.surface_for_next_content(source.size(), skia_backend_context));
    copy_surface_contents(source, surface->painting_surface());
    return surface;
}

bool CanvasHost::create_2d_context(Compositing::CanvasId canvas_id, Gfx::IntSize size, bool alpha, bool will_read_frequently, Gfx::ColorSpace const& color_space)
{
    auto command_player = create_2d_command_player(size, alpha, will_read_frequently, color_space);
    if (!command_player)
        return false;

    // NB: A canvas that draws in memory still presents onto the GPU, if there is one. A present then uploads it once,
    //     instead of every composited frame uploading a fresh snapshot of it.
    PresentableSurfacePool presented_surfaces;
    auto presented_surface_or_error = present_into_surface_of_pool(presented_surfaces, command_player->surface(), m_skia_backend_context);
    if (presented_surface_or_error.is_error()) {
        dbgln("Unable to create the presented surface of a {}x{} 2D canvas context: {}", size.width(), size.height(), presented_surface_or_error.error());
        return false;
    }
    auto presented_surface = presented_surface_or_error.release_value();
    m_canvas_surface_registry.set_canvas_surface(m_canvas_surface_owner_id, canvas_id, *presented_surface);
    Canvas2DContext context {
        .command_player = command_player.release_nonnull(),
        .presented_surfaces = move(presented_surfaces),
        .presented_surface = presented_surface->painting_surface(),
    };
    m_contexts.set(canvas_id, move(context));
    return true;
}

bool CanvasHost::create_webgl_context(Compositing::CanvasId canvas_id, OpenGLContext::WebGLVersion version, OpenGLContext::DrawingBufferOptions options, Gfx::IntSize size)
{
    auto gl_context = OpenGLContext::create(m_skia_backend_context, version, options);
    if (!gl_context)
        return false;
    gl_context->set_size(size);
    m_contexts.set(canvas_id, gl_context.release_nonnull());
    return true;
}

void CanvasHost::destroy_2d_context(Compositing::CanvasId canvas_id)
{
    auto* context = this->context(canvas_id);
    VERIFY(context && context->has<Canvas2DContext>());
    m_contexts.remove(canvas_id);
    m_canvas_surface_registry.remove_canvas_surface(canvas_id);
}

void CanvasHost::destroy_webgl_context(Compositing::CanvasId canvas_id)
{
    auto* context = this->context(canvas_id);
    if (!context || !context->has<WebGLContext>())
        return;
    m_contexts.remove(canvas_id);
    m_canvas_surface_registry.remove_canvas_surface(canvas_id);
}

CanvasHost::Context* CanvasHost::context(Compositing::CanvasId canvas_id)
{
    auto it = m_contexts.find(canvas_id);
    if (it == m_contexts.end())
        return nullptr;
    return &it->value;
}

CanvasHost::Canvas2DContext& CanvasHost::canvas_2d_context(Compositing::CanvasId canvas_id)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    return context->get<Canvas2DContext>();
}

bool CanvasHost::try_present_2d_context(Compositing::CanvasId canvas_id)
{
    auto& context = canvas_2d_context(canvas_id);
    auto presented_surface_or_error = present_into_surface_of_pool(context.presented_surfaces, context.command_player->surface(), m_skia_backend_context);
    // The canvas goes on showing what it presented before, and presents again with its next commands.
    if (presented_surface_or_error.is_error()) {
        dbgln("Unable to present a 2D canvas context: {}", presented_surface_or_error.error());
        return false;
    }
    auto presented_surface = presented_surface_or_error.release_value();
    m_canvas_surface_registry.set_canvas_surface(m_canvas_surface_owner_id, canvas_id, *presented_surface);
    context.presented_surface = presented_surface->painting_surface();
    return true;
}

bool CanvasHost::describe_canvas_2d_fonts(Vector<Compositing::DisplayListFontDescription> const& fonts, Compositing::RegisteredTypefaces const& registered_typefaces)
{
    if (!m_described_canvas_2d_fonts.add_described_fonts_made_of_registered_typefaces(fonts, registered_typefaces))
        return false;
    Vector<u64> described_font_ids;
    described_font_ids.ensure_capacity(fonts.size());
    for (auto const& font : fonts)
        described_font_ids.unchecked_append(font.id.value());
    (void)m_described_canvas_2d_fonts.retain_only(described_font_ids);
    return true;
}

Canvas2DCommandPlayer& CanvasHost::begin_2d_painting(Compositing::CanvasId canvas_id, Compositing::RegisteredImageFrames const& registered_image_frames)
{
    VERIFY(!m_image_frames_registered_while_painting);
    m_image_frames_registered_while_painting = &registered_image_frames;
    m_painting_sampled_video_frame_on_gpu = false;
    return *canvas_2d_context(canvas_id).command_player;
}

bool CanvasHost::end_2d_painting_reporting_whether_video_frames_were_sampled_on_gpu()
{
    m_image_frames_registered_while_painting = nullptr;
    return m_painting_sampled_video_frame_on_gpu;
}

void CanvasHost::flush_2d_contexts(ReadonlySpan<Compositing::CanvasId> canvas_ids)
{
    if (!m_skia_backend_context)
        return;
    for (auto canvas_id : canvas_ids)
        m_skia_backend_context->flush_and_submit(canvas_2d_context(canvas_id).command_player->surface());
}

sk_sp<SkImage> CanvasHost::held_video_frame_image(Media::VideoSinkHandle handle, u64 generation, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context_of_canvas)
{
    auto* video_frame_image = m_video_frame_images.get(handle).value_or(nullptr);
    if (!video_frame_image || video_frame_image->generation != generation) {
        auto frame = m_video_frame_holds.held_frame(handle, generation);
        if (!frame)
            return nullptr;
        auto new_video_frame_image = make<VideoFrameImage>(generation, move(frame), sk_sp<SkImage> {}, sk_sp<SkImage> {});
        video_frame_image = new_video_frame_image.ptr();
        m_video_frame_images.set(handle, move(new_video_frame_image));
    }

    if (!skia_backend_context_of_canvas) {
        if (!video_frame_image->image_in_memory)
            video_frame_image->image_in_memory = sk_image_in_memory_from_video_frame(*video_frame_image->frame, m_skia_backend_context);
        return video_frame_image->image_in_memory;
    }

    if (!video_frame_image->image_on_gpu) {
        video_frame_image->image_on_gpu = sk_image_from_video_frame(*video_frame_image->frame, skia_backend_context_of_canvas);
        if (video_frame_image->image_on_gpu)
            m_painting_sampled_video_frame_on_gpu = true;
    }
    return video_frame_image->image_on_gpu;
}

void CanvasHost::forget_video_sink(Media::VideoSinkHandle handle)
{
    m_video_frame_images.remove(handle);
}

OpenGLContext& CanvasHost::webgl_context(Compositing::CanvasId canvas_id)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    return *context->get<WebGLContext>();
}

CanvasHost::WebGLPresentOutcome CanvasHost::present_webgl_context(Compositing::CanvasId canvas_id, bool preserve_drawing_buffer, bool show_presented_frame)
{
    auto& gl_context = webgl_context(canvas_id);
    auto outcome = WebGLPresentOutcome::ShowedPresentedFrame;
    auto surface = show_presented_frame ? gl_context.presented_surface() : nullptr;
    if (!surface) {
        gl_context.present_drawing_buffer(preserve_drawing_buffer);
        surface = gl_context.presented_surface();
        if (!surface)
            return WebGLPresentOutcome::NoDrawingBuffer;
        outcome = WebGLPresentOutcome::Presented;
    }
    if (auto presentable_surface = gl_context.presented_presentable_surface(); presentable_surface && &presentable_surface->painting_surface() == surface.ptr())
        m_canvas_surface_registry.set_canvas_surface(m_canvas_surface_owner_id, canvas_id, *presentable_surface);
    else
        m_canvas_surface_registry.set_canvas_surface(m_canvas_surface_owner_id, canvas_id, surface.release_nonnull());
    return outcome;
}

Optional<Gfx::IntSize> CanvasHost::read_back_webgl_drawing_buffer(Compositing::CanvasId canvas_id, bool from_presented_frame, Gfx::IntRect rect, Bytes pixels)
{
    auto& gl_context = webgl_context(canvas_id);
    gl_context.make_current();
    gl_context.flush_drawing_buffer();
    auto surface = from_presented_frame ? gl_context.presented_surface() : gl_context.drawing_buffer_surface();
    if (!surface)
        return {};
    return read_back_surface_into(*surface, rect, Gfx::AlphaType::Premultiplied, {}, pixels);
}

RefPtr<Media::VideoFrame> CanvasHost::held_video_frame(Media::VideoSinkHandle handle, u64 generation) const
{
    return m_video_frame_holds.held_frame(handle, generation);
}

CanvasHost::WebGLImageConversion CanvasHost::convert_image_for_webgl_upload(WebGLImageSource source, Compositing::RegisteredImageFrames const& registered_image_frames, Gfx::ExportFormat format, int export_flags, Optional<Gfx::IntSize> target_size, Gfx::BitmapExportResult& out_pixels) const
{
    Optional<BitmapInColorSpace> bitmap;
    auto conversion = source.visit(
        [&](Compositing::ImageFrameResourceId id) {
            auto image_frame = registered_image_frames.image_frame(id);
            if (!image_frame)
                return WebGLImageConversion::UnknownImageFrame;
            bitmap = BitmapInColorSpace { image_frame->frame().bitmap_sharing_pixels(), image_frame->frame().color_space() };
            return WebGLImageConversion::Converted;
        },
        [&](HeldVideoFrame held_video_frame) {
            if (auto video_frame = this->held_video_frame(held_video_frame.sink_handle, held_video_frame.generation)) {
                auto video_bitmap = bitmap_from_video_frame(*video_frame, m_skia_backend_context);
                if (video_bitmap.is_error())
                    return WebGLImageConversion::Failed;
                bitmap = video_bitmap.release_value();
                return WebGLImageConversion::Converted;
            }
            auto missing_frame_bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 1, 1 });
            if (missing_frame_bitmap.is_error())
                return WebGLImageConversion::Failed;
            bitmap = BitmapInColorSpace { missing_frame_bitmap.release_value(), {} };
            return WebGLImageConversion::Converted;
        });
    if (conversion != WebGLImageConversion::Converted)
        return conversion;

    Optional<int> target_width;
    Optional<int> target_height;
    if (target_size.has_value()) {
        target_width = target_size->width();
        target_height = target_size->height();
    }
    auto pixels = Gfx::export_bitmap_to_byte_buffer(*bitmap->bitmap, bitmap->color_space, format, export_flags, target_width, target_height);
    if (pixels.is_error())
        return WebGLImageConversion::Failed;
    out_pixels = pixels.release_value();
    return WebGLImageConversion::Converted;
}

Gfx::ShareableBitmap CanvasHost::read_back_surface(Gfx::PaintingSurface& surface, Gfx::IntRect rect, Gfx::AlphaType alpha_type, Gfx::ColorSpace const& color_space)
{
    auto clipped_rect = rect.intersected(surface.rect());
    if (clipped_rect.is_empty())
        return {};

    // Unpremultiplied pixels are for ImageData, which stores them as RGBA8888. Reading into that format lets the
    // surface do the conversion.
    auto format = alpha_type == Gfx::AlphaType::Unpremultiplied ? Gfx::BitmapFormat::RGBA8888 : Gfx::BitmapFormat::BGRA8888;
    auto bitmap_or_error = Gfx::Bitmap::create_shareable(format, alpha_type, clipped_rect.size());
    if (bitmap_or_error.is_error())
        return {};

    auto bitmap = bitmap_or_error.release_value();
    surface.flush();
    surface.read_into_bitmap(*bitmap, clipped_rect.location(), color_space);
    return Gfx::ShareableBitmap { move(bitmap), Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
}

Optional<Gfx::IntSize> CanvasHost::read_back_surface_into(Gfx::PaintingSurface& surface, Gfx::IntRect rect, Gfx::AlphaType alpha_type, Gfx::ColorSpace const& color_space, Bytes pixels)
{
    auto clipped_rect = rect.intersected(surface.rect());
    if (clipped_rect.is_empty())
        return {};
    auto pitch = static_cast<size_t>(clipped_rect.width()) * sizeof(u32);
    if (pitch * static_cast<size_t>(clipped_rect.height()) > pixels.size())
        return {};

    auto format = alpha_type == Gfx::AlphaType::Unpremultiplied ? Gfx::BitmapFormat::RGBA8888 : Gfx::BitmapFormat::BGRA8888;
    auto bitmap = Gfx::Bitmap::create_wrapper(format, alpha_type, clipped_rect.size(), pitch, pixels.data());
    if (bitmap.is_error())
        return {};
    surface.flush();
    surface.read_into_bitmap(*bitmap.value(), clipped_rect.location(), color_space);
    return clipped_rect.size();
}

RefPtr<Gfx::PaintingSurface> CanvasHost::presented_surface(Compositing::CanvasId canvas_id)
{
    auto* context = this->context(canvas_id);
    if (!context)
        return nullptr;

    return context->visit(
        [](Canvas2DContext& canvas_context) -> RefPtr<Gfx::PaintingSurface> {
            return canvas_context.presented_surface;
        },
        [](WebGLContext& gl_context) -> RefPtr<Gfx::PaintingSurface> {
            return gl_context->presented_surface();
        });
}

Optional<Gfx::IntSize> CanvasHost::read_back_canvas_2d_pixels(Compositing::CanvasId canvas_id, Gfx::IntRect rect, Gfx::AlphaType alpha_type, Gfx::ColorSpace const& color_space, Bytes pixels)
{
    auto* context = this->context(canvas_id);
    if (!context)
        return {};
    auto* canvas_context = context->get_pointer<Canvas2DContext>();
    if (!canvas_context)
        return {};
    return read_back_surface_into(canvas_context->command_player->surface(), rect, alpha_type, color_space, pixels);
}

}
