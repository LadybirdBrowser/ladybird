/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/GenericShorthands.h>
#include <AK/Math.h>
#include <Compositor/Canvas2DCommandPlayer.h>
#include <Compositor/FontSkia.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/SkiaUtils.h>
#include <LibCompositing/DisplayList/Canvas2DTape.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Font/Font.h>
#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkFont.h>
#include <core/SkImage.h>
#include <core/SkPaint.h>
#include <core/SkPath.h>
#include <core/SkPathBuilder.h>
#include <core/SkSurface.h>
#include <effects/SkDashPathEffect.h>
#include <effects/SkGradient.h>
#include <effects/SkImageFilters.h>

namespace Compositor {

using namespace Compositing;

ErrorOr<NonnullOwnPtr<Canvas2DCommandPlayer>> Canvas2DCommandPlayer::create(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, Gfx::IntSize size, Gfx::BitmapFormat format, Gfx::AlphaType alpha_type, Gfx::ColorSpace const& color_space, Resolvers resolvers)
{
    auto surface = TRY(Gfx::PaintingSurface::try_create_with_size(size, format, alpha_type, move(skia_backend_context), color_space));
    return make<Canvas2DCommandPlayer>(move(surface), color_space, move(resolvers));
}

Canvas2DCommandPlayer::Canvas2DCommandPlayer(NonnullRefPtr<Gfx::PaintingSurface> surface, Gfx::ColorSpace const& color_space, Resolvers resolvers)
    : m_color_space(color_space)
    , m_surface(move(surface))
    , m_initial_save_count(m_surface->canvas().save())
    , m_resolvers(move(resolvers))
{
}

Canvas2DCommandPlayer::~Canvas2DCommandPlayer() = default;

sk_sp<SkImage> Canvas2DCommandPlayer::image_of_frame(Gfx::DecodedImageFrame const& frame) const
{
    if (auto skia_backend_context = m_surface->skia_backend_context()) {
        if (auto image = Gfx::sk_texture_image_sharing_pixels_of_decoded_image_frame(frame, *skia_backend_context))
            return image;
    }
    return Gfx::sk_image_from_decoded_image_frame(frame);
}

NonnullRefPtr<Gfx::PaintingSurface> Canvas2DCommandPlayer::surface() const
{
    return m_surface;
}

void Canvas2DCommandPlayer::clear(Gfx::Color color)
{
    clear_rect({ {}, m_surface->size().to_type<float>() }, color);
}

void Canvas2DCommandPlayer::play(ReadonlyBytes tape)
{
    for_each_canvas_2d_command(tape, [&](DisplayListCommandType command_type, ReadonlyBytes payload) {
        m_payload = payload;
        switch (command_type) {
#define PLAY_CANVAS_2D_COMMAND(command_type)                                        \
    case DisplayListCommandType::command_type:                                      \
        play_command(read_display_list_object<Compositing::command_type>(payload)); \
        break;
            ENUMERATE_CANVAS_2D_COMMANDS(PLAY_CANVAS_2D_COMMAND)
#undef PLAY_CANVAS_2D_COMMAND
        default:
            // NB: Validation lets only the commands of the canvas 2D stream onto the tape of a canvas.
            VERIFY_NOT_REACHED();
        }
    });
    m_payload = {};
}

ReadonlyBytes Canvas2DCommandPlayer::inline_data(DisplayListDataSpan span) const
{
    return m_payload.slice(span.offset, span.size);
}

// NB: The tape is not aligned for the values of an array span, so they are copied out.
template<typename T>
static Vector<T> inline_objects(ReadonlyBytes bytes)
{
    Vector<T> objects;
    objects.resize(bytes.size() / sizeof(T));
    if (!objects.is_empty())
        __builtin_memcpy(objects.data(), bytes.data(), objects.size() * sizeof(T));
    return objects;
}

SkPath Canvas2DCommandPlayer::path_from(DisplayListDataSpan path_data) const
{
    return Gfx::skia_path_from_serialized_bytes(inline_data(path_data));
}

sk_sp<SkImageFilter> Canvas2DCommandPlayer::image_filter_from(Canvas2DCompositing const& compositing) const
{
    if (!compositing.has_filter)
        return nullptr;
    return Gfx::to_skia_image_filter(inline_data(compositing.filter), [](u64) -> Gfx::DecodedImageFrame const& {
        // A canvas filter comes from CSS filter functions, which draw no images.
        VERIFY_NOT_REACHED();
    });
}

SkColorSpace* Canvas2DCommandPlayer::surface_color_space() const
{
    return m_surface->sk_surface().imageInfo().colorSpace();
}

Optional<SkPaint> Canvas2DCommandPlayer::paint_from(Canvas2DPaint const& style, Canvas2DCompositing const& compositing) const
{
    SkPaint paint;
    auto* color_space = surface_color_space();

    auto set_shader_with_color_stops = [&](auto make_shader) {
        auto stop_colors = inline_objects<Gfx::Color>(inline_data(style.color_stops.colors));
        auto positions = inline_objects<float>(inline_data(style.color_stops.positions));
        Vector<SkColor4f> colors;
        colors.ensure_capacity(stop_colors.size());
        for (auto const& color : stop_colors)
            colors.unchecked_append(Gfx::to_skia_color4f(color));
        SkGradient gradient { SkGradient::Colors { { colors.data(), colors.size() }, { positions.data(), positions.size() }, SkTileMode::kClamp, sk_ref_sp(color_space) }, {} };
        paint.setShader(make_shader(gradient));
    };

    switch (style.kind) {
    case Canvas2DPaintKind::Color:
        paint.setColor(Gfx::to_skia_color4f(style.color), color_space);
        break;
    case Canvas2DPaintKind::LinearGradient: {
        // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createlineargradient
        // If x0 = x1 and y0 = y1, then the linear gradient must paint nothing.
        if (style.start_point == style.end_point) {
            paint.setColor(SK_ColorTRANSPARENT);
            break;
        }
        Array points { Gfx::to_skia_point(style.start_point), Gfx::to_skia_point(style.end_point) };
        set_shader_with_color_stops([&](SkGradient const& gradient) {
            return SkShaders::LinearGradient(points.data(), gradient);
        });
        break;
    }
    case Canvas2DPaintKind::RadialGradient:
        // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createradialgradient
        // 1. If x0 = x1 and y0 = y1 and r0 = r1, then the radial gradient must paint nothing. Return.
        if (style.start_point == style.end_point && style.start_radius == style.end_radius) {
            paint.setColor(SK_ColorTRANSPARENT);
            break;
        }
        set_shader_with_color_stops([&](SkGradient const& gradient) {
            return SkShaders::TwoPointConicalGradient(Gfx::to_skia_point(style.start_point), style.start_radius, Gfx::to_skia_point(style.end_point), style.end_radius, gradient);
        });
        break;
    case Canvas2DPaintKind::ConicGradient: {
        // NB: The angles of a Skia sweep gradient pick where its colors start and end, and every angle outside of them
        //     takes the color of the nearest end. So the gradient is rotated to its start angle instead.
        SkMatrix rotation_to_start_angle;
        rotation_to_start_angle.setRotate(AK::to_degrees(style.start_angle), style.start_point.x(), style.start_point.y());
        set_shader_with_color_stops([&](SkGradient const& gradient) {
            return SkShaders::SweepGradient(Gfx::to_skia_point(style.start_point), 0, 360, gradient, &rotation_to_start_angle);
        });
        break;
    }
    case Canvas2DPaintKind::Pattern: {
        if (!style.pattern_has_image)
            break;
        auto const* frame = m_resolvers.image_frame(style.pattern_image_id.value());
        if (!frame)
            return {};
        auto image = image_of_frame(*frame);
        if (!image)
            break;

        auto repeat_x = first_is_one_of(style.pattern_repetition, Canvas2DPatternRepetition::Repeat, Canvas2DPatternRepetition::RepeatX);
        auto repeat_y = first_is_one_of(style.pattern_repetition, Canvas2DPatternRepetition::Repeat, Canvas2DPatternRepetition::RepeatY);

        // FIXME: Implement sampling configuration.
        SkSamplingOptions sampling_options { SkFilterMode::kLinear };
        Optional<SkMatrix> pattern_matrix;
        if (style.pattern_transform.has_value())
            pattern_matrix = Gfx::to_skia_matrix(*style.pattern_transform);
        paint.setShader(image->makeShader(
            repeat_x ? SkTileMode::kRepeat : SkTileMode::kDecal,
            repeat_y ? SkTileMode::kRepeat : SkTileMode::kDecal,
            sampling_options,
            pattern_matrix.has_value() ? &pattern_matrix.value() : nullptr));
        break;
    }
    }

    paint.setAlphaf(paint.getAlphaf() * compositing.global_alpha);
    paint.setImageFilter(image_filter_from(compositing));
    return paint;
}

// NB: These operators change the destination even where the source is fully transparent, which makes them affect
//     pixels that the source does not cover.
static bool affects_pixels_not_covered_by_source(Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    return first_is_one_of(compositing_and_blending_operator,
        Gfx::CompositingAndBlendingOperator::Copy,
        Gfx::CompositingAndBlendingOperator::SourceIn,
        Gfx::CompositingAndBlendingOperator::SourceOut,
        Gfx::CompositingAndBlendingOperator::DestinationIn,
        Gfx::CompositingAndBlendingOperator::DestinationATop);
}

// https://html.spec.whatwg.org/multipage/canvas.html#drawing-model
template<typename Callback>
static void draw_with_compositing(SkCanvas& canvas, SkPaint& paint, Canvas2DCompositing const& compositing, Callback draw)
{
    auto compositing_and_blending_operator = compositing.compositing_and_blending_operator;

    // 5. When shadows are drawn, render the shadow from image B, using the current shadow styles, creating image C.
    // 6. When shadows are drawn, composite C within the clipping region over the current output bitmap using the
    //    current compositing and blending operator.
    // NB: The current transformation matrix does not affect the offset and the blur of a shadow, so the source is
    //     drawn into a layer whose filter makes the shadow in the coordinates of the canvas. The paint carries the
    //     global alpha and the filter of the context, so the layer holds image B.
    if (compositing.has_shadow) {
        auto const& shadow = compositing.shadow;
        auto matrix = canvas.getTotalMatrix();
        canvas.save();
        canvas.resetMatrix();
        auto sigma = shadow.blur / 2;
        SkPaint shadow_layer_paint;
        shadow_layer_paint.setImageFilter(SkImageFilters::DropShadowOnly(shadow.offset_x, shadow.offset_y, sigma, sigma, Gfx::to_skia_color4f(shadow.color), sk_ref_sp(canvas.imageInfo().colorSpace()), nullptr));
        shadow_layer_paint.setBlender(Gfx::to_skia_blender(compositing_and_blending_operator));
        canvas.saveLayer(nullptr, &shadow_layer_paint);
        canvas.setMatrix(matrix);
        draw(canvas, paint);
        canvas.restore();
        canvas.restore();
    }

    // 7. Composite B within the clipping region over the current output bitmap using the current compositing and
    //    blending operator.
    // NB: A Skia blend mode only touches the pixels that a draw covers. For the operators that also affect uncovered
    //     pixels, the source is drawn into a transparent layer, which is then composited onto all of the destination
    //     inside the clip.
    if (!affects_pixels_not_covered_by_source(compositing_and_blending_operator)) {
        paint.setBlender(Gfx::to_skia_blender(compositing_and_blending_operator));
        draw(canvas, paint);
        return;
    }

    SkPaint layer_paint;
    layer_paint.setBlender(Gfx::to_skia_blender(compositing_and_blending_operator));
    canvas.saveLayer(nullptr, &layer_paint);
    draw(canvas, paint);
    canvas.restore();
}

// https://html.spec.whatwg.org/multipage/canvas.html#trace-a-path
static SkPath prune_path_for_stroking(SkPath const& path)
{
    // 2. Prune all zero-length line segments from path.
    // 3. Remove from path any subpaths containing no lines (i.e. subpaths with just one point).
    // NB: Skia strokes a zero-length segment with caps, so such segments and the subpaths that are left with no
    //     lines are removed before the path is stroked.
    auto is_zero_length = [](SkSpan<SkPoint const> points) {
        for (size_t i = 1; i < points.size(); ++i) {
            if (points[i] != points[0])
                return false;
        }
        return true;
    };

    SkPathBuilder builder(path.getFillType());
    bool subpath_has_lines = false;
    auto iterator = path.iter();
    while (auto record = iterator.next()) {
        auto points = record->fPoints;
        switch (record->fVerb) {
        case SkPathVerb::kMove:
            subpath_has_lines = false;
            continue;
        case SkPathVerb::kClose:
            if (subpath_has_lines)
                builder.close();
            subpath_has_lines = false;
            continue;
        case SkPathVerb::kLine:
        case SkPathVerb::kQuad:
        case SkPathVerb::kConic:
        case SkPathVerb::kCubic:
            break;
        }

        if (is_zero_length(points))
            continue;
        if (!subpath_has_lines) {
            builder.moveTo(points[0]);
            subpath_has_lines = true;
        }
        switch (record->fVerb) {
        case SkPathVerb::kLine:
            builder.lineTo(points[1]);
            break;
        case SkPathVerb::kQuad:
            builder.quadTo(points[1], points[2]);
            break;
        case SkPathVerb::kConic:
            builder.conicTo(points[1], points[2], record->conicWeight());
            break;
        case SkPathVerb::kCubic:
            builder.cubicTo(points[1], points[2], points[3]);
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
    return builder.detach();
}

void Canvas2DCommandPlayer::play_command(Canvas2DFillPath const& command)
{
    auto path = path_from(command.path_data);
    path.setFillType(Gfx::to_skia_path_fill_type(command.winding_rule));
    auto paint = paint_from(command.paint, command.compositing);
    if (!paint.has_value())
        return;
    paint->setAntiAlias(true);
    draw_with_compositing(m_surface->canvas(), *paint, command.compositing, [&](SkCanvas& canvas, SkPaint const& paint) {
        canvas.drawPath(path, paint);
    });
}

void Canvas2DCommandPlayer::play_command(Canvas2DStrokePath const& command)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (command.thickness <= 0)
        return;

    auto path = prune_path_for_stroking(path_from(command.path_data));
    auto dash_array = inline_objects<float>(inline_data(command.dash_array));
    auto paint = paint_from(command.paint, command.compositing);
    if (!paint.has_value())
        return;
    paint->setAntiAlias(true);
    paint->setStyle(SkPaint::Style::kStroke_Style);
    paint->setStrokeWidth(command.thickness);
    paint->setStrokeCap(Gfx::to_skia_cap(command.cap_style));
    paint->setStrokeJoin(Gfx::to_skia_join(command.join_style));
    paint->setStrokeMiter(command.miter_limit);
    paint->setPathEffect(SkDashPathEffect::Make({ dash_array.data(), dash_array.size() }, command.dash_offset));
    draw_with_compositing(m_surface->canvas(), *paint, command.compositing, [&](SkCanvas& canvas, SkPaint const& paint) {
        canvas.drawPath(path, paint);
    });
}

void Canvas2DCommandPlayer::play_command(Canvas2DDrawGlyphRuns const& command)
{
    if (!m_resolvers.font)
        return;
    auto paint = paint_from(command.paint, command.compositing);
    if (!paint.has_value())
        return;

    struct SkiaGlyphRun {
        SkFont font;
        Vector<SkGlyphID, 32> glyph_ids;
        Vector<SkPoint, 32> glyph_positions;
    };

    auto tape_glyphs = inline_objects<DisplayListGlyph>(inline_data(command.glyphs));
    auto tape_runs = inline_objects<Canvas2DGlyphRun>(inline_data(command.runs));
    Vector<SkiaGlyphRun, 2> skia_runs;
    skia_runs.ensure_capacity(tape_runs.size());
    size_t first_glyph = 0;
    for (auto const& run : tape_runs) {
        // NB: The tape validation guarantees that the runs split the glyphs exactly.
        auto run_glyphs = tape_glyphs.span().slice(first_glyph, run.glyph_count);
        first_glyph += run.glyph_count;
        auto const* font = m_resolvers.font(run.font_id.value());
        if (!font || run_glyphs.is_empty() || font->is_invisible())
            continue;

        auto sk_font = skia_font(*font, 1);
        if (!sk_font.has_value())
            continue;

        SkiaGlyphRun skia_run { .font = sk_font.release_value(), .glyph_ids = {}, .glyph_positions = {} };

        // NB: Preserve canvas text's fractional baseline and unhinted outline geometry.
        skia_run.font.setHinting(SkFontHinting::kNone);
        skia_run.font.setForceAutoHinting(false);
        skia_run.font.setBaselineSnap(false);

        // NB: The scale goes on the font rather than the canvas, as it must not scale the paint of the text.
        skia_run.font.setScaleX(skia_run.font.getScaleX() * command.horizontal_scale);

        skia_run.glyph_ids.ensure_capacity(run_glyphs.size());
        skia_run.glyph_positions.ensure_capacity(run_glyphs.size());
        auto font_ascent = font->pixel_metrics().ascent;
        for (auto const& glyph : run_glyphs) {
            skia_run.glyph_ids.unchecked_append(static_cast<SkGlyphID>(glyph.glyph_id));
            skia_run.glyph_positions.unchecked_append({ glyph.position.x() * command.horizontal_scale, glyph.position.y() + font_ascent });
        }
        skia_runs.unchecked_append(move(skia_run));
    }

    // NB: Not drawTextBlob(). Skia caches a drawn SkTextBlob once per subpixel origin, and finds a cached entry by
    //     scanning every entry of that blob. Canvas text is often redrawn at moving fractional positions, which
    //     piles up thousands of entries per blob and makes each draw scan all of them.
    draw_with_compositing(m_surface->canvas(), *paint, command.compositing, [&](SkCanvas& canvas, SkPaint const& paint) {
        auto draw_runs = [&](SkPaint const& run_paint) {
            for (auto const& run : skia_runs)
                canvas.drawGlyphs({ run.glyph_ids.data(), run.glyph_ids.size() }, { run.glyph_positions.data(), run.glyph_positions.size() }, Gfx::to_skia_point(command.translation), run.font, run_paint);
        };

        if (skia_runs.size() <= 1 || !paint.getImageFilter()) {
            draw_runs(paint);
            return;
        }

        // NB: The filter of the context applies to the text as a whole, so it goes on a layer that holds all runs.
        //     Otherwise, what the filter makes of one run would land on top of the other runs.
        SkPaint layer_paint;
        layer_paint.setImageFilter(paint.refImageFilter());
        layer_paint.setBlender(paint.refBlender());
        SkPaint run_paint = paint;
        run_paint.setImageFilter(nullptr);
        run_paint.setBlender(nullptr);
        canvas.saveLayer(nullptr, &layer_paint);
        draw_runs(run_paint);
        canvas.restore();
    });
}

void Canvas2DCommandPlayer::clear_rect(Gfx::FloatRect const& rect, Gfx::Color color)
{
    auto& canvas = m_surface->canvas();
    canvas.save();
    canvas.clipRect(Gfx::to_skia_rect(rect));
    SkPaint paint;
    paint.setColor(Gfx::to_skia_color4f(color), surface_color_space());
    paint.setBlendMode(SkBlendMode::kSrc);
    canvas.drawPaint(paint);
    canvas.restore();
}

void Canvas2DCommandPlayer::play_command(Canvas2DClearRect const& command)
{
    clear_rect(command.rect, command.color);
}

void Canvas2DCommandPlayer::play_command(Canvas2DDrawImageFrame const& command)
{
    auto const* frame = m_resolvers.image_frame(command.frame_id.value());
    if (!frame)
        return;
    auto image = image_of_frame(*frame);
    if (!image)
        return;
    draw_image(*image, command.dst_rect, command.src_rect, command.scaling_mode, command.compositing);
}

void Canvas2DCommandPlayer::play_command(Canvas2DDrawCanvas const& command)
{
    if (!m_resolvers.canvas_surface)
        return;

    auto* source_surface = m_resolvers.canvas_surface(command.source_canvas_id.value());
    if (!source_surface)
        return;

    auto image = source_surface->sk_image_snapshot<sk_sp<SkImage>>();
    if (!image)
        return;
    draw_image(*image, command.dst_rect, command.src_rect, command.scaling_mode, command.compositing);
}

void Canvas2DCommandPlayer::play_command(Canvas2DDrawVideoFrame const& command)
{
    if (!m_resolvers.video_frame_image)
        return;
    auto image = m_resolvers.video_frame_image(command.video_sink_handle, command.generation);
    if (!image)
        return;
    draw_image(*image, command.dst_rect, command.src_rect, command.scaling_mode, command.compositing);
}

void Canvas2DCommandPlayer::draw_image(SkImage const& image, Gfx::FloatRect const& dst_rect, Gfx::FloatRect const& src_rect, Gfx::ScalingMode scaling_mode, Canvas2DCompositing const& compositing)
{
    SkPaint paint;
    paint.setImageFilter(image_filter_from(compositing));
    paint.setAlpha(static_cast<u8>(compositing.global_alpha * 255));

    draw_with_compositing(m_surface->canvas(), paint, compositing, [&](SkCanvas& canvas, SkPaint const& paint) {
        canvas.drawImageRect(
            &image,
            Gfx::to_skia_rect(src_rect),
            Gfx::to_skia_rect(dst_rect),
            Gfx::to_skia_sampling_options(scaling_mode),
            &paint,
            SkCanvas::kStrict_SrcRectConstraint);
    });
}

void Canvas2DCommandPlayer::play_command(Canvas2DSetTransform const& command)
{
    m_surface->canvas().setMatrix(Gfx::to_skia_matrix(command.transform));
}

void Canvas2DCommandPlayer::play_command(Canvas2DSave const&)
{
    m_surface->canvas().save();
}

void Canvas2DCommandPlayer::play_command(Canvas2DRestore const&)
{
    m_surface->canvas().restore();
}

void Canvas2DCommandPlayer::play_command(Canvas2DClipPath const& command)
{
    auto path = path_from(command.path_data);
    path.setFillType(Gfx::to_skia_path_fill_type(command.winding_rule));
    m_surface->canvas().clipPath(path, SkClipOp::kIntersect, true);
}

void Canvas2DCommandPlayer::play_command(Canvas2DReset const&)
{
    auto& canvas = m_surface->canvas();
    canvas.restoreToCount(m_initial_save_count);
    canvas.save();
}

void Canvas2DCommandPlayer::play_command(Canvas2DClearCanvas const& command)
{
    auto size = m_surface->size();
    if (size.is_empty())
        return;

    static constexpr int max_strip_height = 64;
    auto strip_height = min(size.height(), max_strip_height);
    auto strip_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { size.width(), strip_height });
    if (strip_or_error.is_error())
        return;
    auto strip = strip_or_error.release_value();
    auto alpha = command.color.alpha();
    auto premultiply = [alpha](u8 channel) { return static_cast<u8>((channel * alpha + 127) / 255); };
    auto pixel = Gfx::Color(premultiply(command.color.red()), premultiply(command.color.green()), premultiply(command.color.blue()), alpha).value();
    for (int y = 0; y < strip_height; ++y) {
        auto* scanline = strip->scanline(y);
        for (int x = 0; x < size.width(); ++x)
            scanline[x] = pixel;
    }

    for (int y = 0; y < size.height(); y += strip_height)
        m_surface->write_from_bitmap(*strip, { 0, y }, m_color_space);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-putimagedata
void Canvas2DCommandPlayer::play_command(Canvas2DPutImageFrame const& command)
{
    // NB: putImageData() sets the pixels of the output bitmap, so they are written to the surface directly, without
    //     the current transformation matrix, the clipping region, or compositing.
    auto const* frame = m_resolvers.image_frame(command.frame_id.value());
    if (!frame)
        return;
    m_surface->write_from_bitmap(*frame->bitmap_sharing_pixels(), command.position, frame->color_space());
}

}
