/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/ScopeGuard.h>
#include <LibCore/ElapsedTimer.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>
#include <LibPaintServer/Debug.h>
#include <PaintServer/Compositor/DisplayListPlayer.h>
#include <PaintServer/Compositor/DrawCommandPlayer.h>
#include <PaintServer/Painter.h>
#include <PaintServer/RenderClient/ResourceManager.h>
#include <core/SkBitmap.h>
#include <core/SkCanvas.h>
#include <core/SkImage.h>
#include <core/SkPaint.h>
#include <core/SkRect.h>
#include <core/SkShader.h>

namespace PaintServer {

static Optional<Gfx::BitmapFormat> software_bitmap_format(Gfx::BitmapFormat bitmap_format)
{
    switch (bitmap_format) {
    case Gfx::BitmapFormat::BGRA8888:
        return Gfx::BitmapFormat::BGRA8888;
    case Gfx::BitmapFormat::RGBA8888:
        return Gfx::BitmapFormat::RGBA8888;
    case Gfx::BitmapFormat::BGRx8888:
    case Gfx::BitmapFormat::RGBx8888:
    case Gfx::BitmapFormat::RGBAF16:
    default:
        return {};
    }
}

static sk_sp<SkImage> image_for_scaling_mode(sk_sp<SkImage> image, Gfx::ScalingMode scaling_mode)
{
    if (scaling_mode != Gfx::ScalingMode::BilinearMipmap)
        return image;

    if (auto image_with_mipmaps = image->withDefaultMipmaps())
        return image_with_mipmaps;
    return image;
}

static sk_sp<SkImage> scaled_image_for_mipmap(SkImage const& image, Gfx::FloatRect const& src_rect, Gfx::FloatRect const& dst_rect, Gfx::ScalingMode scaling_mode)
{
    if (scaling_mode != Gfx::ScalingMode::BilinearMipmap)
        return nullptr;
    if (src_rect.x() != 0.0f || src_rect.y() != 0.0f || src_rect.width() != static_cast<f32>(image.width()) || src_rect.height() != static_cast<f32>(image.height()))
        return nullptr;

    int const width = static_cast<int>(dst_rect.width());
    int const height = static_cast<int>(dst_rect.height());
    if (width <= 0 || height <= 0 || static_cast<float>(width) != dst_rect.width() || static_cast<float>(height) != dst_rect.height())
        return nullptr;
    if (width >= image.width() && height >= image.height())
        return nullptr;

    SkBitmap scaled_bitmap;
    if (!scaled_bitmap.tryAllocPixels(image.imageInfo().makeWH(width, height)))
        return nullptr;

    SkPixmap scaled_pixmap;
    if (!scaled_bitmap.peekPixels(&scaled_pixmap))
        return nullptr;
    if (!image.scalePixels(scaled_pixmap, Gfx::to_skia_sampling_options(scaling_mode)))
        return nullptr;

    scaled_bitmap.setImmutable();
    return scaled_bitmap.asImage();
}

Painter::~Painter() = default;

void Painter::draw_missing_image_placeholder(Gfx::FloatRect const& dst_rect, Gfx::FloatRect const& clip_rect, SkCanvas& canvas)
{
    if (dst_rect.is_empty() || clip_rect.is_empty())
        return;

    if (!is_logging_enabled())
        return;

    SkPaint fallback_paint;
    fallback_paint.setAntiAlias(true);
    fallback_paint.setColor(SK_ColorMAGENTA);

    canvas.save();
    canvas.clipRect(SkRect::MakeXYWH(clip_rect.x(), clip_rect.y(), clip_rect.width(), clip_rect.height()), true);
    canvas.drawRect(SkRect::MakeXYWH(dst_rect.x(), dst_rect.y(), dst_rect.width(), dst_rect.height()), fallback_paint);
    canvas.restore();
}

ErrorOr<NonnullRefPtr<Gfx::SkiaBackendContext>> Painter::skia_context()
{
    if (m_skia_backend_context)
        return NonnullRefPtr<Gfx::SkiaBackendContext>(*m_skia_backend_context);

    auto context = TRY(create_skia_context());
    m_skia_backend_context = context;
    return context;
}

ErrorOr<NonnullRefPtr<Gfx::SkiaBackendContext>> Painter::create_skia_context()
{
    if (is_cpu_painting()) {
        auto context = Gfx::SkiaBackendContext::create_raster_context();
        if (!context)
            return Error::from_string_literal("Failed to create Skia raster backend context");
        return context.release_nonnull();
    }
    return create_gpu_backed_skia_context();
}

void Painter::shutdown()
{
    m_surfaces.clear();
    m_presentation_buffer_surface_ids.clear();
    m_surface_active_presentation_buffer.clear();
    m_cpu_backed_presentation_buffers.clear();
    m_gpu_backed_presentation_buffers.clear();
    m_paint_surfaces.clear();
    m_skia_backend_context = nullptr;
}

ErrorOr<void> Painter::register_presentation_buffer(SurfaceID surface_id, u64 image_id, Gfx::SharedImagePayload image_payload)
{
    if (!surface_state(surface_id).has_value())
        return Error::from_string_literal("Painter: surface_id is not valid");

    if (is_cpu_painting()) {
        m_cpu_backed_presentation_buffers.set(image_id, TRY(import_cpu_backed_presentation_buffer(move(image_payload))));
        note_registered_presentation_buffer(surface_id, image_id);
        return {};
    }

    m_gpu_backed_presentation_buffers.set(image_id, TRY(import_gpu_backed_presentation_buffer(move(image_payload))));
    note_registered_presentation_buffer(surface_id, image_id);
    return {};
}

ErrorOr<Gfx::SharedImage> Painter::create_content_image(u64 image_id, Gfx::IntSize size, Gfx::BitmapFormat format)
{
    if (image_id == 0)
        return Error::from_string_literal("Painter: image_id must be non-zero");
    if (size.is_empty())
        return Error::from_string_literal("Painter: image dimensions must be non-zero");

    if (is_cpu_painting()) {
        return Gfx::SharedImage::create({
            .size = size,
            .pixel_format = format,
            .color_space = Gfx::BitmapColorSpace::SRGB,
            .alpha_type = Gfx::BitmapAlpha::Premultiplied,
            .origin = Gfx::BitmapOrigin::TopLeft,
        });
    }

    return create_gpu_backed_content_image(image_id, size, format);
}

ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> Painter::create_content_image_painting_surface(Gfx::SharedImage& shared_image, OffscreenBackendPreference backend_preference)
{
    if (is_cpu_painting() || backend_preference == OffscreenBackendPreference::RequireCPU)
        return Gfx::PaintingSurface::wrap_bitmap(*shared_image.bitmap());
    auto context = TRY(skia_context());
    return shared_image.create_painting_surface(context, Gfx::PaintingSurface::Origin::TopLeft);
}

ErrorOr<void> Painter::paint_draw_list_to_canvas(DrawContext const& draw_context, Gfx::PaintingSurface& painting_surface, DrawListView draw_list)
{
    DrawContext canvas_draw_context = draw_context;
    DrawScaledImagePainter draw_scaled_image_handler = [this, &canvas_draw_context](DrawScaledImageCommand const& command, SkCanvas& canvas, sk_sp<SkImageFilter> const& image_filter) { return draw_scaled_image(canvas_draw_context, command, canvas, image_filter); };
    DrawExternalContentPainter draw_external_content_handler = [this, &canvas_draw_context](DrawExternalContentCommand const& command, SkCanvas& canvas) { return draw_external_content(canvas_draw_context, command, canvas); };
    DrawRepeatedImagePainter draw_repeated_image_handler = [this, &canvas_draw_context](DrawRepeatedImageCommand const& command, SkCanvas& canvas) { return draw_repeated_image(canvas_draw_context, command, canvas); };
    canvas_draw_context.draw_scaled_image_painter = &draw_scaled_image_handler;
    canvas_draw_context.draw_external_content_painter = &draw_external_content_handler;
    canvas_draw_context.draw_repeated_image_painter = &draw_repeated_image_handler;

    auto& canvas = painting_surface.canvas();
    if (canvas.getSaveCount() == 1)
        canvas.save();

    DrawCommandPlayer player(canvas_draw_context, canvas);
    auto cursor = draw_list.cursor();
    for (;;) {
        auto maybe_command = TRY(cursor.next());
        if (!maybe_command.has_value())
            break;
        TRY(player.apply(maybe_command.value()));
    }

    if (auto context = painting_surface.skia_backend_context())
        context->flush_and_submit(&painting_surface.sk_surface());
    else
        painting_surface.flush();
    return {};
}

bool Painter::create_surface(SurfaceID surface_id, Gfx::IntSize logical_size, Gfx::IntSize buffer_size)
{
    if (logical_size.is_empty() || buffer_size.is_empty() || !buffer_size.contains(logical_size))
        return false;

    Optional<SurfaceState> previous_state = surface_state(surface_id);

    m_surfaces.set(surface_id, SurfaceState {
                                   .logical_size = logical_size,
                                   .buffer_size = buffer_size,
                               });

    if (previous_state.has_value()) {
        bool const logical_size_changed = previous_state->logical_size != logical_size;
        bool const buffer_size_changed = previous_state->buffer_size != buffer_size;
        if (logical_size_changed || buffer_size_changed)
            remove_paint_surface(surface_id);
    }

    return true;
}

void Painter::destroy_surface(SurfaceID surface_id)
{
    m_surfaces.remove(surface_id);
    unregister_all_presentation_buffers_for_surface(surface_id);
}

bool Painter::has_surface(SurfaceID surface_id) const
{
    return m_surfaces.contains(surface_id);
}

Optional<Painter::SurfaceState> Painter::surface_state(SurfaceID surface_id) const
{
    auto maybe_surface = m_surfaces.get(surface_id);
    if (!maybe_surface.has_value())
        return {};
    return maybe_surface.value();
}

void Painter::unregister_all_presentation_buffers_for_surface(SurfaceID surface_id)
{
    m_surface_active_presentation_buffer.remove(surface_id);
    m_paint_surfaces.remove(surface_id);

    Vector<u64> ids_to_remove;
    ids_to_remove.ensure_capacity(m_presentation_buffer_surface_ids.size());
    for (auto const& it : m_presentation_buffer_surface_ids) {
        if (it.value == surface_id)
            ids_to_remove.append(it.key);
    }

    for (auto image_id : ids_to_remove) {
        unregister_presentation_buffer(image_id);
        m_presentation_buffer_surface_ids.remove(image_id);
    }
}

void Painter::unregister_presentation_buffer_by_id(u64 image_id)
{
    auto maybe_surface_id = m_presentation_buffer_surface_ids.get(image_id);
    if (!maybe_surface_id.has_value())
        return;

    unregister_presentation_buffer(image_id);
    m_presentation_buffer_surface_ids.remove(image_id);

    if (auto active_image_id = m_surface_active_presentation_buffer.get(maybe_surface_id.value()); active_image_id.has_value() && active_image_id.value() == image_id)
        m_surface_active_presentation_buffer.remove(maybe_surface_id.value());
}

void Painter::clear_surface_cached_state(SurfaceID surface_id)
{
    remove_paint_surface(surface_id);
}

bool Painter::set_surface_presentation_buffer(SurfaceID surface_id, u64 image_id)
{
    if (!has_registered_presentation_buffer(image_id))
        return false;
    m_surface_active_presentation_buffer.set(surface_id, image_id);
    return true;
}

ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> Painter::acquire_paint_surface(SurfaceID surface_id, Optional<u64> presentation_buffer_id)
{
    Optional<u64> active_buffer_id = presentation_buffer_id;
    if (!active_buffer_id.has_value())
        active_buffer_id = active_presentation_buffer_id(surface_id);
    if (!active_buffer_id.has_value()) {
        auto surface = surface_state(surface_id);
        if (!surface.has_value())
            return Error::from_string_literal("Painter: no surface for paint surface fallback");
        return Gfx::PaintingSurface::create_with_size(surface->buffer_size, Gfx::BitmapFormat::BGRA8888, Gfx::BitmapAlpha::Premultiplied);
    }

    if (is_cpu_painting()) {
        auto it = m_cpu_backed_presentation_buffers.find(*active_buffer_id);
        if (it == m_cpu_backed_presentation_buffers.end())
            return Error::from_string_literal("Painter: active software presentation buffer id not registered");

        return TRY(get_or_create_paint_surface(surface_id, active_buffer_id, [&]() -> ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> {
            return Gfx::PaintingSurface::wrap_bitmap(*it->value);
        }));
    }

    return acquire_gpu_backed_paint_surface(surface_id, *active_buffer_id);
}

ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> Painter::acquire_gpu_backed_paint_surface(SurfaceID surface_id, u64 presentation_buffer_id)
{
    auto context = TRY(skia_context());

    auto it = m_gpu_backed_presentation_buffers.find(presentation_buffer_id);
    if (it == m_gpu_backed_presentation_buffers.end())
        return Error::from_string_literal("Painter: active GPU presentation buffer id not registered");

    return TRY(get_or_create_paint_surface(surface_id, presentation_buffer_id, [&]() -> ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> {
        return it->value.create_painting_surface(context, Gfx::PaintingSurface::Origin::TopLeft);
    }));
}

ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> Painter::get_or_create_paint_surface(SurfaceID surface_id, Optional<u64> cache_key, Function<ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>>()> create)
{
    auto existing = m_paint_surfaces.get(surface_id);
    bool const needs_recreate = !existing.has_value() || existing->key != cache_key || !existing->surface;
    if (needs_recreate) {
        NonnullRefPtr<Gfx::PaintingSurface> surface = TRY(create());
        m_paint_surfaces.set(surface_id, PaintSurfaceCacheEntry {
                                             .key = cache_key,
                                             .surface = surface,
                                         });
        return surface;
    }

    return NonnullRefPtr<Gfx::PaintingSurface>(*existing->surface);
}

void Painter::note_registered_presentation_buffer(SurfaceID surface_id, u64 image_id)
{
    m_presentation_buffer_surface_ids.set(image_id, surface_id);
}

bool Painter::has_registered_presentation_buffer(u64 image_id) const
{
    if (is_cpu_painting())
        return m_cpu_backed_presentation_buffers.contains(image_id);
    return m_gpu_backed_presentation_buffers.contains(image_id);
}

Optional<u64> Painter::active_presentation_buffer_id(SurfaceID surface_id) const
{
    return m_surface_active_presentation_buffer.get(surface_id);
}

void Painter::remove_paint_surface(SurfaceID surface_id)
{
    m_paint_surfaces.remove(surface_id);
}

void Painter::unregister_presentation_buffer(u64 image_id)
{
    if (is_cpu_painting()) {
        m_cpu_backed_presentation_buffers.remove(image_id);
        return;
    }

    m_gpu_backed_presentation_buffers.remove(image_id);
}

sk_sp<SkImage> Painter::snapshot_shared_image(Gfx::SharedImage& shared_image)
{
    auto context = skia_context();
    if (context.is_error())
        return nullptr;

    auto painting_surface = shared_image.create_painting_surface(context.release_value(), Gfx::PaintingSurface::Origin::TopLeft);
    return painting_surface->sk_image_snapshot<sk_sp<SkImage>>();
}

ErrorOr<void> Painter::draw_scaled_image(DrawContext const& draw_context, DrawScaledImageCommand const& command, SkCanvas& canvas, sk_sp<SkImageFilter> const& image_filter)
{
    ImageID image_id = command.image_id;

    if (image_id == 0) {
        if (is_logging_enabled(LOG_RESOURCE)) {
            dbgln("DrawScaledImage: unresolved image {} connection_id={} image_id={} dst={}x{}@{},{} clip={}x{}@{},{}",
                command.image_resource_id,
                draw_context.connection_id,
                command.image_id,
                command.dst_rect.width(),
                command.dst_rect.height(),
                command.dst_rect.x(),
                command.dst_rect.y(),
                command.clip_rect.width(),
                command.clip_rect.height(),
                command.clip_rect.x(),
                command.clip_rect.y());
        }
        draw_missing_image_placeholder(command.dst_rect, command.clip_rect, canvas);
        return {};
    }

    if (!draw_context.image_resolver)
        return Error::from_string_literal("DrawScaledImage command has no image resolver");

    auto sk_image = draw_context.image_resolver->operator()(command.image_resource_id, command.image_id);
    if (!sk_image) {
        log_missing_image(draw_context.connection_id, image_id, MissingImageKind::Scaled, command.dst_rect, command.clip_rect);
        draw_missing_image_placeholder(command.dst_rect, command.clip_rect, canvas);
        return {};
    }

    if (command.scaling_mode > to_underlying(Gfx::ScalingMode::NearestNeighbor))
        return Error::from_string_literal("DrawScaledImage command has invalid scaling mode");
    if (command.compositing_and_blending_operator > to_underlying(Gfx::CompositingAndBlendingOperator::PlusLighter))
        return Error::from_string_literal("DrawScaledImage command has invalid compositing operator");

    auto scaling_mode = static_cast<Gfx::ScalingMode>(command.scaling_mode);
    auto compositing_and_blending_operator = static_cast<Gfx::CompositingAndBlendingOperator>(command.compositing_and_blending_operator);
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setAlphaf(command.opacity);
    if (compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal)
        paint.setBlender(Gfx::to_skia_blender(compositing_and_blending_operator));
    if (image_filter)
        paint.setImageFilter(image_filter);
    canvas.save();
    if (!image_filter)
        canvas.clipRect(SkRect::MakeXYWH(command.clip_rect.x(), command.clip_rect.y(), command.clip_rect.width(), command.clip_rect.height()), true);
    SkRect src_rect = SkRect::MakeXYWH(command.src_rect.x(), command.src_rect.y(), command.src_rect.width(), command.src_rect.height());
    SkSamplingOptions sampling_options = Gfx::to_skia_sampling_options(scaling_mode);
    if (auto scaled_image = scaled_image_for_mipmap(*sk_image, command.src_rect, command.dst_rect, scaling_mode)) {
        sk_image = move(scaled_image);
        src_rect = SkRect::MakeWH(static_cast<SkScalar>(sk_image->width()), static_cast<SkScalar>(sk_image->height()));
        sampling_options = SkSamplingOptions();
    } else {
        sk_image = image_for_scaling_mode(move(sk_image), scaling_mode);
    }
    canvas.drawImageRect(sk_image, src_rect, SkRect::MakeXYWH(command.dst_rect.x(), command.dst_rect.y(), command.dst_rect.width(), command.dst_rect.height()), sampling_options, &paint, SkCanvas::kStrict_SrcRectConstraint);
    canvas.restore();
    return {};
}

ErrorOr<void> Painter::draw_external_content(DrawContext const& draw_context, DrawExternalContentCommand const& command, SkCanvas& canvas)
{
    ImageID image_id = command.image_id;

    if (image_id == 0) {
        if (is_logging_enabled(LOG_RESOURCE)) {
            dbgln("DrawExternalContent: unresolved image {} connection_id={} image_id={} dst={}x{}@{},{} clip={}x{}@{},{}",
                command.image_resource_id,
                draw_context.connection_id,
                command.image_id,
                command.dst_rect.width(),
                command.dst_rect.height(),
                command.dst_rect.x(),
                command.dst_rect.y(),
                command.clip_rect.width(),
                command.clip_rect.height(),
                command.clip_rect.x(),
                command.clip_rect.y());
        }
        draw_missing_image_placeholder(command.dst_rect, command.clip_rect, canvas);
        return {};
    }

    if (!draw_context.image_resolver)
        return Error::from_string_literal("DrawExternalContent command has no image resolver");

    auto sk_image = draw_context.image_resolver->operator()(command.image_resource_id, command.image_id);
    if (!sk_image) {
        log_missing_image(draw_context.connection_id, image_id, MissingImageKind::ExternalContent, command.dst_rect, command.clip_rect);
        draw_missing_image_placeholder(command.dst_rect, command.clip_rect, canvas);
        return {};
    }

    if (command.scaling_mode > to_underlying(Gfx::ScalingMode::NearestNeighbor))
        return Error::from_string_literal("DrawExternalContent command has invalid scaling mode");

    auto scaling_mode = static_cast<Gfx::ScalingMode>(command.scaling_mode);
    SkPaint paint;
    paint.setAntiAlias(true);
    canvas.save();
    canvas.clipRect(SkRect::MakeXYWH(command.clip_rect.x(), command.clip_rect.y(), command.clip_rect.width(), command.clip_rect.height()), true);
    sk_image = image_for_scaling_mode(move(sk_image), scaling_mode);
    canvas.drawImageRect(sk_image, SkRect::MakeXYWH(command.dst_rect.x(), command.dst_rect.y(), command.dst_rect.width(), command.dst_rect.height()), Gfx::to_skia_sampling_options(scaling_mode), &paint);
    canvas.restore();
    return {};
}

ErrorOr<void> Painter::draw_repeated_image(DrawContext const& draw_context, DrawRepeatedImageCommand const& command, SkCanvas& canvas)
{
    ImageID image_id = command.image_id;

    if (image_id == 0) {
        if (is_logging_enabled(LOG_RESOURCE)) {
            dbgln("DrawRepeatedImage: unresolved image {} connection_id={} image_id={} dst={}x{}@{},{} clip={}x{}@{},{}",
                command.image_resource_id,
                draw_context.connection_id,
                command.image_id,
                command.dst_rect.width(),
                command.dst_rect.height(),
                command.dst_rect.x(),
                command.dst_rect.y(),
                command.clip_rect.width(),
                command.clip_rect.height(),
                command.clip_rect.x(),
                command.clip_rect.y());
        }
        draw_missing_image_placeholder(command.dst_rect, command.clip_rect, canvas);
        return {};
    }

    if (!draw_context.image_resolver)
        return Error::from_string_literal("DrawRepeatedImage command has no image resolver");

    auto sk_image = draw_context.image_resolver->operator()(command.image_resource_id, command.image_id);
    if (!sk_image) {
        log_missing_image(draw_context.connection_id, image_id, MissingImageKind::Repeated, command.dst_rect, command.clip_rect);
        draw_missing_image_placeholder(command.dst_rect, command.clip_rect, canvas);
        return {};
    }

    if (command.scaling_mode > to_underlying(Gfx::ScalingMode::NearestNeighbor))
        return Error::from_string_literal("DrawRepeatedImage command has invalid scaling mode");

    f32 const image_width = static_cast<f32>(sk_image->width());
    f32 const image_height = static_cast<f32>(sk_image->height());
    if (image_width <= 0.0f || image_height <= 0.0f)
        return {};

    auto scaling_mode = static_cast<Gfx::ScalingMode>(command.scaling_mode);
    SkMatrix matrix;
    matrix.setScale(command.dst_rect.width() / image_width, command.dst_rect.height() / image_height);
    matrix.postTranslate(command.dst_rect.x(), command.dst_rect.y());

    sk_image = image_for_scaling_mode(move(sk_image), scaling_mode);
    auto shader = sk_image->makeShader(
        command.repeat_x != 0 ? SkTileMode::kRepeat : SkTileMode::kDecal,
        command.repeat_y != 0 ? SkTileMode::kRepeat : SkTileMode::kDecal,
        Gfx::to_skia_sampling_options(scaling_mode),
        matrix);
    if (!shader)
        return {};

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setShader(shader);
    canvas.save();
    canvas.clipRect(SkRect::MakeXYWH(command.clip_rect.x(), command.clip_rect.y(), command.clip_rect.width(), command.clip_rect.height()), true);
    canvas.drawPaint(paint);
    canvas.restore();
    return {};
}

Painter::SubmitResult Painter::submit_commands(RenderContext const& render_context, FrameHeader const& header, ReadonlyBytes payload, ReleaseToken release_token)
{
    auto submit_failure = [&](StringView reason) {
        dbgln("{}: submit_commands failed reason={} client={} surface={} viewport={}x{} payload_size={} presentation_buffer_id={} release_token={} payload_generation={}",
            backend_name(),
            reason,
            render_context.draw_context.connection_id,
            render_context.draw_context.surface_id,
            header.viewport_size.width(),
            header.viewport_size.height(),
            payload.size(),
            render_context.presentation_buffer_id.value_or(0),
            release_token,
            header.payload_generation);
        return Painter::SubmitResult {};
    };

    u32 const target_width = static_cast<u32>(render_context.target_size.width());
    u32 const target_height = static_cast<u32>(render_context.target_size.height());
    if (target_width == 0 || target_height == 0)
        return submit_failure("empty_paint_surface"sv);

    Optional<SurfaceState> surface;
    if (render_context.output_type == FrameOutputType::Presentation) {
        surface = surface_state(render_context.draw_context.surface_id);
        if (!surface.has_value())
            return submit_failure("surface_not_found"sv);

        if (target_width > static_cast<u32>(surface->buffer_size.width())
            || target_height > static_cast<u32>(surface->buffer_size.height())) {
            return submit_failure(ByteString::formatted("viewport_larger_than_surface_buffer viewport={}x{} logical_size={} buffer_size={}", target_width, target_height, surface->logical_size, surface->buffer_size));
        }
    }

    RefPtr<Gfx::Bitmap> offscreen_bitmap;
    auto presentation_surface_or_error = [&]() -> ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> {
        if (render_context.output_type == FrameOutputType::Offscreen) {
            if (header.offscreen_target.kind == OffscreenTargetKind::ContentImage) {
                if (!render_context.offscreen_content_image)
                    return Error::from_string_literal("missing_offscreen_content_image");
                if (is_cpu_painting() || header.offscreen_target.backend_preference == OffscreenBackendPreference::RequireCPU)
                    return Gfx::PaintingSurface::wrap_bitmap(*render_context.offscreen_content_image->bitmap());
                auto context = TRY(skia_context());
                return render_context.offscreen_content_image->create_painting_surface(context, Gfx::PaintingSurface::Origin::TopLeft);
            }

            VERIFY(header.offscreen_target.kind == OffscreenTargetKind::ShareableBitmap);
            auto bitmap_format = software_bitmap_format(Gfx::BitmapFormat::BGRA8888);
            if (!bitmap_format.has_value())
                return Error::from_string_literal("unsupported_offscreen_bitmap_format");
            auto offscreen_bitmap_or_error = Gfx::Bitmap::create(bitmap_format.release_value(), Gfx::BitmapAlpha::Premultiplied, render_context.target_size);
            if (offscreen_bitmap_or_error.is_error())
                return offscreen_bitmap_or_error.release_error();
            offscreen_bitmap = offscreen_bitmap_or_error.release_value();
            return Gfx::PaintingSurface::wrap_bitmap(*offscreen_bitmap);
        }

        return acquire_paint_surface(render_context.draw_context.surface_id, render_context.presentation_buffer_id);
    }();
    if (presentation_surface_or_error.is_error()) {
        StringView reason = render_context.output_type == FrameOutputType::Offscreen ? "offscreen_surface_allocation_failed"sv : "acquire_paint_surface_failed"sv;
        return submit_failure(ByteString::formatted("{} error={}", reason, presentation_surface_or_error.error()));
    }
    NonnullRefPtr<Gfx::PaintingSurface> presentation_surface = presentation_surface_or_error.release_value();

    if (render_context.output_type == FrameOutputType::Offscreen && header.offscreen_target.kind == OffscreenTargetKind::ShareableBitmap) {
        VERIFY(offscreen_bitmap);
    }

    Gfx::IntSize const viewport_size { static_cast<int>(target_width), static_cast<int>(target_height) };
    Optional<NonnullRefPtr<Gfx::SkiaBackendContext>> backend_context;
    if (render_context.output_type == FrameOutputType::Presentation || (render_context.output_type == FrameOutputType::Offscreen && header.offscreen_target.kind == OffscreenTargetKind::ContentImage && !is_cpu_painting() && header.offscreen_target.backend_preference != OffscreenBackendPreference::RequireCPU)) {
        auto context = skia_context();
        if (context.is_error())
            return submit_failure(ByteString::formatted("skia_backend_context error={}", context.error()));
        backend_context = context.release_value();
    }

    if (!render_context.draw_context.image_resolver)
        return submit_failure("missing_image_resolver"sv);

    DrawImageResolver image_resolver = [&upstream_resolver = *render_context.draw_context.image_resolver, offscreen = render_context.output_type == FrameOutputType::Offscreen](ResourceID resource_id, ImageID image_id) {
        auto sk_image = upstream_resolver(resource_id, image_id);
        if (!offscreen || !sk_image)
            return sk_image;
        return sk_image->makeRasterImage();
    };
    DrawContext draw_context = render_context.draw_context;
    DrawScaledImagePainter draw_scaled_image_handler = [this, &draw_context](DrawScaledImageCommand const& command, SkCanvas& canvas, sk_sp<SkImageFilter> const& image_filter) { return draw_scaled_image(draw_context, command, canvas, image_filter); };
    DrawExternalContentPainter draw_external_content_handler = [this, &draw_context](DrawExternalContentCommand const& command, SkCanvas& canvas) { return draw_external_content(draw_context, command, canvas); };
    DrawRepeatedImagePainter draw_repeated_image_handler = [this, &draw_context](DrawRepeatedImageCommand const& command, SkCanvas& canvas) { return draw_repeated_image(draw_context, command, canvas); };
    draw_context.image_resolver = &image_resolver;
    DrawContext paint_context {
        .connection_id = draw_context.connection_id,
        .surface_id = draw_context.surface_id,
        .font_cache = draw_context.font_cache,
        .draw_scaled_image_painter = &draw_scaled_image_handler,
        .draw_external_content_painter = &draw_external_content_handler,
        .draw_repeated_image_painter = &draw_repeated_image_handler,
        .image_resolver = &image_resolver,
        .scroll_offset_resolver = draw_context.scroll_offset_resolver,
    };
    auto validated_payload = validate_display_list_payload(payload);
    if (validated_payload.is_error())
        return submit_failure("invalid_display_list_payload"sv);

    SkCanvas& presentation_canvas = presentation_surface->canvas();
    int const base_save_count = presentation_canvas.getSaveCount();
    presentation_canvas.clear(SK_ColorTRANSPARENT);
    presentation_canvas.save();
    presentation_canvas.clipRect(SkRect::MakeWH(static_cast<SkScalar>(viewport_size.width()), static_cast<SkScalar>(viewport_size.height())), true);

    auto paint_result = paint_display_list_payload(paint_context, payload, validated_payload.release_value(), presentation_canvas);
    presentation_canvas.restoreToCount(base_save_count);
    if (paint_result.is_error())
        return submit_failure(ByteString::formatted("paint_display_list_payload error={}", paint_result.error()));

    if (backend_context.has_value()) {
        Core::ElapsedTimer flush_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        backend_context.value()->flush_and_submit(&presentation_surface->sk_surface());
        if (is_logging_enabled(LOG_TIMING))
            dbgtrack("gpu flush ms"sv, static_cast<f32>(flush_timer.elapsed_time().to_microseconds()) / 1000.f, 5000);
    } else {
        presentation_surface->flush();
    }

    did_complete_submit(render_context.draw_context.surface_id, header, release_token);
    if (render_context.offscreen_content_image) {
        return Painter::SubmitResult {
            .operation_result = Painter::OperationResult::Completed,
            .offscreen_image_id = header.offscreen_target.image_id,
            .offscreen_content_image = render_context.offscreen_content_image->export_payload(),
            .offscreen_bitmap = {},
        };
    }
    if (offscreen_bitmap)
        return Painter::SubmitResult { .operation_result = Painter::OperationResult::Completed, .offscreen_image_id = 0, .offscreen_content_image = {}, .offscreen_bitmap = offscreen_bitmap->to_shareable_bitmap() };
    return Painter::SubmitResult { .operation_result = Painter::OperationResult::Completed, .offscreen_image_id = 0, .offscreen_content_image = {}, .offscreen_bitmap = {} };
}

void Painter::log_missing_image(ConnectionID connection_id, u64 image_id, MissingImageKind kind, Gfx::FloatRect const& dst_rect, Gfx::FloatRect const& clip_rect) const
{
    if (!is_logging_enabled(LOG_RESOURCE))
        return;
    StringView command_name = "DrawScaledImage"sv;
    switch (kind) {
    case MissingImageKind::Scaled:
        command_name = "DrawScaledImage"sv;
        break;
    case MissingImageKind::ExternalContent:
        command_name = "DrawExternalContent"sv;
        break;
    case MissingImageKind::Repeated:
        command_name = "DrawRepeatedImage"sv;
        break;
    }

    dbgln("{}: {} miss client={} image_id={} dst={}x{}@{},{} clip={}x{}@{},{}",
        backend_name(),
        command_name,
        connection_id,
        image_id,
        dst_rect.width(),
        dst_rect.height(),
        dst_rect.x(),
        dst_rect.y(),
        clip_rect.width(),
        clip_rect.height(),
        clip_rect.x(),
        clip_rect.y());
}

}
