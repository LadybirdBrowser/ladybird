/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <Compositor/Canvas2DCommandPlayer.h>
#include <Compositor/CanvasHost.h>
#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/HostWebGLContext.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/VideoFrameHolds.h>
#include <Compositor/VideoFrameSkia.h>
#include <LibCompositing/CanvasSize.h>
#include <LibCompositing/DisplayList/Canvas2DCommandStream.h>
#include <LibCompositing/DisplayList/DisplayList.h>
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
        if (!m_text_resources.has_font(Compositing::FontResourceId { font_id }))
            return nullptr;
        return &m_text_resources.font(Compositing::FontResourceId { font_id });
    };
    auto image_frame_resolver = [this](u64 frame_id) -> Gfx::DecodedImageFrame const* {
        auto it = m_image_frames_of_batch.find(frame_id);
        if (it == m_image_frames_of_batch.end())
            return nullptr;
        return &it->value;
    };
    auto video_frame_image_resolver = [this, skia_backend_context_of_canvas](u64 video_sink_handle, u64 generation) {
        return held_video_frame_image(Media::VideoSinkHandle { video_sink_handle }, generation, skia_backend_context_of_canvas);
    };
    auto player = make<Canvas2DCommandPlayer>(skia_backend_context_of_canvas, size, format, Gfx::AlphaType::Premultiplied, color_space,
        Canvas2DCommandPlayer::Resolvers {
            .canvas_surface = move(canvas_surface_resolver),
            .font = move(font_resolver),
            .image_frame = move(image_frame_resolver),
            .video_frame_image = move(video_frame_image_resolver),
        });

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

static NonnullRefPtr<Gfx::PaintingSurface> create_presented_canvas_surface(Gfx::PaintingSurface& source, RefPtr<Gfx::SkiaBackendContext> skia_backend_context)
{
    auto surface = Gfx::PaintingSurface::create_with_size(
        source.size(),
        Gfx::BitmapFormat::BGRA8888,
        Gfx::AlphaType::Premultiplied,
        move(skia_backend_context));
    copy_surface_contents(source, *surface);
    return surface;
}

HostWebGLContext& CanvasHost::as_webgl(Context& context)
{
    auto* webgl_context = context.get_pointer<WebGLContext>();
    VERIFY(webgl_context);
    return **webgl_context;
}

bool CanvasHost::create_2d_context(Compositing::CanvasId canvas_id, Gfx::IntSize size, bool alpha, bool will_read_frequently, Gfx::ColorSpace const& color_space)
{
    auto command_player = create_2d_command_player(size, alpha, will_read_frequently, color_space);
    if (!command_player)
        return false;

    // NB: A canvas that draws in memory still presents onto the GPU, if there is one. A present then uploads it once,
    //     instead of every composited frame uploading a fresh snapshot of it.
    auto presented_surface = create_presented_canvas_surface(command_player->surface(), m_skia_backend_context);
    m_canvas_surface_registry.set_canvas_surface(m_canvas_surface_owner_id, canvas_id, presented_surface);
    Canvas2DContext context {
        .command_player = command_player.release_nonnull(),
        .presented_surface = move(presented_surface),
    };
    m_contexts.set(canvas_id, move(context));
    return true;
}

CanvasHost::CreateWebGLContextResult CanvasHost::create_webgl_context(Compositing::WebGL::WebGLVersion version, Gfx::IntSize size, bool depth, bool stencil, bool antialias)
{
    auto context = HostWebGLContext::create(m_skia_backend_context, version, { .depth = depth, .stencil = stencil, .antialias = antialias }, size);
    if (!context)
        return {};

    auto canvas_id = m_canvas_surface_registry.allocate_canvas_id();
    auto supported_extensions = context->gl_context().get_supported_opengl_extensions();
    m_contexts.set(canvas_id, context.release_nonnull());
    return { .success = true, .canvas_id = canvas_id, .supported_extensions = move(supported_extensions) };
}

void CanvasHost::destroy_context(Compositing::CanvasId canvas_id)
{
    if (m_contexts.remove(canvas_id))
        m_canvas_surface_registry.remove_canvas_surface(canvas_id);
}

CanvasHost::Context* CanvasHost::context(Compositing::CanvasId canvas_id)
{
    auto it = m_contexts.find(canvas_id);
    if (it == m_contexts.end())
        return nullptr;
    return &it->value;
}

void CanvasHost::present_canvas_2d_context(Compositing::CanvasId canvas_id, Canvas2DContext& context)
{
    copy_surface_contents(context.command_player->surface(), context.presented_surface);
    m_canvas_surface_registry.set_canvas_surface(m_canvas_surface_owner_id, canvas_id, context.presented_surface);
    context.has_uncommitted_commands = false;
}

void CanvasHost::execute_canvas_2d_stream(Vector<Compositing::Canvas2DCommandStreamSegment> const& segments, Vector<Compositing::FontResourceId> const& kept_font_ids, Vector<Compositing::DisplayListFontResource> const& added_fonts, Vector<Compositing::DisplayListImageFrameResource> const& image_frames)
{
    Compositing::DisplayListResourceSet resources;
    for (auto font_id : kept_font_ids)
        resources.fonts.set(font_id);
    for (auto const& font : added_fonts) {
        // NB: Font IDs are immutable. Keep the typeface that Skia's glyph caches were built for.
        if (!m_text_resources.has_font(font.id))
            m_text_resources.set_font(font.id, font.font);
        resources.fonts.set(font.id);
    }
    m_text_resources.retain_only(resources);
    for (auto const& image_frame : image_frames)
        m_image_frames_of_batch.set(image_frame.id.value(), image_frame.frame);
    ScopeGuard forget_image_frames_of_batch = [&] { m_image_frames_of_batch.clear(); };
    Vector<NonnullRefPtr<Gfx::PaintingSurface>> surfaces_that_sample_video_frames;
    for (auto const& segment : segments) {
        // The canvas may have been destroyed while this segment was pending in
        // WebContent, so a missing context is not a protocol violation.
        auto* context = this->context(segment.canvas_id);
        auto* canvas_context = context ? context->get_pointer<Canvas2DContext>() : nullptr;
        if (!canvas_context)
            continue;

        if (!segment.tape.is_empty()) {
            m_drew_video_frame_in_segment = false;
            canvas_context->command_player->play(segment.tape);
            canvas_context->has_uncommitted_commands = true;
            if (m_drew_video_frame_in_segment && !surfaces_that_sample_video_frames.contains_slow(canvas_context->command_player->surface()))
                surfaces_that_sample_video_frames.append(canvas_context->command_player->surface());
        }

        if (segment.present && canvas_context->has_uncommitted_commands)
            present_canvas_2d_context(segment.canvas_id, *canvas_context);
    }

    // The frames stay held only until WebContent releases them, which follows this stream, so the GPU has to have
    // finished sampling them by then.
    if (m_skia_backend_context) {
        for (auto& surface : surfaces_that_sample_video_frames)
            m_skia_backend_context->flush_and_submit(surface);
    }
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
            m_drew_video_frame_in_segment = true;
    }
    return video_frame_image->image_on_gpu;
}

void CanvasHost::forget_video_sink(Media::VideoSinkHandle handle)
{
    m_video_frame_images.remove(handle);
}

Vector<HostWebGLContext::UploadSource> CanvasHost::resolve_webgl_upload_sources(Vector<Compositing::WebGL::ImageSourceFrame> const& frames) const
{
    Vector<HostWebGLContext::UploadSource> sources;
    sources.ensure_capacity(frames.size());
    for (auto const& frame : frames) {
        frame.visit(
            [&](Gfx::DecodedImageFrame const& decoded_frame) {
                sources.unchecked_append(decoded_frame);
            },
            [&](Compositing::CapturedVideoFrame const& captured_frame) {
                if (auto video_frame = m_video_frame_holds.held_frame(captured_frame.sink_handle, captured_frame.generation)) {
                    sources.unchecked_append(video_frame.release_nonnull());
                    return;
                }
                sources.unchecked_append(Gfx::DecodedImageFrame { NonnullRefPtr<Gfx::Bitmap const> { MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 1, 1 })) } });
            });
    }
    return sources;
}

void CanvasHost::execute_webgl_commands(Compositing::CanvasId canvas_id, ReadonlyBytes commands, Vector<Compositing::WebGL::ImageSourceFrame> const& frames)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    MUST(as_webgl(*context).execute_commands(commands, resolve_webgl_upload_sources(frames)));
}

void CanvasHost::set_webgl_shared_command_buffer(Compositing::CanvasId canvas_id, Compositing::WebGL::WebGLSharedCommandBuffer shared_command_buffer)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    as_webgl(*context).set_shared_command_buffer(move(shared_command_buffer));
}

bool CanvasHost::execute_webgl_commands_from_shared_buffer(Compositing::CanvasId canvas_id, u64 offset, u64 size_in_bytes, u64 flush_sequence_number, Vector<Compositing::WebGL::ImageSourceFrame> const& frames)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    auto& webgl_context = as_webgl(*context);

    auto commands = webgl_context.shared_command_buffer_range(offset, size_in_bytes);
    if (!commands.has_value())
        return false;

    // WebContent can rewrite shared bytes while they execute, so a malformed stream is
    // a misbehaving peer rather than a Compositor bug; report instead of crashing.
    if (webgl_context.execute_commands(*commands, resolve_webgl_upload_sources(frames)).is_error())
        return false;
    webgl_context.store_executed_flush_sequence_number(flush_sequence_number);
    return true;
}

ErrorOr<ByteBuffer> CanvasHost::execute_webgl_sync_call(Compositing::CanvasId canvas_id, ByteBuffer request)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    return as_webgl(*context).execute_sync_call(request);
}

Compositing::WebGL::ReadPixelsResult CanvasHost::webgl_read_pixels_robust_angle(Compositing::CanvasId canvas_id, Compositing::WebGL::GLint x, Compositing::WebGL::GLint y, Compositing::WebGL::GLsizei width, Compositing::WebGL::GLsizei height, Compositing::WebGL::GLenum format, Compositing::WebGL::GLenum type, Compositing::WebGL::GLsizei buf_size, Core::AnonymousBuffer pixels)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    return as_webgl(*context).read_pixels_robust_angle(x, y, width, height, format, type, buf_size, move(pixels));
}

bool CanvasHost::webgl_read_buffer_sub_data(Compositing::CanvasId canvas_id, Compositing::WebGL::GLenum target, Compositing::WebGL::GLintptr offset, Compositing::WebGL::GLintptr size, Core::AnonymousBuffer data)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    return as_webgl(*context).read_buffer_sub_data(target, offset, size, move(data));
}

void CanvasHost::present_webgl_canvas(Compositing::CanvasId canvas_id, bool preserve_drawing_buffer)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);

    auto surface = MUST(as_webgl(*context).prepare_for_compositing(preserve_drawing_buffer));
    m_canvas_surface_registry.set_canvas_surface(m_canvas_surface_owner_id, canvas_id, move(surface));
}

void CanvasHost::clear_webgl_drawing_buffer(Compositing::CanvasId canvas_id)
{
    auto* context = this->context(canvas_id);
    if (!context || !context->has<WebGLContext>())
        return;
    as_webgl(*context).clear_drawing_buffer();
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

RefPtr<Gfx::PaintingSurface> CanvasHost::presented_surface(Compositing::CanvasId canvas_id)
{
    auto* context = this->context(canvas_id);
    if (!context)
        return nullptr;

    return context->visit(
        [](Canvas2DContext& canvas_context) -> RefPtr<Gfx::PaintingSurface> {
            return canvas_context.presented_surface;
        },
        [](WebGLContext& webgl_context) -> RefPtr<Gfx::PaintingSurface> {
            return webgl_context->presented_surface();
        });
}

Gfx::ShareableBitmap CanvasHost::read_back_pixels(Compositing::CanvasId canvas_id, Gfx::IntRect rect, Gfx::AlphaType alpha_type, Gfx::ColorSpace const& color_space)
{
    auto* context = this->context(canvas_id);
    if (!context)
        return {};

    return context->visit(
        [rect, alpha_type, &color_space](Canvas2DContext& canvas_context) {
            return read_back_surface(canvas_context.command_player->surface(), rect, alpha_type, color_space);
        },
        [rect, alpha_type, &color_space](WebGLContext& webgl_context) -> Gfx::ShareableBitmap {
            // NB: The drawing buffer of a WebGL context is sRGB.
            if (alpha_type != Gfx::AlphaType::Premultiplied || !color_space.data().has<Empty>())
                return {};
            return webgl_context->read_back_drawing_buffer(rect);
        });
}

}
