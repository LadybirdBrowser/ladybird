/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <AK/GenericShorthands.h>
#include <AK/String.h>
#include <AK/TypeCasts.h>
#include <Compositor/PainterSkia.h>
#include <Compositor/SkiaUtils.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Font/Font.h>

#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkFont.h>
#include <core/SkImage.h>
#include <core/SkPath.h>
#include <core/SkPathBuilder.h>
#include <core/SkSurface.h>
#include <effects/SkBlurMaskFilter.h>
#include <effects/SkDashPathEffect.h>
#include <effects/SkGradient.h>

namespace Gfx {

static sk_sp<SkImage> sk_image_for_decoded_image_frame(DecodedImageFrame const& frame)
{
    return sk_image_from_bitmap(frame.bitmap(), frame.color_space());
}

// NB: The colors of a canvas are in the color space of its surface.
static void set_color(SkPaint& paint, Color color, SkColorSpace* color_space)
{
    paint.setColor(to_skia_color4f(color), color_space);
}

static void apply_paint_style(SkPaint& paint, PaintStyle const& style, SkColorSpace* color_space)
{
    if (auto const& solid_color = as_if<SolidColorPaintStyle>(style)) {
        set_color(paint, solid_color->color(), color_space);
    } else if (auto const& linear_gradient = as_if<Gfx::CanvasLinearGradientPaintStyle>(style)) {
        // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createlineargradient
        // If x0 = x1 and y0 = y1, then the linear gradient must paint nothing.
        if (linear_gradient->start_point() == linear_gradient->end_point()) {
            paint.setColor(SK_ColorTRANSPARENT);
            return;
        }

        auto const& color_stops = linear_gradient->color_stops();

        Vector<SkColor4f> colors;
        colors.ensure_capacity(color_stops.size());
        Vector<SkScalar> positions;
        positions.ensure_capacity(color_stops.size());
        for (auto const& color_stop : color_stops) {
            colors.unchecked_append(to_skia_color4f(color_stop.color));
            positions.unchecked_append(color_stop.position);
        }

        Array points { to_skia_point(linear_gradient->start_point()), to_skia_point(linear_gradient->end_point()) };

        SkMatrix matrix;
        SkGradient gradient { SkGradient::Colors { { colors.data(), colors.size() }, { positions.data(), positions.size() }, SkTileMode::kClamp, sk_ref_sp(color_space) }, {} };
        auto shader = SkShaders::LinearGradient(points.data(), gradient, &matrix);
        paint.setShader(shader);
    } else if (auto const* radial_gradient = as_if<CanvasRadialGradientPaintStyle>(style)) {
        // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createradialgradient
        // 1. If x0 = x1 and y0 = y1 and r0 = r1, then the radial gradient must paint nothing. Return.
        if (radial_gradient->start_center() == radial_gradient->end_center() && radial_gradient->start_radius() == radial_gradient->end_radius()) {
            paint.setColor(SK_ColorTRANSPARENT);
            return;
        }

        auto const& color_stops = radial_gradient->color_stops();

        Vector<SkColor4f> colors;
        colors.ensure_capacity(color_stops.size());
        Vector<SkScalar> positions;
        positions.ensure_capacity(color_stops.size());
        for (auto const& color_stop : color_stops) {
            colors.unchecked_append(to_skia_color4f(color_stop.color));
            positions.unchecked_append(color_stop.position);
        }

        auto start_center = radial_gradient->start_center();
        auto end_center = radial_gradient->end_center();
        auto start_radius = radial_gradient->start_radius();
        auto end_radius = radial_gradient->end_radius();

        auto start_sk_point = to_skia_point(start_center);
        auto end_sk_point = to_skia_point(end_center);

        SkMatrix matrix;
        SkGradient gradient { SkGradient::Colors { { colors.data(), colors.size() }, { positions.data(), positions.size() }, SkTileMode::kClamp, sk_ref_sp(color_space) }, {} };
        auto shader = SkShaders::TwoPointConicalGradient(start_sk_point, start_radius, end_sk_point, end_radius, gradient, &matrix);
        paint.setShader(shader);
    } else if (auto const* conic_gradient = as_if<CanvasConicGradientPaintStyle>(style)) {
        auto const& color_stops = conic_gradient->color_stops();

        Vector<SkColor4f> colors;
        colors.ensure_capacity(color_stops.size());
        Vector<SkScalar> positions;
        positions.ensure_capacity(color_stops.size());
        for (auto const& color_stop : color_stops) {
            colors.unchecked_append(to_skia_color4f(color_stop.color));
            positions.unchecked_append(color_stop.position);
        }

        auto center = conic_gradient->center();
        auto start_angle_degrees = AK::to_degrees(conic_gradient->start_angle());

        SkMatrix matrix;
        SkGradient gradient { SkGradient::Colors { { colors.data(), colors.size() }, { positions.data(), positions.size() }, SkTileMode::kClamp, sk_ref_sp(color_space) }, {} };
        auto shader = SkShaders::SweepGradient(to_skia_point(center), start_angle_degrees, start_angle_degrees + 360, gradient, &matrix);
        paint.setShader(shader);
    } else if (auto const* canvas_pattern = as_if<CanvasPatternPaintStyle>(style)) {
        auto frame = canvas_pattern->image();
        if (!frame.has_value())
            return;
        auto sk_image = sk_image_for_decoded_image_frame(*frame);
        if (!sk_image)
            return;

        auto repetition = canvas_pattern->repetition();
        auto repeat_x = first_is_one_of(repetition, CanvasPatternPaintStyle::Repetition::Repeat, CanvasPatternPaintStyle::Repetition::RepeatX);
        auto repeat_y = first_is_one_of(repetition, CanvasPatternPaintStyle::Repetition::Repeat, CanvasPatternPaintStyle::Repetition::RepeatY);

        // FIXME: Implement sampling configuration.
        SkSamplingOptions sk_sampling_options { SkFilterMode::kLinear };
        Optional<SkMatrix> transformation_matrix;

        if (canvas_pattern->transform().has_value()) {
            auto const& transform = canvas_pattern->transform().value();
            transformation_matrix = SkMatrix::MakeAll(
                transform.a(), transform.c(), transform.e(),
                transform.b(), transform.d(), transform.f(),
                0, 0, 1);
        }
        auto shader = sk_image->makeShader(
            repeat_x ? SkTileMode::kRepeat : SkTileMode::kDecal,
            repeat_y ? SkTileMode::kRepeat : SkTileMode::kDecal,
            sk_sampling_options, transformation_matrix.has_value() ? &transformation_matrix.value() : nullptr);
        paint.setShader(move(shader));
    } else {
        dbgln("FIXME: Unsupported PaintStyle");
    }
}

static SkPaint to_skia_paint(Gfx::PaintStyle const& style, sk_sp<SkImageFilter> image_filter, SkColorSpace* color_space)
{
    SkPaint paint;

    apply_paint_style(paint, style, color_space);
    paint.setImageFilter(move(image_filter));

    return paint;
}

// NB: These operators change the destination even where the source is fully transparent, which makes them affect
//     pixels that the source does not cover.
static bool affects_pixels_not_covered_by_source(CompositingAndBlendingOperator compositing_and_blending_operator)
{
    return first_is_one_of(compositing_and_blending_operator,
        CompositingAndBlendingOperator::Copy,
        CompositingAndBlendingOperator::SourceIn,
        CompositingAndBlendingOperator::SourceOut,
        CompositingAndBlendingOperator::DestinationIn,
        CompositingAndBlendingOperator::DestinationATop);
}

// NB: A Skia blend mode only touches the pixels that a draw covers. For the operators that also affect uncovered
//     pixels, the source is drawn into a transparent layer, which is then composited onto all of the destination
//     inside the clip.
template<typename Callback>
static void draw_with_compositing_and_blending_operator(SkCanvas& canvas, SkPaint& paint, CompositingAndBlendingOperator compositing_and_blending_operator, Callback draw)
{
    if (!affects_pixels_not_covered_by_source(compositing_and_blending_operator)) {
        paint.setBlender(to_skia_blender(compositing_and_blending_operator));
        draw(canvas, paint);
        return;
    }

    SkPaint layer_paint;
    layer_paint.setBlender(to_skia_blender(compositing_and_blending_operator));
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

PainterSkia::PainterSkia(NonnullRefPtr<Gfx::PaintingSurface> painting_surface)
    : m_painting_surface(move(painting_surface))
{
    m_initial_save_count = m_painting_surface->canvas().save();
}

PainterSkia::~PainterSkia() = default;

SkColorSpace* PainterSkia::color_space() const
{
    return m_painting_surface->sk_surface().imageInfo().colorSpace();
}

void PainterSkia::clear_rect(Gfx::FloatRect const& rect, Gfx::Color color)
{
    auto& canvas = m_painting_surface->canvas();
    canvas.save();
    canvas.clipRect(to_skia_rect(rect));
    SkPaint paint;
    set_color(paint, color, color_space());
    paint.setBlendMode(SkBlendMode::kSrc);
    canvas.drawPaint(paint);
    canvas.restore();
}

void PainterSkia::fill_rect(Gfx::FloatRect const& rect, Color color)
{
    SkPaint paint;
    set_color(paint, color, color_space());
    auto& canvas = m_painting_surface->canvas();
    canvas.drawRect(to_skia_rect(rect), paint);
}

void PainterSkia::draw_bitmap(Gfx::FloatRect const& dst_rect, Gfx::DecodedImageFrame const& source, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode, sk_sp<SkImageFilter> image_filter, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    auto sk_image = sk_image_for_decoded_image_frame(source);
    if (!sk_image)
        return;
    draw_image(*sk_image, dst_rect, src_rect, scaling_mode, move(image_filter), global_alpha, compositing_and_blending_operator);
}

void PainterSkia::draw_image(SkImage const& image, Gfx::FloatRect const& dst_rect, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode, sk_sp<SkImageFilter> image_filter, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    SkPaint paint;
    paint.setImageFilter(move(image_filter));
    paint.setAlpha(static_cast<u8>(global_alpha * 255));

    draw_with_compositing_and_blending_operator(m_painting_surface->canvas(), paint, compositing_and_blending_operator, [&](SkCanvas& canvas, SkPaint const& paint) {
        canvas.drawImageRect(
            &image,
            to_skia_rect(src_rect),
            to_skia_rect(dst_rect),
            to_skia_sampling_options(scaling_mode),
            &paint,
            SkCanvas::kStrict_SrcRectConstraint);
    });
}

void PainterSkia::set_transform(Gfx::AffineTransform const& transform)
{
    auto matrix = SkMatrix::MakeAll(
        transform.a(), transform.c(), transform.e(),
        transform.b(), transform.d(), transform.f(),
        0, 0, 1);

    auto& canvas = m_painting_surface->canvas();
    canvas.setMatrix(matrix);
}

void PainterSkia::stroke_path(Gfx::Path const& path, Gfx::Color color, float thickness, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle cap_style, Gfx::Path::JoinStyle join_style, float miter_limit, Vector<float> const& dash_array, float dash_offset)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (thickness <= 0)
        return;

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur_radius / 2));
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(thickness);
    set_color(paint, color, color_space());
    paint.setStrokeCap(to_skia_cap(cap_style));
    paint.setStrokeJoin(to_skia_join(join_style));
    paint.setStrokeMiter(miter_limit);
    paint.setPathEffect(SkDashPathEffect::Make({ dash_array.data(), dash_array.size() }, dash_offset));
    auto sk_path = prune_path_for_stroking(to_skia_path(path));
    draw_with_compositing_and_blending_operator(m_painting_surface->canvas(), paint, compositing_and_blending_operator, [&](SkCanvas& canvas, SkPaint const& paint) {
        canvas.drawPath(sk_path, paint);
    });
}

void PainterSkia::stroke_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, sk_sp<SkImageFilter> image_filter, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle const& cap_style, Gfx::Path::JoinStyle const& join_style, float miter_limit, Vector<float> const& dash_array, float dash_offset)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (thickness <= 0)
        return;

    auto sk_path = prune_path_for_stroking(to_skia_path(path));
    auto paint = to_skia_paint(paint_style, move(image_filter), color_space());
    paint.setAntiAlias(true);
    float alpha = paint.getAlphaf();
    paint.setAlphaf(alpha * global_alpha);
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(thickness);
    paint.setStrokeCap(to_skia_cap(cap_style));
    paint.setStrokeJoin(to_skia_join(join_style));
    paint.setStrokeMiter(miter_limit);
    paint.setPathEffect(SkDashPathEffect::Make({ dash_array.data(), dash_array.size() }, dash_offset));
    draw_with_compositing_and_blending_operator(m_painting_surface->canvas(), paint, compositing_and_blending_operator, [&](SkCanvas& canvas, SkPaint const& paint) {
        canvas.drawPath(sk_path, paint);
    });
}

void PainterSkia::fill_path(Gfx::Path const& path, Gfx::Color color, Gfx::WindingRule winding_rule, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur_radius / 2));
    set_color(paint, color, color_space());
    auto sk_path = to_skia_path(path);
    sk_path.setFillType(to_skia_path_fill_type(winding_rule));
    draw_with_compositing_and_blending_operator(m_painting_surface->canvas(), paint, compositing_and_blending_operator, [&](SkCanvas& canvas, SkPaint const& paint) {
        canvas.drawPath(sk_path, paint);
    });
}

void PainterSkia::fill_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, sk_sp<SkImageFilter> image_filter, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::WindingRule winding_rule)
{
    auto sk_path = to_skia_path(path);
    sk_path.setFillType(to_skia_path_fill_type(winding_rule));
    auto paint = to_skia_paint(paint_style, move(image_filter), color_space());
    paint.setAntiAlias(true);
    float alpha = paint.getAlphaf();
    paint.setAlphaf(alpha * global_alpha);
    draw_with_compositing_and_blending_operator(m_painting_surface->canvas(), paint, compositing_and_blending_operator, [&](SkCanvas& canvas, SkPaint const& paint) {
        canvas.drawPath(sk_path, paint);
    });
}

void PainterSkia::draw_glyph_run(Font const& font, SkFont sk_font, ReadonlySpan<CanvasGlyph> glyphs, FloatPoint translation, PaintStyle const& style, sk_sp<SkImageFilter> image_filter, float global_alpha, CompositingAndBlendingOperator compositing_and_blending_operator)
{
    if (glyphs.is_empty() || font.is_invisible())
        return;

    // NB: Preserve canvas text's fractional baseline and unhinted outline geometry.
    sk_font.setHinting(SkFontHinting::kNone);
    sk_font.setForceAutoHinting(false);
    sk_font.setBaselineSnap(false);

    Vector<SkGlyphID, 32> glyph_ids;
    Vector<SkPoint, 32> glyph_positions;
    glyph_ids.ensure_capacity(glyphs.size());
    glyph_positions.ensure_capacity(glyphs.size());
    auto font_ascent = font.pixel_metrics().ascent;
    for (auto const& glyph : glyphs) {
        glyph_ids.unchecked_append(static_cast<SkGlyphID>(glyph.glyph_id));
        glyph_positions.unchecked_append({ glyph.position.x(), glyph.position.y() + font_ascent });
    }

    auto paint = to_skia_paint(style, move(image_filter), color_space());
    paint.setAlphaf(paint.getAlphaf() * global_alpha);

    // NB: Not drawTextBlob(). Skia caches a drawn SkTextBlob once per subpixel origin, and finds a cached entry by
    //     scanning every entry of that blob. Canvas text is often redrawn at moving fractional positions, which
    //     piles up thousands of entries per blob and makes each draw scan all of them.
    draw_with_compositing_and_blending_operator(m_painting_surface->canvas(), paint, compositing_and_blending_operator, [&](SkCanvas& canvas, SkPaint const& paint) {
        canvas.drawGlyphs({ glyph_ids.data(), glyph_ids.size() }, { glyph_positions.data(), glyph_positions.size() }, to_skia_point(translation), sk_font, paint);
    });
}

void PainterSkia::save()
{
    auto& canvas = m_painting_surface->canvas();
    canvas.save();
}

void PainterSkia::restore()
{
    auto& canvas = m_painting_surface->canvas();
    canvas.restore();
}

void PainterSkia::clip(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    auto sk_path = to_skia_path(path);
    sk_path.setFillType(to_skia_path_fill_type(winding_rule));
    auto& canvas = m_painting_surface->canvas();
    canvas.clipPath(sk_path, SkClipOp::kIntersect, true);
}

void PainterSkia::reset()
{
    auto& canvas = m_painting_surface->canvas();
    canvas.restoreToCount(m_initial_save_count);
    canvas.save();
}

}
